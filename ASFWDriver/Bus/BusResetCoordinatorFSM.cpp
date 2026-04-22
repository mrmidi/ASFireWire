#include "BusResetCoordinator.hpp"

#ifdef ASFW_HOST_TEST
#include <chrono>
#include <thread>
#else
#include <DriverKit/IOLib.h>
#endif

#include "../Async/Interfaces/IAsyncControllerPort.hpp"
#include "../Hardware/HardwareInterface.hpp"
#include "BusManager.hpp"
#include "Logging.hpp"
#include "TopologyManager.hpp"
#include "../ConfigROM/ROMScanner.hpp"

namespace {

constexpr uint32_t kDeferredPollMs = 1;
constexpr uint32_t kSelfIDTimeoutMs = 1000;

} // namespace

namespace ASFW::Driver {

void BusResetCoordinator::BeginNewResetCycle() {
    pendingBusResetEdge_ = false;
    selfIdLatch_.Reset();
    stopFlushIssued_ = false;
    filtersEnabled_ = false;
    atArmed_ = false;
    cycle_.ResetForNewEdge();

    if ((romScanner_ != nullptr) && (lastGeneration_.value > 0U)) {
        ++metrics_.abortCount;
        ASFW_LOG(BusReset, "Aborting ROM scan for generation %u", lastGeneration_.value);
        romScanner_->Abort(lastGeneration_);
    }

    if (topologyManager_ != nullptr) {
        topologyManager_->InvalidateForBusReset();
    }

    TransitionTo(State::Detecting, "busReset edge observed");
    MaskBusReset();
    ClearStaleSelfIDComplete2();
}

BusResetCoordinator::StepResult BusResetCoordinator::StepIdle() {
    if (HasSelfIDCompletion()) {
        HandleStraySelfID();
        if (state_ != State::Idle) {
            return StepResult::Continue;
        }
    }

    ForceUnmaskBusResetIfNeeded();

    if (cycle_.pendingReset.has_value()) {
        MaybeDispatchPendingSoftwareReset();
        return StepResult::Finish;
    }

    return StepResult::Finish;
}

BusResetCoordinator::StepResult BusResetCoordinator::StepDetecting() {
    ArmSelfIDBuffer();
    TransitionTo(State::WaitingSelfID, "Self-ID buffer armed");
    return StepResult::Continue;
}

BusResetCoordinator::StepResult BusResetCoordinator::StepWaitingSelfID() {
    if (CanAttemptSelfIDDecode()) {
        const bool decoded = DecodeSelfID();
        ClearConsumedSelfIDInterrupts();
        if (!decoded) {
            RequestSoftwareReset(
                {ResetRequestKind::Recovery, ResetFlavor::Short, std::nullopt,
                 "Self-ID decode failed"});
        }
        TransitionTo(State::QuiescingAT, decoded ? "Self-ID decoded" : "Self-ID recovery path");
        return StepResult::Continue;
    }

    const uint64_t waitedNs = MonotonicNow() - stateEntryTime_;
    if (waitedNs >= static_cast<uint64_t>(kSelfIDTimeoutMs) * 1'000'000ULL) {
        RecordRecoveryReason("Self-ID timeout");
        ClearConsumedSelfIDInterrupts();
        RequestSoftwareReset(
            {ResetRequestKind::Recovery, ResetFlavor::Short, std::nullopt, "Self-ID timeout"});
        TransitionTo(State::QuiescingAT, "Self-ID timeout");
        return StepResult::Continue;
    }

    YieldAndReschedule(kDeferredPollMs, "Waiting for Self-ID completion");
    return StepResult::Yield;
}

BusResetCoordinator::StepResult BusResetCoordinator::StepQuiescingAT() {
    if (!stopFlushIssued_) {
        StopFlushAT();
        stopFlushIssued_ = true;
    }

    if (G_ATInactive()) {
        TransitionTo(State::RestoringConfigROM, "AT contexts quiesced");
        return StepResult::Continue;
    }

    YieldAndReschedule(kDeferredPollMs, "Waiting for AT inactivity");
    return StepResult::Yield;
}

BusResetCoordinator::StepResult BusResetCoordinator::StepRestoringConfigROM() {
    RestoreConfigROM();
    BuildTopology();

    if (cycle_.acceptedTopology.has_value()) {
        EvaluateRootDelegation(*cycle_.acceptedTopology);

        bool delegationRequested = false;
        if (busManager_ != nullptr) {
            if (auto command =
                    busManager_->AssignCycleMaster(*cycle_.acceptedTopology,
                                                   topologyManager_->GetBadIRMFlags())) {
                RequestSoftwareReset({ResetRequestKind::Delegation, ResetFlavor::Long, command,
                                      "AssignCycleMaster"});
                delegationRequested = true;
            }

            if (!delegationRequested && cycle_.acceptedSelfId.has_value()) {
                if (const auto gapDecision =
                        busManager_->EvaluateGapPolicy(*cycle_.acceptedTopology,
                                                       cycle_.acceptedSelfId->quads)) {
                    BusManager::PhyConfigCommand command{};
                    command.gapCount = gapDecision->gapCount;
                    RequestSoftwareReset({ResetRequestKind::GapCorrection, ResetFlavor::Long, command,
                                          BusManager::GapDecisionReasonString(gapDecision->reason),
                                          gapDecision->reason});
                }
            }
        }
    }

    TransitionTo(State::ClearingBusReset, "Config ROM restored");
    return StepResult::Continue;
}

BusResetCoordinator::StepResult BusResetCoordinator::StepClearingBusReset() {
    if (G_ATInactive()) {
        ClearBusReset();
        UnmaskBusReset();
        TransitionTo(State::Rearming, "busReset cleared");
        return StepResult::Continue;
    }

    YieldAndReschedule(kDeferredPollMs, "Waiting for AT inactivity before clear");
    return StepResult::Yield;
}

BusResetCoordinator::StepResult BusResetCoordinator::StepRearming() {
    if (!G_NodeIDValid()) {
        YieldAndReschedule(kDeferredPollMs, "Waiting for NodeID valid");
        return StepResult::Yield;
    }

    // Per Linux bus_reset_work(): re-assert cycleMaster after bus reset.
    // The OHCI hardware may have auto-cleared it if cycleTooLong fired during
    // the reset sequence. Without cycle-start packets, devices like the Nikon
    // SAA7356HL cannot complete MCU firmware download.
    if (hardware_ != nullptr) {
        hardware_->SetLinkControlBits(LinkControlBits::kCycleMaster);
    }

    EnableFilters();
    RearmAT();

    if ((asyncSubsystem_ != nullptr) && (lastGeneration_.value <= 0xFFU)) {
        asyncSubsystem_->OnBusResetComplete(static_cast<uint8_t>(lastGeneration_.value));
    }

    TransitionTo(State::Complete, "AT contexts re-armed");
    return StepResult::Continue;
}

BusResetCoordinator::StepResult BusResetCoordinator::StepComplete() {
    LogMetrics();

    if (cycle_.pendingReset.has_value()) {
        TransitionTo(State::Idle, "awaiting deferred software reset");
        MaybeDispatchPendingSoftwareReset();
        return StepResult::Finish;
    }

    SendGlobalResumeIfNeeded();
    TransitionTo(State::Idle, "bus reset cycle complete");

    if (topologyCallback_ && cycle_.acceptedTopology.has_value() && (workQueue_.get() != nullptr)) {
        auto topo = *cycle_.acceptedTopology;
        const Discovery::Generation generation{topo.generation};

        if (previousScanHadBusyNodes_ && currentDiscoveryDelayMs_ > 0U) {
            const uint32_t delayMs = currentDiscoveryDelayMs_;
            ASFW_LOG(BusReset, "Discovery delayed %ums for generation %u", delayMs,
                     generation.value);
            workQueue_->DispatchAsync(^{
#ifdef ASFW_HOST_TEST
              std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
#else
              IOSleep(delayMs);
#endif
              if (ReadyForDiscovery(generation)) {
                  topologyCallback_(topo);
              }
            });
        } else {
            workQueue_->DispatchAsync(^{
              if (ReadyForDiscovery(generation)) {
                  topologyCallback_(topo);
              }
            });
        }
    }

    return StepResult::Finish;
}

void BusResetCoordinator::RunStateMachine() {
    if (workInProgress_.exchange(true, std::memory_order_acq_rel)) {
        ASFW_LOG_V3(BusReset, "FSM already running; coalescing request");
        return;
    }

    if (hardware_ == nullptr) {
        ForceUnmaskBusResetIfNeeded();
        CompleteCurrentRun();
        return;
    }

    constexpr int kMaxIterations = 12;
    int iteration = 0;

    while (iteration++ < kMaxIterations) {
        if (pendingBusResetEdge_) {
            BeginNewResetCycle();
        }

        const StepResult result = [this]() {
            switch (state_) {
            case State::Idle:
                return StepIdle();
            case State::Detecting:
                return StepDetecting();
            case State::WaitingSelfID:
                return StepWaitingSelfID();
            case State::QuiescingAT:
                return StepQuiescingAT();
            case State::RestoringConfigROM:
                return StepRestoringConfigROM();
            case State::ClearingBusReset:
                return StepClearingBusReset();
            case State::Rearming:
                return StepRearming();
            case State::Complete:
                return StepComplete();
            }
            return StepResult::Finish;
        }();

        if (result == StepResult::Continue) {
            continue;
        }

        CompleteCurrentRun();
        return;
    }

    YieldAndReschedule(kDeferredPollMs, "Max iteration guard");
    CompleteCurrentRun();
}

} // namespace ASFW::Driver
