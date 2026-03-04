#include "BusResetCoordinator.hpp"

#ifdef ASFW_HOST_TEST
#include <chrono>
#include <thread>
#else
#include <DriverKit/IOLib.h>
#endif

#include "../Async/AsyncSubsystem.hpp"
#include "../ConfigROM/ConfigROMStager.hpp"
#include "../ConfigROM/ROMScanner.hpp"
#include "../Hardware/OHCIConstants.hpp"
#include "BusManager.hpp"
#include "HardwareInterface.hpp"
#include "InterruptManager.hpp"
#include "Logging.hpp"
#include "SelfIDCapture.hpp"
#include "TopologyManager.hpp"

namespace ASFW::Driver {

void BusResetCoordinator::TransitionTo(State newState, const char* reason) {
    if (state_ == newState) {
        return;
    }

    const State previous = state_;
    const uint64_t now = MonotonicNow();

    // Increment resetCount when entering Detecting state
    if (newState == State::Detecting && previous == State::Idle) {
        metrics_.resetCount++;
        ASFW_LOG(BusReset, "Reset count: %u", metrics_.resetCount);
    }

    // Capture Reset Capsule timestamps for structured metrics logging
    if (newState == State::Detecting && previous == State::Idle) {
        firstIrqTime_ = now;
    } else if (newState == State::RestoringConfigROM) {
        busResetClearTime_ = now; // Bus reset cleared before restoration
    }

    ASFW_LOG(BusReset, "[FSM] %{public}s -> %{public}s: %{public}s", StateString(),
             StateString(newState), reason);

    state_ = newState;
    stateEntryTime_ = now;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void BusResetCoordinator::ProcessEvent(Event event) {
    if (event == Event::IrqBusReset) {
        const uint64_t now = MonotonicNow();
        const uint64_t sinceLastMs =
            (lastResetNs_ > 0 && now > lastResetNs_) ? (now - lastResetNs_) / 1'000'000 : 0;
        ASFW_LOG(BusReset,
                 "══ BUS RESET ══ gen=%u state=%{public}s sinceLastReset=%llums "
                 "prevScanBusy=%d filtersEnabled=%d atArmed=%d",
                 lastGeneration_.value, StateString(), sinceLastMs, previousScanHadBusyNodes_,
                 filtersEnabled_, atArmed_);

        if ((romScanner_ != nullptr) && (lastGeneration_.value > 0U)) {
            ASFW_LOG(BusReset, "  Aborting ROM scan for gen=%u", lastGeneration_.value);
            romScanner_->Abort(lastGeneration_);
        }

        filtersEnabled_ = false;
        atArmed_ = false;

        TransitionTo(State::Detecting, "busReset edge detected");
        A_MaskBusReset();
        A_ClearSelfID2Stale();
        selfIDComplete1_ = false;
        selfIDComplete2_ = false;
        return;
    }

    if (event == Event::IrqSelfIDComplete) {
        selfIDComplete1_ = true;
        selfIDComplete1Time_ = MonotonicNow();
        ASFW_LOG(BusReset, "[FSM] Self-ID phase 1 complete");

        if (state_ == State::Idle || state_ == State::Complete) {
            if (workQueue_.get() != nullptr) {
                workQueue_->DispatchAsync(^{
                  HandleStraySelfID();
                });
            }
        }
    }

    if (event == Event::IrqSelfIDComplete2) {
        selfIDComplete2_ = true;
        selfIDComplete2Time_ = MonotonicNow();
        ASFW_LOG(BusReset, "[FSM] Self-ID phase 2 complete");

        if (state_ == State::Idle || state_ == State::Complete) {
            if (workQueue_.get() != nullptr) {
                workQueue_->DispatchAsync(^{
                  HandleStraySelfID();
                });
            }
        }
    }

    switch (state_) {
    case State::Error:
        ASFW_LOG(BusReset, "[FSM] Error state - ignoring events");
        break;

    default:
        break;
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void BusResetCoordinator::RunStateMachine() {
    if (workInProgress_.exchange(true, std::memory_order_acq_rel)) {
        ASFW_LOG(BusReset, "FSM already running, ignoring reentrant call");
        return;
    }

    if (hardware_ == nullptr) {
        ASFW_LOG(BusReset, "RunStateMachine: hardware_ is NULL!");
        ForceUnmaskBusResetIfNeeded();
        workInProgress_.store(false, std::memory_order_release);
        return;
    }

    constexpr int kMaxIterations = 10;
    int iteration = 0;

    while (iteration++ < kMaxIterations) {
        ASFW_LOG_BUSRESET_DETAIL(
            "[FSM] RunStateMachine iteration %d: state=%{public}s selfID1=%d selfID2=%d", iteration,
            StateString(), selfIDComplete1_, selfIDComplete2_);

        switch (state_) {
        case State::Idle:
            if (selfIDComplete1_ || selfIDComplete2_) {
                ASFW_LOG(BusReset, "[FSM] Idle state - draining stray Self-ID bits");
                if (G_NodeIDValid()) {
                    A_DecodeSelfID();
                }
                A_AckSelfIDPair();
            }
            ASFW_LOG_BUSRESET_DETAIL("[FSM] Idle state - no action");
            ForceUnmaskBusResetIfNeeded();
            workInProgress_.store(false, std::memory_order_release);
            return;

        case State::Detecting:
            ASFW_LOG_BUSRESET_DETAIL("[FSM] Detecting state - arming Self-ID buffer");
            if (selfIdCapture_ != nullptr) {
                A_ArmSelfIDBuffer();
            }
            TransitionTo(State::WaitingSelfID, "Self-ID buffer armed");
            continue;

        case State::WaitingSelfID:
            ASFW_LOG_BUSRESET_DETAIL(
                "[FSM] WaitingSelfID state - checking guards: selfID1=%d selfID2=%d",
                selfIDComplete1_, selfIDComplete2_);

            if (G_HaveSelfIDPair()) {
                if (selfIDComplete1Time_ == 0) {
                    selfIDComplete1Time_ = MonotonicNow();
                }
                A_DecodeSelfID();
                A_AckSelfIDPair();
                TransitionTo(State::QuiescingAT, "Self-ID pair received + acked");
                continue;
            }

            if (G_NodeIDValid()) {
                if (!selfIDComplete2_) {
                    selfIDComplete2_ = true;
                    selfIDComplete2Time_ = MonotonicNow();
                    ASFW_LOG_BUSRESET_DETAIL("[FSM] Self-ID phase 2 synthesized via NodeID valid");
                }
                if (selfIDComplete1Time_ == 0) {
                    selfIDComplete1Time_ = MonotonicNow();
                }
                A_DecodeSelfID();
                A_AckSelfIDPair();
                TransitionTo(State::QuiescingAT, "NodeID valid + acked — proceed");
                continue;
            }

            if ((selfIDComplete1_ || selfIDComplete2_) &&
                (MonotonicNow() - stateEntryTime_) > 2'000'000) {
                ASFW_LOG_BUSRESET_DETAIL("[FSM] Single-bit grace path: complete1=%d complete2=%d",
                                         selfIDComplete1_, selfIDComplete2_);
                A_AckSelfIDPair();
                TransitionTo(State::QuiescingAT, "Self-ID single-bit grace path + acked");
                continue;
            }

            ASFW_LOG_BUSRESET_DETAIL("[FSM] WaitingSelfID - no guard satisfied, waiting...");
            workInProgress_.store(false, std::memory_order_release);
            return;

        case State::QuiescingAT:
            ASFW_LOG_BUSRESET_DETAIL("[FSM] QuiescingAT state - stopping AT contexts");

            A_StopFlushAT();

            if (G_ATInactive()) {
                ASFW_LOG_BUSRESET_DETAIL(
                    "[FSM] AT contexts inactive - continuing to ConfigROM restore");
                TransitionTo(State::RestoringConfigROM, "AT contexts quiesced");
                continue;
            }

            ASFW_LOG_BUSRESET_DETAIL("[FSM] AT contexts still active - rescheduling");
            ScheduleDeferredRun(1, "AT contexts active during QuiescingAT");
            workInProgress_.store(false, std::memory_order_release);
            return;

        case State::RestoringConfigROM:
            ASFW_LOG_BUSRESET_DETAIL("[FSM] RestoringConfigROM state");

            if (configRomStager_ != nullptr) {
                A_RestoreConfigROM();
            }
            A_BuildTopology();
            if (lastTopology_.has_value()) {
                EvaluateRootDelegation(*lastTopology_);
            }

            ASFW_LOG(BusReset, "🔍 BusManager check: busManager_=%p lastTopology_=%d (gen=%u)",
                     busManager_, lastTopology_.has_value(),
                     lastTopology_.has_value() ? lastTopology_->generation : 0xFF);
            if ((busManager_ != nullptr) && lastTopology_.has_value()) {
                if (auto phyCmd = busManager_->AssignCycleMaster(
                        *lastTopology_, topologyManager_->GetBadIRMFlags())) {
                    StageDelayedPhyPacket(*phyCmd, "AssignCycleMaster");
                }

                if (pendingManagedReset_) {
                    ASFW_LOG(
                        BusReset,
                        "[FSM] BusManager staged PHY packet; will trigger reset after completion");
                }
            }

            TransitionTo(State::ClearingBusReset,
                         "Config ROM restored + topology built + bus managed");
            continue;

        case State::ClearingBusReset:
            ASFW_LOG_BUSRESET_DETAIL("[FSM] ClearingBusReset state - checking AT inactive");

            if (G_ATInactive()) {
                A_ClearBusReset();
                A_UnmaskBusReset();

                TransitionTo(State::Rearming, "busReset cleared & re-enabled");
                continue;
            }

            ASFW_LOG_BUSRESET_DETAIL("[FSM] ClearingBusReset - AT still active, waiting");
            ScheduleDeferredRun(1, "AT contexts active during ClearingBusReset");
            workInProgress_.store(false, std::memory_order_release);
            return;

        case State::Rearming:
            ASFW_LOG_BUSRESET_DETAIL("[FSM] Rearming state - verifying NodeID valid before AT.run");

            if (!G_NodeIDValid()) {
                ASFW_LOG_BUSRESET_DETAIL("[FSM] Rearming - NodeID not valid yet, rescheduling");
                ScheduleDeferredRun(1, "Waiting for NodeID valid");
                workInProgress_.store(false, std::memory_order_release);
                return;
            }

            A_EnableFilters();
            A_RearmAT();

            if ((asyncSubsystem_ != nullptr) && (lastGeneration_.value != 0xFFU)) {
                asyncSubsystem_->OnBusResetComplete(
                    static_cast<uint8_t>(lastGeneration_.value & 0xFFU));
            }

            TransitionTo(State::Complete, "AT contexts re-armed (NodeID valid)");
            continue;

        case State::Complete:
            ASFW_LOG_BUSRESET_DETAIL("[FSM] Complete state - finalizing bus reset cycle");

            A_MetricsLog();

            if (pendingManagedReset_ && pendingPhyCommand_.has_value()) {
                ASFW_LOG(BusReset, "Dispatching staged PHY packet (reason=%s)",
                         pendingPhyReason_.c_str());
                if (DispatchPendingPhyPacket()) {
                    workInProgress_.store(false, std::memory_order_release);
                    return;
                }

                ASFW_LOG(BusReset, "⚠️  Failed to dispatch staged PHY packet - clearing request");
                pendingManagedReset_ = false;
                pendingPhyCommand_.reset();
                pendingPhyReason_.clear();
            }

            if (!pendingManagedReset_) {
                A_SendGlobalResumeIfNeeded();
            }

            TransitionTo(State::Idle, "bus reset cycle complete");

            if (topologyCallback_ && lastTopology_.has_value() && (workQueue_.get() != nullptr)) {
                auto topo = *lastTopology_;
                const Discovery::Generation gen{topo.generation};

                if (previousScanHadBusyNodes_ && currentDiscoveryDelayMs_ > 0) {
                    // DICE/Saffire-class devices: delay discovery to let firmware
                    // finish booting before we start a new scan.  The generation
                    // staleness check inside the callback prevents acting on a
                    // stale topology if another bus reset occurs during the delay.
                    // Delay escalates with consecutive failures (2s→4s→6s→8s→10s).
                    const uint32_t delayMs = currentDiscoveryDelayMs_;
                    ASFW_LOG(BusReset, "Discovery delayed %ums for gen=%u (ack_busy in prev scan)",
                             delayMs, gen.value);
                    workQueue_->DispatchAsync(^{
#ifdef ASFW_HOST_TEST
                      std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
#else
                            IOSleep(delayMs);
#endif
                      if (ReadyForDiscovery(gen)) {
                          uint8_t localNode = topo.localNodeId.value_or(0xFF);
                          ASFW_LOG(BusReset, "Discovery start gen=%u local=%u (after %ums delay)",
                                   gen.value, localNode, delayMs);
                          topologyCallback_(topo);
                      } else {
                          ASFW_LOG(BusReset, "Discovery cancelled gen=%u (stale after delay)",
                                   gen.value);
                      }
                    });
                } else {
                    ASFW_LOG(BusReset, "Post-reset hooks scheduled for gen=%u", gen.value);
                    workQueue_->DispatchAsync(^{
                      if (ReadyForDiscovery(gen)) {
                          uint8_t localNode = topo.localNodeId.value_or(0xFF);
                          ASFW_LOG(BusReset, "Discovery start gen=%u local=%u", gen.value,
                                   localNode);
                          topologyCallback_(topo);
                      } else {
                          ASFW_LOG(BusReset, "Discovery deferred gen=%u", gen.value);
                      }
                    });
                }
            }

            continue;

        case State::Error:
            ASFW_LOG_BUSRESET_DETAIL("[FSM] Error state - terminal");
            ForceUnmaskBusResetIfNeeded();
            workInProgress_.store(false, std::memory_order_release);
            return;
        }

        ASFW_LOG(BusReset, "[FSM] WARNING: State %d fell through", static_cast<int>(state_));
        ForceUnmaskBusResetIfNeeded();
        workInProgress_.store(false, std::memory_order_release);
        return;
    }

    ASFW_LOG(BusReset, "[FSM] Max iterations (%u) reached in state %d - rescheduling",
             kMaxIterations, static_cast<int>(state_));
    ForceUnmaskBusResetIfNeeded();
    ScheduleDeferredRun(1, "max iteration guard");
    workInProgress_.store(false, std::memory_order_release);
}

} // namespace ASFW::Driver

