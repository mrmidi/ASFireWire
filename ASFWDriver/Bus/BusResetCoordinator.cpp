#include "BusResetCoordinator.hpp"

#ifdef ASFW_HOST_TEST
#include <chrono>
#include <thread>
#else
#include <DriverKit/IOLib.h>
#endif

#include "../Async/Interfaces/IAsyncControllerPort.hpp"
#include "../ConfigROM/ConfigROMStager.hpp"
#include "../ConfigROM/ROMScanner.hpp"
#include "../Hardware/OHCIConstants.hpp"
#include "BusManager.hpp"
#include "HardwareInterface.hpp"
#include "InterruptManager.hpp"
#include "Logging.hpp"
#include "SelfIDCapture.hpp"
#include "TopologyManager.hpp"

namespace {

void LogBusResetEdgeLatched(uint64_t timestamp) {
    ASFW_LOG_V2(BusReset, "Latched busReset edge @ %llu ns", timestamp);
}

void LogSelfIDCompletionLatched(uint64_t timestamp, bool sticky) {
    if (sticky) {
        ASFW_LOG_V3(BusReset, "Latched selfIDComplete2 @ %llu ns", timestamp);
        return;
    }

    ASFW_LOG_V3(BusReset, "Latched selfIDComplete @ %llu ns", timestamp);
}

void LogDeferredRunAlreadyScheduled(const char* reason) {
    ASFW_LOG_V3(BusReset, "Deferred run already scheduled (%{public}s)",
                (reason != nullptr) ? reason : "unspecified");
}

void SleepForDelay(uint32_t delayMs) {
#ifdef ASFW_HOST_TEST
    if (delayMs > 0U) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    }
#else
    if (delayMs > 0U) {
        IOSleep(delayMs);
    }
#endif
}

void LogStateTransition(ASFW::Driver::BusResetCoordinator::State previousState,
                        ASFW::Driver::BusResetCoordinator::State nextState, const char* reason) {
    ASFW_LOG_V2(BusReset, "[FSM] %{public}s -> %{public}s: %{public}s",
                ASFW::Driver::BusResetCoordinator::StateString(previousState),
                ASFW::Driver::BusResetCoordinator::StateString(nextState), reason);
}

} // namespace

namespace ASFW::Driver {

std::atomic<uint32_t> BusResetCoordinator::nextDiagnosticsInstanceId_{1};

BusResetCoordinator::BusResetCoordinator()
    : diagnosticsInstanceId_(nextDiagnosticsInstanceId_.fetch_add(1, std::memory_order_relaxed)) {}
BusResetCoordinator::~BusResetCoordinator() = default;

void BusResetCoordinator::Initialize(HardwareInterface* hw, OSSharedPtr<IODispatchQueue> workQueue,
                                     Async::IAsyncControllerPort* asyncSys,
                                     SelfIDCapture* selfIdCapture, ConfigROMStager* configRom,
                                     InterruptManager* interrupts, TopologyManager* topology,
                                     BusManager* busManager, Discovery::ROMScanner* romScanner) {
    hardware_ = hw;
    workQueue_ = std::move(workQueue);
    asyncSubsystem_ = asyncSys;
    selfIdCapture_ = selfIdCapture;
    configRomStager_ = configRom;
    interruptManager_ = interrupts;
    topologyManager_ = topology;
    busManager_ = busManager;
    romScanner_ = romScanner;

    state_ = State::Idle;
    selfIdLatch_.Reset();
    pendingBusResetEdge_ = false;
    cycle_ = ResetCycleState{};
    delegateAttemptActive_ = false;
    delegateTarget_ = 0xFF;
    delegateRetryCount_ = 0;
    delegateSuppressed_ = false;
    stopFlushIssued_ = false;

    if (hardware_ == nullptr || workQueue_.get() == nullptr || asyncSubsystem_ == nullptr ||
        selfIdCapture_ == nullptr || configRomStager_ == nullptr || interruptManager_ == nullptr ||
        topologyManager_ == nullptr) {
        ASFW_LOG(BusReset, "ERROR: BusResetCoordinator initialized with null dependencies");
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void BusResetCoordinator::OnIrq(uint32_t intEvent, uint64_t timestamp) {
    bool relevant = false;

    if ((intEvent & IntEventBits::kBusReset) != 0U) {
        cycle_.timing.lastBusResetEdgeNs = timestamp;
        pendingBusResetEdge_ = true;
        relevant = true;
        ++busResetIrqCount_;
        LogBusResetEdgeLatched(timestamp);
    }

    if ((intEvent & IntEventBits::kSelfIDComplete) != 0U) {
        selfIdLatch_.complete = true;
        selfIdLatch_.completeTimeNs = timestamp;
        relevant = true;
        LogSelfIDCompletionLatched(timestamp, false);
    }

    if ((intEvent & IntEventBits::kSelfIDComplete2) != 0U) {
        selfIdLatch_.stickyComplete = true;
        selfIdLatch_.stickyCompleteTimeNs = timestamp;
        relevant = true;
        LogSelfIDCompletionLatched(timestamp, true);
    }

    if (!relevant || workQueue_.get() == nullptr) {
        return;
    }

    workQueue_->DispatchAsync(^{
      RunStateMachine();
    });
}

void BusResetCoordinator::BindCallbacks(TopologyReadyCallback onTopology) {
    topologyCallback_ = std::move(onTopology);
}

BusResetCoordinator::ResetDiagnostics BusResetCoordinator::Diagnostics() const {
    return ResetDiagnostics{
        .driverStartId = diagnosticsInstanceId_,
        .resetEpoch = resetEpoch_,
        .manualResetEpoch = manualResetEpoch_,
        .softwareResetIssuedCount = softwareResetIssuedCount_,
        .busResetIrqCount = busResetIrqCount_,
        .lastAcceptedGeneration = lastAcceptedGeneration_,
        .lastTopologyNodeCount = lastTopologyNodeCount_,
        .readyForDiscoveryFailureBits = readyForDiscoveryFailureBits_,
        .lastRecoveryReasonCode = lastRecoveryReasonCode_,
        .lastResetKind = static_cast<uint8_t>(lastResetKind_),
        .recoveryResetAttempts = manualRecoveryResetAttempts_,
        .discoveryCallbackCount = discoveryCallbackCount_,
    };
}

uint64_t BusResetCoordinator::MonotonicNow() {
#ifdef ASFW_HOST_TEST
    return ASFW::Testing::HostMonotonicNow();
#else
    static mach_timebase_info_data_t info{};
    if (info.denom == 0) {
        mach_timebase_info(&info);
    }
    const uint64_t ticks = mach_absolute_time();
    return ticks * info.numer / info.denom;
#endif
}

const char* BusResetCoordinator::StateString() const { return StateString(state_); }

const char* BusResetCoordinator::StateString(State state) {
    switch (state) {
    case State::Idle:
        return "Idle";
    case State::Detecting:
        return "Detecting";
    case State::WaitingSelfID:
        return "WaitingSelfID";
    case State::QuiescingAT:
        return "QuiescingAT";
    case State::RestoringConfigROM:
        return "RestoringConfigROM";
    case State::ClearingBusReset:
        return "ClearingBusReset";
    case State::Rearming:
        return "Rearming";
    case State::Complete:
        return "Complete";
    }
    return "Unknown";
}

void BusResetCoordinator::TransitionTo(State newState, const char* reason) {
    if (state_ == newState) {
        return;
    }

    const uint64_t now = MonotonicNow();

    if (newState == State::Detecting) {
        ++metrics_.resetCount;
        firstIrqTime_ = now;
    } else if (newState == State::RestoringConfigROM) {
        busResetClearTime_ = now;
    }

    LogStateTransition(state_, newState, reason);

    state_ = newState;
    stateEntryTime_ = now;
}

void BusResetCoordinator::CompleteCurrentRun() {
    workInProgress_.store(false, std::memory_order_release);
}

void BusResetCoordinator::YieldAndReschedule(uint32_t delayMs, const char* reason) {
    if (workQueue_.get() == nullptr) {
        return;
    }

    bool expected = false;
    if (!deferredRunScheduled_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        LogDeferredRunAlreadyScheduled(reason);
        return;
    }

#ifdef ASFW_HOST_TEST
    if (workQueue_->UsesManualDispatchForTesting()) {
        workQueue_->DispatchAsyncAfter(static_cast<uint64_t>(delayMs) * 1'000'000ULL, ^{
          deferredRunScheduled_.store(false, std::memory_order_release);
          RunStateMachine();
        });
        return;
    }
#endif

    workQueue_->DispatchAsync(^{
      SleepForDelay(delayMs);
      deferredRunScheduled_.store(false, std::memory_order_release);
      RunStateMachine();
    });
}

bool BusResetCoordinator::G_ATInactive() {
    if (hardware_ == nullptr) {
        return false;
    }

    // OHCI 1.1 §7.2.3.2 requires software to wait until the AT contexts are no
    // longer active before clearing `IntEvent.busReset`.
    const uint32_t atReqControl =
        hardware_->Read(Register32FromOffsetUnchecked(DMAContextHelpers::AsReqTrContextControlSet));
    const uint32_t atRspControl =
        hardware_->Read(Register32FromOffsetUnchecked(DMAContextHelpers::AsRspTrContextControlSet));

    const bool atReqActive = (atReqControl & kContextControlActiveBit) != 0U;
    const bool atRspActive = (atRspControl & kContextControlActiveBit) != 0U;
    return !atReqActive && !atRspActive;
}

bool BusResetCoordinator::HasSelfIDCompletion() const {
    return selfIdLatch_.complete || selfIdLatch_.stickyComplete;
}

bool BusResetCoordinator::CanAttemptSelfIDDecode() const {
    return G_NodeIDValid() && HasSelfIDCompletion();
}

bool BusResetCoordinator::G_NodeIDValid() const {
    if (hardware_ == nullptr) {
        return false;
    }

    const uint32_t nodeId = hardware_->Read(Register32::kNodeID);
    return ((nodeId & 0x80000000U) != 0U) && ((nodeId & 0x3FU) != 63U);
}

} // namespace ASFW::Driver
