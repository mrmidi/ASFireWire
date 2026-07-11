// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "DiceDuplexRestartCoordinator.hpp"
#include "DiceRecoveryPolicy.hpp"
#include "DuplexStreamProfile.hpp"
#include "RestartJournal.hpp"
#include "SyncAsyncBridge.hpp"

#include "../../../Logging/Logging.hpp"
#include "../../Core/AudioRuntimeRegistry.hpp"
#include "../../Model/ASFWAudioDevice.hpp"
#include "../DICE/Core/IDICEDuplexProtocol.hpp"

#include <DriverKit/IOLib.h>

#include <atomic>
#include <memory>
#include <utility>

namespace ASFW::Audio {

// FW-66: the recovery-policy classification vocabulary now lives in DiceRecoveryPolicy.hpp.
using namespace ASFW::Audio::Backends;

namespace {

using ASFW::Audio::DICE::ClearRestartProgress;
using ASFW::Audio::DICE::DiceClockRequestCompletion;
using ASFW::Audio::DICE::DiceClockRequestOutcome;
using ASFW::Audio::DICE::DiceDesiredClockConfig;
using ASFW::Audio::DICE::DiceDuplexConfirmResult;
using ASFW::Audio::DICE::DiceDuplexHealthResult;
using ASFW::Audio::DICE::DiceDuplexPrepareResult;
using ASFW::Audio::DICE::DiceDuplexStageResult;
using ASFW::Audio::DICE::DiceRestartErrorClass;
using ASFW::Audio::DICE::DiceRestartFailureCause;
using ASFW::Audio::DICE::DiceRestartIssueInfo;
using ASFW::Audio::DICE::DiceRestartPhase;
using ASFW::Audio::DICE::DiceRestartReason;
using ASFW::Audio::DICE::DiceRestartSession;
using ASFW::Audio::DICE::DiceRestartState;
using ASFW::Audio::DICE::HasAnyRestartState;
using ASFW::Audio::DICE::HasDeviceRestartState;
using ASFW::Audio::DICE::HasHostRestartState;
using ASFW::Audio::DICE::HasRestartIntent;
using ASFW::Audio::DICE::IsSupportedClockConfig;
using ASFW::Audio::DICE::kDiceClockSelect48kInternal;

constexpr uint32_t kClockRequestWaitTimeoutMs = 15000;

[[nodiscard]] uint64_t UptimeMilliseconds() noexcept {
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.denom == 0) {
        return 0;
    }
    const unsigned __int128 nanos =
        static_cast<unsigned __int128>(mach_absolute_time()) * timebase.numer / timebase.denom;
    return static_cast<uint64_t>(nanos / 1'000'000U);
}

inline uint8_t ReadLocalSid(Driver::HardwareInterface& hw) noexcept {
    return static_cast<uint8_t>(hw.ReadNodeID() & 0x3Fu);
}

} // namespace

// FW-70: the execution body of a duplex start/stop/idle-clock transaction.  The
// coordinator remains the FSM entry-point and owns the gate/store, while this
// runner owns the ordered stage sequence.  FW-71 will replace the temporary
// DICE-named dependency types with their protocol-neutral seams.
// Device stream programming before GLOBAL_ENABLE is cross-validated with Linux
// dice-stream.c:326-374 and dice-interface.h:120-125; no source code is copied.
class DuplexStartTransaction final {
public:
    struct StartRequest {
        uint64_t guid;
        Discovery::DeviceRecord& record;
        DICE::IDICEDuplexProtocol& deviceControl;
        DiceRestartSession& session;
        const DiceDesiredClockConfig& desiredClock;
        DiceRestartReason reason;
    };

    struct StopRequest {
        uint64_t guid;
        Discovery::DeviceRecord& record;
        DICE::IDICEDuplexProtocol& deviceControl;
        DiceRestartSession& session;
    };

    struct IdleClockApplyRequest {
        uint64_t guid;
        DICE::IDICEDuplexProtocol& deviceControl;
        DiceRestartSession& session;
        FW::Generation topologyGeneration;
        const DiceDesiredClockConfig& desiredClock;
        DiceRestartReason reason;
    };

    explicit DuplexStartTransaction(DiceDuplexRestartCoordinator& coordinator) noexcept
        : coordinator_(coordinator) {}

    [[nodiscard]] IOReturn Run(const StartRequest& request) noexcept;
    [[nodiscard]] IOReturn Stop(const StopRequest& request) noexcept;
    [[nodiscard]] IOReturn ApplyIdleClock(const IdleClockApplyRequest& request) noexcept;

private:
    [[nodiscard]] IOReturn WaitForStableGlobalClock(
        uint64_t guid,
        DICE::IDICEDuplexProtocol& deviceControl,
        FW::Generation topologyGeneration,
        const DiceDesiredClockConfig& desiredClock) noexcept;

    DiceDuplexRestartCoordinator& coordinator_;
};

DiceDuplexRestartCoordinator::DiceDuplexRestartCoordinator(
    Discovery::DeviceRegistry& registry, AudioRuntimeRegistry& runtime,
    IDiceHostTransport& hostTransport, Driver::HardwareInterface& hardware,
    const std::atomic<bool>* cancel,
    DirectAudioBindingSourceProvider bindingSourceProvider) noexcept
    : registry_(registry), runtime_(runtime), hostTransport_(hostTransport), hardware_(hardware),
      cancel_(cancel), bindingSourceProvider_(std::move(bindingSourceProvider)) {
    lock_ = IOLockAlloc();
    if (!lock_) {
        ASFW_LOG_ERROR(Audio, "DiceDuplexRestartCoordinator: failed to allocate lock");
        return;
    }
}

DiceDuplexRestartCoordinator::~DiceDuplexRestartCoordinator() noexcept {
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

IOReturn DiceDuplexRestartCoordinator::StartStreaming(uint64_t guid) noexcept {
    if (guid == 0) {
        return kIOReturnBadArgument;
    }
    if (TeardownRequested()) {
        ASFW_LOG(Audio,
                 "DiceDuplexRestartCoordinator: StartStreaming refused by teardown GUID=%llx",
                 guid);
        return kIOReturnAborted;
    }

    const DiceRestartSession session = LoadSession(guid);
    LogFsmEvent("start", guid, session.restartId, session.topologyGeneration, session.state,
                session.phase, session.reason);

    bool acquired = false;
    for (uint32_t waited = 0; waited < kSyncBridgeTimeoutMs; waited += kSyncBridgePollMs) {
        if (TryAcquireGuid(guid)) {
            acquired = true;
            break;
        }
        if (TeardownRequested()) {
            return kIOReturnAborted;
        }
        IOSleep(kSyncBridgePollMs);
    }
    if (!acquired) {
        ASFW_LOG_ERROR(Audio,
                       "DiceDuplexRestartCoordinator: StartStreaming timed out waiting for GUID "
                       "claim GUID=%llx timeoutMs=%u",
                       guid, kSyncBridgeTimeoutMs);
        return kIOReturnTimeout;
    }

    const IOReturn status = RunStartStreaming(guid);
    ReleaseGuid(guid);
    return status;
}

IOReturn DiceDuplexRestartCoordinator::StopStreaming(uint64_t guid) noexcept {
    if (guid == 0) {
        return kIOReturnBadArgument;
    }
    if (TeardownRequested()) {
        ASFW_LOG(Audio, "DiceDuplexRestartCoordinator: StopStreaming refused by teardown GUID=%llx",
                 guid);
        return kIOReturnAborted;
    }

    const DiceRestartSession session = LoadSession(guid);
    LogFsmEvent("stop", guid, session.restartId, session.topologyGeneration, session.state,
                session.phase, session.reason);

    RequestStopIntent(guid);
    FailPendingClockRequest(guid, DiceClockRequestOutcome::kAbortedByStop, kIOReturnAborted);

    bool acquired = false;
    for (uint32_t waited = 0; waited < kSyncBridgeTimeoutMs; waited += kSyncBridgePollMs) {
        if (TryAcquireGuid(guid)) {
            acquired = true;
            break;
        }
        if (TeardownRequested()) {
            ClearStopIntent(guid);
            return kIOReturnAborted;
        }
        IOSleep(kSyncBridgePollMs);
    }
    if (!acquired) {
        ASFW_LOG_ERROR(Audio,
                       "DiceDuplexRestartCoordinator: StopStreaming timed out waiting for GUID "
                       "claim GUID=%llx timeoutMs=%u",
                       guid, kSyncBridgeTimeoutMs);
        ClearStopIntent(guid);
        return kIOReturnTimeout;
    }

    const IOReturn status = RunStopStreaming(guid);
    FailPendingClockRequest(guid, DiceClockRequestOutcome::kAbortedByStop, kIOReturnAborted);
    ClearStopIntent(guid);
    ReleaseGuid(guid);
    return status;
}

IOReturn DiceDuplexRestartCoordinator::RequestClockConfig(
    uint64_t guid, const DiceDesiredClockConfig& desiredClock, DiceRestartReason reason) noexcept {
    if (guid == 0) {
        return kIOReturnBadArgument;
    }
    if (TeardownRequested()) {
        ASFW_LOG(Audio,
                 "DiceDuplexRestartCoordinator: RequestClockConfig refused by teardown GUID=%llx",
                 guid);
        return kIOReturnAborted;
    }
    if (!IsSupportedClockConfig(desiredClock)) {
        return kIOReturnUnsupported;
    }

    PendingClockRequest request{
        .desiredClock = desiredClock,
        .reason = reason,
    };

    bool shouldLaunchLoop = false;
    std::optional<PendingClockRequest> supersededRequest{};
    DiceRestartSession session = LoadSession(guid);
    if (!lock_) {
        return kIOReturnNoResources;
    }

    IOLockLock(lock_);
    if (gate_.IsStopRequestedLocked(guid)) {
        IOLockUnlock(lock_);
        return kIOReturnAborted;
    }

    request.token = clockRequests_.AllocateTokenLocked();
    if (gate_.IsActiveLocked(guid)) {
        supersededRequest = clockRequests_.QueuePendingLocked(guid, request);
    } else {
        gate_.AcquireLocked(guid);
        shouldLaunchLoop = true;
    }
    IOLockUnlock(lock_);

    session.pendingClock = request.desiredClock;
    session.pendingReason = request.reason;
    LogFsmEvent("clock", guid, session.restartId, session.topologyGeneration, session.state,
                session.phase, reason, request.token);

    if (supersededRequest.has_value()) {
        CompleteClockRequest(
            DiceClockRequestCompletion{
                .token = supersededRequest->token,
                .desiredClock = supersededRequest->desiredClock,
                .reason = supersededRequest->reason,
                .outcome = DiceClockRequestOutcome::kSuperseded,
                .status = kIOReturnAborted,
                .restartId = session.restartId,
                .generation = session.topologyGeneration,
            },
            guid);
    }

    if (shouldLaunchLoop) {
        (void)RunClockRequestLoop(guid, request);
        ReleaseGuid(guid);
    }

    for (uint32_t waited = 0; waited < kClockRequestWaitTimeoutMs; waited += kSyncBridgePollMs) {
        DiceClockRequestCompletion completion{};
        if (TryTakeCompletedClockRequest(guid, request.token, completion)) {
            return completion.status;
        }
        if (TeardownRequested()) {
            return kIOReturnAborted;
        }

        IOSleep(kSyncBridgePollMs);
    }

    return kIOReturnTimeout;
}

IOReturn DiceDuplexRestartCoordinator::RecoverStreaming(uint64_t guid,
                                                        DiceRestartReason reason) noexcept {
    if (guid == 0) {
        return kIOReturnBadArgument;
    }
    if (TeardownRequested()) {
        ASFW_LOG(Audio,
                 "DiceDuplexRestartCoordinator: RecoverStreaming refused by teardown GUID=%llx",
                 guid);
        return kIOReturnAborted;
    }

    const DiceRestartSession session = LoadSession(guid);
    LogFsmEvent("recover", guid, session.restartId, session.topologyGeneration, session.state,
                session.phase, reason);

    FailPendingClockRequest(guid, DiceClockRequestOutcome::kAbortedByStop, kIOReturnAborted);

    bool acquired = false;
    for (uint32_t waited = 0; waited < kSyncBridgeTimeoutMs; waited += kSyncBridgePollMs) {
        if (TryAcquireGuid(guid)) {
            acquired = true;
            break;
        }
        if (TeardownRequested()) {
            return kIOReturnAborted;
        }
        IOSleep(kSyncBridgePollMs);
    }
    if (!acquired) {
        ASFW_LOG_ERROR(Audio,
                       "DiceDuplexRestartCoordinator: RecoverStreaming timed out waiting for GUID "
                       "claim GUID=%llx timeoutMs=%u",
                       guid, kSyncBridgeTimeoutMs);
        return kIOReturnTimeout;
    }

    const IOReturn status = RunRecoveryStreaming(guid, reason);
    ReleaseGuid(guid);
    return status;
}

void DiceDuplexRestartCoordinator::ClearSession(uint64_t guid) noexcept {
    if (!lock_ || guid == 0) {
        return;
    }

    IOLockLock(lock_);
    store_.EraseSessionLocked(guid);
    clockRequests_.ClearLocked(guid);
    gate_.ReleaseLocked(guid);
    gate_.ClearStopLocked(guid);
    IOLockUnlock(lock_);
}

std::optional<DiceRestartSession>
DiceDuplexRestartCoordinator::GetSession(uint64_t guid) const noexcept {
    return store_.GetSession(guid);
}

IOReturn DiceDuplexRestartCoordinator::RunStartStreaming(uint64_t guid) noexcept {
    if (TeardownRequested()) {
        return kIOReturnAborted;
    }

    DICE::IDICEDuplexProtocol* diceProtocol = nullptr;
    std::shared_ptr<IDeviceProtocol> protoHold; // keeps the protocol alive for this op
    auto* record = RequireDiceRecord(guid, diceProtocol, protoHold);
    if (!record || !diceProtocol) {
        FailPendingClockRequest(guid, DiceClockRequestOutcome::kFailed, kIOReturnNotReady);
        return kIOReturnNotReady;
    }

    DiceRestartSession session = LoadSession(guid);
    const DiceDesiredClockConfig desiredClock{
        .sampleRateHz = 48000U,
        .clockSelect = kDiceClockSelect48kInternal,
    };
    const DiceRestartReason reason = DICE::HasRestartIntent(session)
                                         ? DICE::ClassifyRestartReason(&session, desiredClock)
                                         : DiceRestartReason::kInitialStart;

    const IOReturn status =
        RunDuplexStart(guid, *record, *diceProtocol, session, desiredClock, reason);
    if (status != kIOReturnSuccess) {
        FailPendingClockRequest(guid, DiceClockRequestOutcome::kFailed, status);
        return status;
    }

    if (IsStopRequested(guid)) {
        FailPendingClockRequest(guid, DiceClockRequestOutcome::kAbortedByStop, kIOReturnAborted);
        return kIOReturnSuccess;
    }

    PendingClockRequest pending{};
    while (TryConsumePendingClockRequest(guid, pending)) {
        if (IsStopRequested(guid)) {
            const DiceRestartSession completionSession = LoadSession(guid);
            CompleteClockRequest(
                DiceClockRequestCompletion{
                    .token = pending.token,
                    .desiredClock = pending.desiredClock,
                    .reason = pending.reason,
                    .outcome = DiceClockRequestOutcome::kAbortedByStop,
                    .status = kIOReturnAborted,
                    .restartId = completionSession.restartId,
                    .generation = completionSession.topologyGeneration,
                },
                guid);
            FailPendingClockRequest(guid, DiceClockRequestOutcome::kAbortedByStop,
                                    kIOReturnAborted);
            break;
        }

        const IOReturn pendingStatus = ApplyClockRequest(guid, pending);
        if (IsStopRequested(guid)) {
            const DiceRestartSession completionSession = LoadSession(guid);
            CompleteClockRequest(
                DiceClockRequestCompletion{
                    .token = pending.token,
                    .desiredClock = pending.desiredClock,
                    .reason = pending.reason,
                    .outcome = DiceClockRequestOutcome::kAbortedByStop,
                    .status = kIOReturnAborted,
                    .restartId = completionSession.restartId,
                    .generation = completionSession.topologyGeneration,
                },
                guid);
            FailPendingClockRequest(guid, DiceClockRequestOutcome::kAbortedByStop,
                                    kIOReturnAborted);
            break;
        }

        const DiceRestartSession completionSession = LoadSession(guid);
        CompleteClockRequest(
            DiceClockRequestCompletion{
                .token = pending.token,
                .desiredClock = pending.desiredClock,
                .reason = pending.reason,
                .outcome = (pendingStatus == kIOReturnSuccess) ? DiceClockRequestOutcome::kApplied
                                                               : DiceClockRequestOutcome::kFailed,
                .status = pendingStatus,
                .restartId = completionSession.restartId,
                .generation = completionSession.topologyGeneration,
            },
            guid);
    }

    return kIOReturnSuccess;
}

IOReturn DiceDuplexRestartCoordinator::RunStopStreaming(uint64_t guid) noexcept {
    if (TeardownRequested()) {
        return kIOReturnAborted;
    }

    DICE::IDICEDuplexProtocol* diceProtocol = nullptr;
    std::shared_ptr<IDeviceProtocol> protoHold; // keeps the protocol alive for this op
    auto* record = RequireDiceRecord(guid, diceProtocol, protoHold);
    if (!record || !diceProtocol) {
        DiceRestartSession session = LoadSession(guid);
        ClearFailureSnapshot(session);
        ClearRestartProgress(session);
        StoreSession(session);
        LogTerminal(session);
        return kIOReturnNotReady;
    }

    DiceRestartSession session = LoadSession(guid);
    return RunDuplexStop(guid, *record, *diceProtocol, session);
}

IOReturn DiceDuplexRestartCoordinator::RunRecoveryStreaming(uint64_t guid,
                                                            DiceRestartReason reason) noexcept {
    if (TeardownRequested()) {
        return kIOReturnAborted;
    }

    DICE::IDICEDuplexProtocol* diceProtocol = nullptr;
    std::shared_ptr<IDeviceProtocol> protoHold; // keeps the protocol alive for this op
    auto* record = RequireDiceRecord(guid, diceProtocol, protoHold);
    DiceRestartSession session = LoadSession(guid);
    session.guid = guid;
    if (IsRecoveryReason(reason)) {
        RecordIssue(session, session.lastInvalidation, session.phase,
                    DiceRestartErrorClass::kEpochInvalidated, FailureCauseForReason(reason),
                    kIOReturnAborted, true, false, kIOReturnSuccess, true, true);
        StoreSession(session);
        LogInvalidation(session);
    }

    const DiceRecoveryContext context{
        .triggerReason = reason,
        .state = session.state,
        .phase = session.phase,
        .stopRequested = IsStopRequested(guid),
        .hasRestartIntent = HasRestartIntent(session),
        .hasHostFootprint = HasHostRestartState(session),
        .hasDeviceFootprint = HasDeviceRestartState(session),
        .hasDiceRecord = (record != nullptr),
        .hasProtocol = (diceProtocol != nullptr),
        .lastFailureRetryable = session.lastFailure.has_value() && session.lastFailure->retryable,
    };
    const DiceRecoveryDecision decision = EvaluateRecoveryPolicy(context);
    LogRecoveryPolicy(session, reason, decision);

    if (decision.disposition == DiceRecoveryDisposition::kIgnore) {
        return (decision.reason == DiceRecoveryPolicyReason::kSuppressedByStop ||
                decision.reason == DiceRecoveryPolicyReason::kIdleApplyInvalidated)
                   ? kIOReturnAborted
                   : kIOReturnSuccess;
    }

    if (decision.disposition == DiceRecoveryDisposition::kFailSession) {
        const bool missingDependency = (!record || !diceProtocol);
        session.terminalError = missingDependency
                                    ? kIOReturnNotReady
                                    : (session.lastFailure.has_value() ? session.lastFailure->status
                                                                       : kIOReturnUnsupported);
        if (missingDependency) {
            RecordIssue(session, session.lastFailure, session.phase,
                        DiceRestartErrorClass::kMissingDependency, FailureCauseForReason(reason),
                        session.terminalError, false, false, kIOReturnSuccess, false, false);
        }
        ApplyTerminalPhase(session, DiceRestartPhase::kFailed, ToString(decision.reason));
        StoreSession(session);
        LogTerminal(session);
        return session.terminalError;
    }

    if (!record || !diceProtocol) {
        return kIOReturnNotReady;
    }

    const DiceDesiredClockConfig desiredClock =
        (session.desiredClock.sampleRateHz != 0 && session.desiredClock.clockSelect != 0)
            ? session.desiredClock
            : ((session.appliedClock.sampleRateHz != 0 && session.appliedClock.clockSelect != 0)
                   ? session.appliedClock
                   : DiceDesiredClockConfig{
                         .sampleRateHz = 48000U,
                         .clockSelect = kDiceClockSelect48kInternal,
                     });

    if (HasAnyRestartState(session) || session.phase == DiceRestartPhase::kRunning) {
        if (TeardownRequested()) {
            return kIOReturnAborted;
        }
        const IOReturn stopStatus = RunDuplexStop(guid, *record, *diceProtocol, session);
        if (stopStatus != kIOReturnSuccess) {
            return stopStatus;
        }
    }

    if (IsStopRequested(guid) || TeardownRequested()) {
        return kIOReturnAborted;
    }

    return RunDuplexStart(guid, *record, *diceProtocol, session, desiredClock, reason);
}

IOReturn
DiceDuplexRestartCoordinator::RunClockRequestLoop(uint64_t guid,
                                                  PendingClockRequest initialRequest) noexcept {
    PendingClockRequest current = initialRequest;
    IOReturn lastStatus = kIOReturnSuccess;

    while (true) {
        if (IsStopRequested(guid) || TeardownRequested()) {
            const DiceRestartSession completionSession = LoadSession(guid);
            CompleteClockRequest(
                DiceClockRequestCompletion{
                    .token = current.token,
                    .desiredClock = current.desiredClock,
                    .reason = current.reason,
                    .outcome = DiceClockRequestOutcome::kAbortedByStop,
                    .status = kIOReturnAborted,
                    .restartId = completionSession.restartId,
                    .generation = completionSession.topologyGeneration,
                },
                guid);
            FailPendingClockRequest(guid, DiceClockRequestOutcome::kAbortedByStop,
                                    kIOReturnAborted);
            break;
        }

        lastStatus = ApplyClockRequest(guid, current);

        if (IsStopRequested(guid) || TeardownRequested()) {
            const DiceRestartSession completionSession = LoadSession(guid);
            CompleteClockRequest(
                DiceClockRequestCompletion{
                    .token = current.token,
                    .desiredClock = current.desiredClock,
                    .reason = current.reason,
                    .outcome = DiceClockRequestOutcome::kAbortedByStop,
                    .status = kIOReturnAborted,
                    .restartId = completionSession.restartId,
                    .generation = completionSession.topologyGeneration,
                },
                guid);
            FailPendingClockRequest(guid, DiceClockRequestOutcome::kAbortedByStop,
                                    kIOReturnAborted);
            break;
        }

        const DiceRestartSession completionSession = LoadSession(guid);
        CompleteClockRequest(
            DiceClockRequestCompletion{
                .token = current.token,
                .desiredClock = current.desiredClock,
                .reason = current.reason,
                .outcome = (lastStatus == kIOReturnSuccess) ? DiceClockRequestOutcome::kApplied
                                                            : DiceClockRequestOutcome::kFailed,
                .status = lastStatus,
                .restartId = completionSession.restartId,
                .generation = completionSession.topologyGeneration,
            },
            guid);

        PendingClockRequest next{};
        if (!TryConsumePendingClockRequest(guid, next)) {
            break;
        }
        current = next;
    }

    return lastStatus;
}

IOReturn
DiceDuplexRestartCoordinator::ApplyClockRequest(uint64_t guid,
                                                const PendingClockRequest& request) noexcept {
    if (IsStopRequested(guid) || TeardownRequested()) {
        return kIOReturnAborted;
    }

    DICE::IDICEDuplexProtocol* diceProtocol = nullptr;
    std::shared_ptr<IDeviceProtocol> protoHold; // keeps the protocol alive for this op
    auto* record = RequireDiceRecord(guid, diceProtocol, protoHold);
    if (!record || !diceProtocol) {
        return kIOReturnNotReady;
    }
    if (!IsSupportedClockConfig(request.desiredClock)) {
        return kIOReturnUnsupported;
    }

    DiceRestartSession session = LoadSession(guid);
    if (HasAnyRestartState(session) || session.phase == DiceRestartPhase::kRunning ||
        session.state == DiceRestartState::kRunning ||
        session.state == DiceRestartState::kRecovering ||
        session.state == DiceRestartState::kFailed) {
        if (TeardownRequested()) {
            return kIOReturnAborted;
        }
        const IOReturn stopStatus = RunDuplexStop(guid, *record, *diceProtocol, session);
        if (stopStatus != kIOReturnSuccess) {
            return stopStatus;
        }
        if (TeardownRequested()) {
            return kIOReturnAborted;
        }
        return RunDuplexStart(guid, *record, *diceProtocol, session, request.desiredClock,
                              request.reason);
    }

    if (TeardownRequested()) {
        return kIOReturnAborted;
    }

    return RunIdleClockApply(guid, *diceProtocol, session, record->gen, request.desiredClock,
                             request.reason);
}

IOReturn DuplexStartTransaction::Run(const StartRequest& request) noexcept {
    const uint64_t guid = request.guid;
    Discovery::DeviceRecord& record = request.record;
    DICE::IDICEDuplexProtocol& diceProtocol = request.deviceControl;
    DiceRestartSession& session = request.session;
    const DiceDesiredClockConfig& desiredClock = request.desiredClock;
    const DiceRestartReason reason = request.reason;
    auto& runtime_ = coordinator_.runtime_;
    auto& hostTransport_ = coordinator_.hostTransport_;
    auto& hardware_ = coordinator_.hardware_;
    const std::atomic<bool>* const cancel_ = coordinator_.cancel_;
    constexpr uint32_t kSyncBridgeTimeoutMs = DiceDuplexRestartCoordinator::kSyncBridgeTimeoutMs;

    auto TeardownRequested = [this]() noexcept { return coordinator_.TeardownRequested(); };
    auto RecordTeardownAbort = [this](const char* stage, uint64_t abortGuid) noexcept {
        coordinator_.RecordTeardownAbort(stage, abortGuid);
    };
    auto AllocateRestartId = [this]() noexcept { return coordinator_.AllocateRestartId(); };
    auto StoreSession = [this](const DiceRestartSession& updatedSession) noexcept {
        coordinator_.StoreSession(updatedSession);
    };
    auto IsStopRequested = [this](uint64_t requestGuid) noexcept {
        return coordinator_.IsStopRequested(requestGuid);
    };
    auto IsRestartEpochCurrent = [this](uint64_t requestGuid, uint64_t restartId,
                                        FW::Generation generation) noexcept {
        return coordinator_.IsRestartEpochCurrent(requestGuid, restartId, generation);
    };
    auto GetDirectAudioBindingSource = [this](uint64_t requestGuid) noexcept {
        return coordinator_.GetDirectAudioBindingSource(requestGuid);
    };
    auto RunDuplexStop = [this](uint64_t stopGuid, Discovery::DeviceRecord& stopRecord,
                                DICE::IDICEDuplexProtocol& stopDevice,
                                DiceRestartSession& stopSession) noexcept {
        return Stop(StopRequest{stopGuid, stopRecord, stopDevice, stopSession});
    };

    auto abortIfTeardown = [&](const char* stage) noexcept -> bool {
        if (!TeardownRequested()) {
            return false;
        }
        RecordTeardownAbort(stage, guid);
        return true;
    };

    if (abortIfTeardown("RunDuplexStart")) {
        return kIOReturnAborted;
    }
    const FW::Generation topologyGeneration = record.gen;
    auto runtimeProtocol = runtime_.FindShared(record.guid);

    // Pre-read the device's static stream format (DICE TX_NUMBER/RX_NUMBER +
    // per-stream channels) so the channel resolution + IRM reservation below see
    // the real stream count. EnsureRuntimeStreamGeometry publishes the per-stream
    // caps that DuplexStreamProfileResolver consumes via GetRuntimeAudioStreamCaps.
    // Non-fatal: on failure we fall back to the legacy single-stream resolution
    // and PrepareDuplex will surface any genuine device error. A multi-stream
    // device (Venice F32 = 2×16) needs this to allocate a channel per stream;
    // cross-validated with FFADO dice_avdevice.cpp prepare() (m_nb_rx/m_nb_tx).
    const auto geometryLoad = WaitForAsyncResult<bool>(
        [&](auto callback) {
            diceProtocol.EnsureRuntimeStreamGeometry(
                [callback = std::move(callback)](IOReturn st) mutable { callback(st, true); });
        },
        kSyncBridgeTimeoutMs, kIOReturnTimeout, cancel_);
    if (geometryLoad.status != kIOReturnSuccess) {
        ASFW_LOG(DICE,
                 "RunDuplexStart: stream-geometry pre-read failed (0x%x); "
                 "resolving channels with existing caps",
                 geometryLoad.status);
    }

    const DuplexStreamProfile initialProfile =
        DuplexStreamProfileResolver::Resolve(record, runtimeProtocol.get());
    const AudioDuplexChannels channels = initialProfile.channels;
    const uint64_t restartId = AllocateRestartId();

    auto finalizeFailure = [&](IOReturn failureStatus, DiceRestartPhase failedPhase,
                               DiceRestartFailureCause cause, DiceRestartErrorClass errorClass,
                               bool rollbackAttempted, IOReturn rollbackStatus, bool hostStateKnown,
                               bool deviceStateKnown) noexcept {
        session.guid = guid;
        session.restartId = restartId;
        session.generation = topologyGeneration;
        session.topologyGeneration = topologyGeneration;
        session.channels = channels;
        session.reason = reason;
        session.desiredClock = desiredClock;
        session.terminalError = failureStatus;
        RecordIssue(session, session.lastFailure, failedPhase, errorClass, cause, failureStatus,
                    IsRetryableStatus(failureStatus), rollbackAttempted, rollbackStatus,
                    hostStateKnown, deviceStateKnown);
        ApplyTerminalPhase(session, DiceRestartPhase::kFailed, ToString(errorClass));
        StoreSession(session);
        LogTerminal(session);
        return failureStatus;
    };

    auto rollbackToFailure = [&](IOReturn failureStatus, DiceRestartPhase failedPhase,
                                 DiceRestartFailureCause cause) noexcept {
        if (failureStatus == kIOReturnAborted && TeardownRequested()) {
            return failureStatus;
        }
        const IOReturn rollbackStatus = RunDuplexStop(guid, record, diceProtocol, session);
        return finalizeFailure(failureStatus, failedPhase, cause,
                               DiceRestartErrorClass::kStageFailure, true, rollbackStatus, true,
                               true);
    };

    auto rollbackToInvalidation = [&](IOReturn invalidationStatus, DiceRestartPhase failedPhase,
                                      DiceRestartFailureCause cause) noexcept {
        if (invalidationStatus == kIOReturnAborted && TeardownRequested()) {
            return invalidationStatus;
        }
        const IOReturn rollbackStatus = RunDuplexStop(guid, record, diceProtocol, session);
        if (rollbackStatus != kIOReturnSuccess) {
            return finalizeFailure(
                rollbackStatus, DiceRestartPhase::kStopping, DiceRestartFailureCause::kStop,
                DiceRestartErrorClass::kStageFailure, true, rollbackStatus, true, true);
        }

        session.guid = guid;
        session.restartId = restartId;
        session.generation = topologyGeneration;
        session.topologyGeneration = topologyGeneration;
        session.channels = channels;
        session.reason = reason;
        session.desiredClock = desiredClock;
        session.terminalError = kIOReturnSuccess;
        RecordIssue(session, session.lastInvalidation, failedPhase,
                    IsStopRequested(guid) ? DiceRestartErrorClass::kStopIntent
                                          : DiceRestartErrorClass::kEpochInvalidated,
                    cause, invalidationStatus, true, true, rollbackStatus, true, true);
        ClearFailureSnapshot(session);
        ApplyTerminalPhase(session, DiceRestartPhase::kIdle,
                           ToString(session.lastInvalidation->errorClass));
        StoreSession(session);
        LogInvalidation(session);
        LogTerminal(session);
        return invalidationStatus;
    };

    auto* irmClient = diceProtocol.GetIRMClient();
    if (irmClient == nullptr) {
        ASFW_LOG_ERROR(Audio, "DiceDuplexRestartCoordinator: protocol missing IRM client GUID=%llx",
                       guid);
        return finalizeFailure(kIOReturnNotReady, DiceRestartPhase::kPreparingDevice,
                               DiceRestartFailureCause::kPrepare,
                               DiceRestartErrorClass::kMissingDependency, false, kIOReturnSuccess,
                               false, false);
    }

    auto* bindingSource = GetDirectAudioBindingSource(guid);
    if (!bindingSource) {
        return finalizeFailure(kIOReturnNotReady, DiceRestartPhase::kPreparingDevice,
                               DiceRestartFailureCause::kPrepare,
                               DiceRestartErrorClass::kMissingDependency, false, kIOReturnSuccess,
                               false, true);
    }

    session.guid = guid;
    session.restartId = restartId;
    session.generation = topologyGeneration;
    session.topologyGeneration = topologyGeneration;
    session.channels = channels;
    session.reason = reason;
    session.desiredClock = desiredClock;
    session.terminalError = kIOReturnSuccess;
    ApplyTerminalPhase(session, DiceRestartPhase::kIdle, "reset_before_start");
    SetSessionState(session, RestartStateForStartReason(reason), ToString(reason));
    SetSessionPhase(session, DiceRestartPhase::kPreparingDevice);
    StoreSession(session);
    LogFsmEvent("start", guid, restartId, topologyGeneration, session.state, session.phase, reason);
    ASFW_LOG(Audio, "DiceDuplexRestartCoordinator: using DICE iso channels d2h=%u h2d=%u GUID=%llx",
             channels.deviceToHostIsoChannel, channels.hostToDeviceIsoChannel, guid);

    if (abortIfTeardown("BeginSplitDuplex")) {
        return kIOReturnAborted;
    }
    const kern_return_t claimStatus = hostTransport_.BeginSplitDuplex(guid);
    if (claimStatus != kIOReturnSuccess) {
        return finalizeFailure(
            claimStatus, DiceRestartPhase::kPreparingDevice, DiceRestartFailureCause::kPrepare,
            DiceRestartErrorClass::kStageFailure, false, kIOReturnSuccess, true, true);
    }
    session.hostDuplexClaimed = true;
    StoreSession(session);

    if (abortIfTeardown("PreparingDevice")) {
        return kIOReturnAborted;
    }
    const auto prepare = WaitForAsyncResult<DiceDuplexPrepareResult>(
        [&](auto callback) {
            diceProtocol.PrepareDuplex(channels, desiredClock, std::move(callback));
        },
        kSyncBridgeTimeoutMs, kIOReturnTimeout, cancel_);
    if (prepare.status != kIOReturnSuccess) {
        if (prepare.status == kIOReturnAborted && TeardownRequested()) {
            RecordTeardownAbort("PreparingDevice", guid);
        }
        return rollbackToFailure(prepare.status, DiceRestartPhase::kPreparingDevice,
                                 DiceRestartFailureCause::kPrepare);
    }
    if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
        return rollbackToInvalidation(kIOReturnAborted, DiceRestartPhase::kPreparingDevice,
                                      DiceRestartFailureCause::kPrepare);
    }
    session.ownerClaimed = true;
    session.devicePrepared = true;
    session.generation = prepare.value.generation;
    session.appliedClock = prepare.value.appliedClock;
    session.runtimeCaps = prepare.value.runtimeCaps;
    const DuplexStreamProfile streamProfile =
        DuplexStreamProfileResolver::Resolve(record, session.runtimeCaps, channels);
    SetSessionPhase(session, DiceRestartPhase::kPrepared);
    StoreSession(session);

    // Reserve IRM bandwidth + channel for EVERY playback (host IT -> device RX)
    // stream. A multi-stream DICE device (Venice F32) needs all of its RX iso
    // channels reserved before GLOBAL_ENABLE; single-stream devices loop once.
    if (abortIfTeardown("ReservingPlaybackResources")) {
        return kIOReturnAborted;
    }
    for (uint32_t i = 0; i < channels.playbackStreamCount; ++i) {
        const kern_return_t reservePlaybackStatus = hostTransport_.ReservePlaybackResources(
            guid, *irmClient, channels.PlaybackChannel(i), streamProfile.playbackBandwidthUnits);
        if (reservePlaybackStatus != kIOReturnSuccess) {
            return rollbackToFailure(reservePlaybackStatus,
                                     DiceRestartPhase::kReservingPlaybackResources,
                                     DiceRestartFailureCause::kReservePlayback);
        }
        if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
            return rollbackToInvalidation(kIOReturnAborted,
                                          DiceRestartPhase::kReservingPlaybackResources,
                                          DiceRestartFailureCause::kReservePlayback);
        }
    }
    SetSessionPhase(session, DiceRestartPhase::kReservingPlaybackResources);
    session.hostPlaybackReserved = true;
    StoreSession(session);

    // Reserve IRM bandwidth + channel for EVERY capture (device TX -> host IR) stream.
    if (abortIfTeardown("ReservingCaptureResources")) {
        return kIOReturnAborted;
    }
    for (uint32_t i = 0; i < channels.captureStreamCount; ++i) {
        const kern_return_t reserveCaptureStatus = hostTransport_.ReserveCaptureResources(
            guid, *irmClient, channels.CaptureChannel(i), streamProfile.captureBandwidthUnits);
        if (reserveCaptureStatus != kIOReturnSuccess) {
            return rollbackToFailure(reserveCaptureStatus,
                                     DiceRestartPhase::kReservingCaptureResources,
                                     DiceRestartFailureCause::kReserveCapture);
        }
        if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
            return rollbackToInvalidation(kIOReturnAborted,
                                          DiceRestartPhase::kReservingCaptureResources,
                                          DiceRestartFailureCause::kReserveCapture);
        }
    }
    SetSessionPhase(session, DiceRestartPhase::kReservingCaptureResources);
    session.hostCaptureReserved = true;
    StoreSession(session);

    ASFW_LOG(Audio,
             "DICE DUPLEX START guid=0x%016llx ir=%u it=%u inCh=%u outCh=%u inSlots=%u outSlots=%u "
             "mode=blocking rxFmt=%u txFmt=%u",
             guid, channels.deviceToHostIsoChannel, channels.hostToDeviceIsoChannel,
             session.runtimeCaps.hostInputPcmChannels, session.runtimeCaps.hostOutputPcmChannels,
             session.runtimeCaps.deviceToHostAm824Slots, session.runtimeCaps.hostToDeviceAm824Slots,
             static_cast<uint32_t>(streamProfile.captureWireFormat),
             static_cast<uint32_t>(streamProfile.playbackWireFormat));

    // Saffire.kext allocates every local/remote isoch port before it reports
    // the assigned channels to DICE. Keep the expensive DMA setup on the
    // disabled side of GLOBAL_ENABLE as well.
    // Multi-stream capture (e.g. Venice F32 = 2×16) is described by the
    // profile's per-stream AM824/PCM geometry. The master owns clock/ZTS/replay;
    // secondaries write their PCM slice only. A single stream retains the legacy
    // full-width (streamChannels == 0) host receive path.
    const DuplexCaptureStreamGeometry& masterCapture = streamProfile.captureStreams[0];

    for (const DuplexHostDirection direction : streamProfile.startOrder.prepareOrder) {
        if (direction == DuplexHostDirection::kReceive) {
            if (abortIfTeardown("PreparingHostReceive")) {
                return kIOReturnAborted;
            }
            const kern_return_t prepareReceiveStatus =
                hostTransport_.PrepareReceive(channels.CaptureChannel(0), hardware_, bindingSource,
                                              streamProfile.captureWireFormat,
                                              masterCapture.am824Slots, masterCapture.pcmChannels);
            if (prepareReceiveStatus != kIOReturnSuccess) {
                return rollbackToFailure(prepareReceiveStatus,
                                         DiceRestartPhase::kStartingHostReceive,
                                         DiceRestartFailureCause::kStartReceive);
            }
            if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
                return rollbackToInvalidation(kIOReturnAborted,
                                              DiceRestartPhase::kStartingHostReceive,
                                              DiceRestartFailureCause::kStartReceive);
            }

            // Prepare each secondary capture stream on its own OHCI IR context, writing
            // its slice at the running channel offset.
            for (uint32_t i = 1; i < channels.captureStreamCount; ++i) {
                const DuplexCaptureStreamGeometry& captureStream = streamProfile.captureStreams[i];
                if (abortIfTeardown("PreparingHostReceiveStream")) {
                    return kIOReturnAborted;
                }
                const kern_return_t status = hostTransport_.PrepareReceiveStream(
                    i, channels.CaptureChannel(i), hardware_, bindingSource,
                    captureStream.pcmChannelOffset, captureStream.pcmChannels,
                    streamProfile.captureWireFormat, captureStream.am824Slots);
                if (status != kIOReturnSuccess) {
                    return rollbackToFailure(status, DiceRestartPhase::kStartingHostReceive,
                                             DiceRestartFailureCause::kStartReceive);
                }
                if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
                    return rollbackToInvalidation(kIOReturnAborted,
                                                  DiceRestartPhase::kStartingHostReceive,
                                                  DiceRestartFailureCause::kStartReceive);
                }
            }

            continue;
        }

        if (abortIfTeardown("PreparingHostTransmit")) {
            return kIOReturnAborted;
        }
        const kern_return_t prepareTransmitStatus = hostTransport_.PrepareTransmit(
            channels.hostToDeviceIsoChannel, hardware_, ReadLocalSid(hardware_));
        if (prepareTransmitStatus != kIOReturnSuccess) {
            return rollbackToFailure(prepareTransmitStatus, DiceRestartPhase::kStartingHostTransmit,
                                     DiceRestartFailureCause::kStartTransmit);
        }
        if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
            return rollbackToInvalidation(kIOReturnAborted, DiceRestartPhase::kStartingHostTransmit,
                                          DiceRestartFailureCause::kStartTransmit);
        }

        // Prepare each secondary playback stream on its own host IT context. It
        // transmits on the iso channel that feeds the device's matching RX stream
        // (PlaybackChannel(i)); the host IT ring stamps that channel into every
        // packet header. The audio engine maps the secondary's shared slab (already
        // allocated by StartIO) and writes its 16-ch slice.
        for (uint32_t i = 1; i < channels.playbackStreamCount; ++i) {
            if (abortIfTeardown("PreparingHostTransmitStream")) {
                return kIOReturnAborted;
            }
            const kern_return_t status = hostTransport_.PrepareTransmitStream(
                i, channels.PlaybackChannel(i), hardware_, ReadLocalSid(hardware_));
            if (status != kIOReturnSuccess) {
                return rollbackToFailure(status, DiceRestartPhase::kStartingHostTransmit,
                                         DiceRestartFailureCause::kStartTransmit);
            }
            if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
                return rollbackToInvalidation(kIOReturnAborted,
                                              DiceRestartPhase::kStartingHostTransmit,
                                              DiceRestartFailureCause::kStartTransmit);
            }
        }
    }

    SetSessionPhase(session, DiceRestartPhase::kWaitingGlobalClock);
    StoreSession(session);
    if (abortIfTeardown("WaitingGlobalClock")) {
        return kIOReturnAborted;
    }
    const IOReturn clockLockStatus =
        WaitForStableGlobalClock(guid, diceProtocol, topologyGeneration, desiredClock);
    if (clockLockStatus != kIOReturnSuccess) {
        return rollbackToFailure(clockLockStatus, DiceRestartPhase::kWaitingGlobalClock,
                                 DiceRestartFailureCause::kGlobalClockLock);
    }
    if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
        return rollbackToInvalidation(kIOReturnAborted, DiceRestartPhase::kWaitingGlobalClock,
                                      DiceRestartFailureCause::kGlobalClockLock);
    }

    if (abortIfTeardown("ProgrammingDeviceRx")) {
        return kIOReturnAborted;
    }
    const auto programRx = WaitForAsyncResult<DiceDuplexStageResult>(
        [&](auto callback) { diceProtocol.ProgramRx(std::move(callback)); }, kSyncBridgeTimeoutMs,
        kIOReturnTimeout, cancel_);
    if (programRx.status != kIOReturnSuccess) {
        if (programRx.status == kIOReturnAborted && TeardownRequested()) {
            RecordTeardownAbort("ProgrammingDeviceRx", guid);
        }
        return rollbackToFailure(programRx.status, DiceRestartPhase::kProgrammingDeviceRx,
                                 DiceRestartFailureCause::kProgramRx);
    }
    if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
        return rollbackToInvalidation(kIOReturnAborted, DiceRestartPhase::kProgrammingDeviceRx,
                                      DiceRestartFailureCause::kProgramRx);
    }
    session.generation = programRx.value.generation;
    SetSessionPhase(session, DiceRestartPhase::kProgrammingDeviceRx);
    session.deviceRxProgrammed = true;
    session.runtimeCaps = programRx.value.runtimeCaps.hostInputPcmChannels != 0
                              ? programRx.value.runtimeCaps
                              : session.runtimeCaps;
    SetSessionPhase(session, DiceRestartPhase::kDeviceRxProgrammed);
    StoreSession(session);

    if (abortIfTeardown("ProgrammingDeviceTx")) {
        return kIOReturnAborted;
    }
    const auto programTx = WaitForAsyncResult<DiceDuplexStageResult>(
        [&](auto callback) { diceProtocol.ProgramTxAndEnableDuplex(std::move(callback)); },
        kSyncBridgeTimeoutMs, kIOReturnTimeout, cancel_);
    if (programTx.status != kIOReturnSuccess) {
        if (programTx.status == kIOReturnAborted && TeardownRequested()) {
            RecordTeardownAbort("ProgrammingDeviceTx", guid);
        }
        return rollbackToFailure(programTx.status, DiceRestartPhase::kProgrammingDeviceTx,
                                 DiceRestartFailureCause::kProgramTx);
    }
    if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
        return rollbackToInvalidation(kIOReturnAborted, DiceRestartPhase::kProgrammingDeviceTx,
                                      DiceRestartFailureCause::kProgramTx);
    }
    session.generation = programTx.value.generation;
    SetSessionPhase(session, DiceRestartPhase::kProgrammingDeviceTx);
    session.deviceTxArmed = true;
    session.runtimeCaps = programTx.value.runtimeCaps.hostInputPcmChannels != 0
                              ? programTx.value.runtimeCaps
                              : session.runtimeCaps;
    SetSessionPhase(session, DiceRestartPhase::kDeviceTxArmed);
    StoreSession(session);

    // The DICE recipe waits after GLOBAL_ENABLE before starting
    // the already-allocated host isoch channels.
    IOSleep(streamProfile.startOrder.postDeviceEnableDelayMs);
    if (abortIfTeardown("PostGlobalEnableDelay")) {
        return kIOReturnAborted;
    }

    // The profile's DICE recipe starts RX first: the device needs to be
    // receiving before it can lock its TX clock. Starting IT before IR causes
    // the DICE PLL to see TX without a valid RX reference, leading to timing
    // instability.
    for (const DuplexHostDirection direction : streamProfile.startOrder.startOrder) {
        if (direction == DuplexHostDirection::kReceive) {
            if (abortIfTeardown("StartingHostReceive")) {
                return kIOReturnAborted;
            }
            const kern_return_t startReceiveStatus = hostTransport_.StartPreparedReceive();
            if (startReceiveStatus != kIOReturnSuccess) {
                return rollbackToFailure(startReceiveStatus, DiceRestartPhase::kStartingHostReceive,
                                         DiceRestartFailureCause::kStartReceive);
            }
            if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
                return rollbackToInvalidation(kIOReturnAborted,
                                              DiceRestartPhase::kStartingHostReceive,
                                              DiceRestartFailureCause::kStartReceive);
            }
            SetSessionPhase(session, DiceRestartPhase::kStartingHostReceive);
            session.hostReceiveStarted = true;
            StoreSession(session);

            continue;
        }

        if (abortIfTeardown("StartingHostTransmit")) {
            return kIOReturnAborted;
        }
        const kern_return_t startTransmitStatus = hostTransport_.StartPreparedTransmit();
        if (startTransmitStatus != kIOReturnSuccess) {
            return rollbackToFailure(startTransmitStatus, DiceRestartPhase::kStartingHostTransmit,
                                     DiceRestartFailureCause::kStartTransmit);
        }
        if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
            return rollbackToInvalidation(kIOReturnAborted, DiceRestartPhase::kStartingHostTransmit,
                                          DiceRestartFailureCause::kStartTransmit);
        }
        SetSessionPhase(session, DiceRestartPhase::kStartingHostTransmit);
        session.hostTransmitStarted = true;
        StoreSession(session);
    }

    if (abortIfTeardown("ConfirmingDeviceStart")) {
    return kIOReturnAborted;
}
const auto confirm = WaitForAsyncResult<DiceDuplexConfirmResult>(
    [&](auto callback) { diceProtocol.ConfirmDuplexStart(std::move(callback)); },
    kSyncBridgeTimeoutMs, kIOReturnTimeout, cancel_);
if (confirm.status != kIOReturnSuccess) {
    if (confirm.status == kIOReturnAborted && TeardownRequested()) {
        RecordTeardownAbort("ConfirmingDeviceStart", guid);
    }
    return rollbackToFailure(confirm.status, DiceRestartPhase::kConfirmingDeviceStart,
                             DiceRestartFailureCause::kConfirmStart);
}
if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
    return rollbackToInvalidation(kIOReturnAborted, DiceRestartPhase::kConfirmingDeviceStart,
                                  DiceRestartFailureCause::kConfirmStart);
}

SetSessionPhase(session, DiceRestartPhase::kConfirmingDeviceStart);
SetSessionPhase(session, DiceRestartPhase::kRunning);
SetSessionState(session, DiceRestartState::kRunning, "confirmed_running");
session.generation = confirm.value.generation;
session.deviceRunning = true;
session.appliedClock = confirm.value.appliedClock;
session.runtimeCaps = confirm.value.runtimeCaps;
session.terminalError = kIOReturnSuccess;
ClearFailureSnapshot(session);
StoreSession(session);
LogTerminal(session);
return kIOReturnSuccess;
}

IOReturn DuplexStartTransaction::WaitForStableGlobalClock(
    uint64_t guid, DICE::IDICEDuplexProtocol& diceProtocol, const FW::Generation topologyGeneration,
    const DiceDesiredClockConfig& desiredClock) noexcept {
    const std::atomic<bool>* const cancel_ = coordinator_.cancel_;
    constexpr uint32_t kGlobalClockLockTimeoutMs =
        DiceDuplexRestartCoordinator::kGlobalClockLockTimeoutMs;
    constexpr uint32_t kGlobalClockLockPollMs =
        DiceDuplexRestartCoordinator::kGlobalClockLockPollMs;
    constexpr uint32_t kGlobalClockStableReads =
        DiceDuplexRestartCoordinator::kGlobalClockStableReads;
    auto TeardownRequested = [this]() noexcept { return coordinator_.TeardownRequested(); };
    auto RecordTeardownAbort = [this](const char* stage, uint64_t abortGuid) noexcept {
        coordinator_.RecordTeardownAbort(stage, abortGuid);
    };

    uint32_t consecutiveLockedReads = 0;
    const uint64_t startMs = UptimeMilliseconds();
    const uint64_t deadlineMs = startMs + kGlobalClockLockTimeoutMs;

    while (UptimeMilliseconds() < deadlineMs) {
        if (TeardownRequested()) {
            RecordTeardownAbort("WaitingGlobalClock", guid);
            return kIOReturnAborted;
        }

        const uint64_t nowMs = UptimeMilliseconds();
        const uint32_t remainingMs = static_cast<uint32_t>(deadlineMs - nowMs);
        const auto health = WaitForAsyncResult<DiceDuplexHealthResult>(
            [&](auto callback) { diceProtocol.ReadDuplexHealth(std::move(callback)); },
            std::max(remainingMs, 1U), kIOReturnTimeout, cancel_);
        if (health.status != kIOReturnSuccess) {
            if (health.status == kIOReturnAborted && TeardownRequested()) {
                RecordTeardownAbort("WaitingGlobalClock", guid);
                return health.status;
            }
            ASFW_LOG_ERROR(Audio, "DICE global clock health read failed before isoch start kr=0x%x",
                           health.status);
            return health.status;
        }
        if (health.value.generation != topologyGeneration) {
            ASFW_LOG_ERROR(
                Audio,
                "DICE global clock generation changed before isoch start expected=%u actual=%u",
                GenerationValue(topologyGeneration), GenerationValue(health.value.generation));
            return kIOReturnOffline;
        }

        const bool lockedAtTarget =
            DICE::IsSourceLocked(health.value.status) &&
            DICE::NominalRateHz(health.value.status) == desiredClock.sampleRateHz &&
            health.value.appliedClock.sampleRateHz == desiredClock.sampleRateHz &&
            health.value.appliedClock.clockSelect == desiredClock.clockSelect;
        consecutiveLockedReads = lockedAtTarget ? consecutiveLockedReads + 1 : 0;

        if (consecutiveLockedReads >= kGlobalClockStableReads) {
            ASFW_LOG(Audio,
                     "DICE global clock stable before isoch start rate=%u status=0x%08x reads=%u",
                     desiredClock.sampleRateHz, health.value.status, consecutiveLockedReads);
            return kIOReturnSuccess;
        }

        const uint64_t afterReadMs = UptimeMilliseconds();
        if (afterReadMs >= deadlineMs) {
            break;
        }
        if (TeardownRequested()) {
            RecordTeardownAbort("WaitingGlobalClock", guid);
            return kIOReturnAborted;
        }
        IOSleep(std::min<uint64_t>(kGlobalClockLockPollMs, deadlineMs - afterReadMs));
    }

    ASFW_LOG_ERROR(Audio,
                   "DICE global clock failed to stabilize before isoch start rate=%u timeoutMs=%u",
                   desiredClock.sampleRateHz, kGlobalClockLockTimeoutMs);
    return kIOReturnTimeout;
}

IOReturn DuplexStartTransaction::Stop(const StopRequest& request) noexcept {
    const uint64_t guid = request.guid;
    Discovery::DeviceRecord& record = request.record;
    DICE::IDICEDuplexProtocol& diceProtocol = request.deviceControl;
    DiceRestartSession& session = request.session;
    auto& hostTransport_ = coordinator_.hostTransport_;
    auto TeardownRequested = [this]() noexcept { return coordinator_.TeardownRequested(); };
    auto RecordTeardownAbort = [this](const char* stage, uint64_t abortGuid) noexcept {
        coordinator_.RecordTeardownAbort(stage, abortGuid);
    };
    auto StoreSession = [this](const DiceRestartSession& updatedSession) noexcept {
        coordinator_.StoreSession(updatedSession);
    };

    (void)record;

    if (TeardownRequested()) {
        RecordTeardownAbort("RunDuplexStop", guid);
        return kIOReturnAborted;
    }

    IOReturn result = kIOReturnSuccess;
    SetSessionPhase(session, DiceRestartPhase::kStopping);
    SetSessionState(session, DiceRestartState::kStopping, "stop_requested");
    StoreSession(session);

    const kern_return_t hostStatus = hostTransport_.StopAll();
    if (hostStatus != kIOReturnSuccess) {
        result = hostStatus;
    }

    const IOReturn deviceStatus = diceProtocol.StopDuplex();
    if (deviceStatus == kIOReturnAborted && TeardownRequested()) {
        RecordTeardownAbort("DeviceStop", guid);
        return kIOReturnAborted;
    }
    if (deviceStatus != kIOReturnSuccess && deviceStatus != kIOReturnUnsupported &&
        result == kIOReturnSuccess) {
        result = deviceStatus;
    }

    if (result == kIOReturnSuccess) {
        session.terminalError = kIOReturnSuccess;
        ClearFailureSnapshot(session);
        ApplyTerminalPhase(session, DiceRestartPhase::kIdle, "stop_complete");
    } else {
        session.terminalError = result;
        RecordIssue(session, session.lastFailure, DiceRestartPhase::kStopping,
                    DiceRestartErrorClass::kStageFailure, DiceRestartFailureCause::kStop, result,
                    IsRetryableStatus(result), false, kIOReturnSuccess, true, true);
        ApplyTerminalPhase(session, DiceRestartPhase::kFailed, "stop_failed");
    }
    StoreSession(session);
    LogTerminal(session);
    return result;
}

IOReturn DuplexStartTransaction::ApplyIdleClock(const IdleClockApplyRequest& request) noexcept {
    const uint64_t guid = request.guid;
    DICE::IDICEDuplexProtocol& diceProtocol = request.deviceControl;
    DiceRestartSession& session = request.session;
    const FW::Generation topologyGeneration = request.topologyGeneration;
    const DiceDesiredClockConfig& desiredClock = request.desiredClock;
    const DiceRestartReason reason = request.reason;
    const std::atomic<bool>* const cancel_ = coordinator_.cancel_;
    constexpr uint32_t kSyncBridgeTimeoutMs = DiceDuplexRestartCoordinator::kSyncBridgeTimeoutMs;
    auto TeardownRequested = [this]() noexcept { return coordinator_.TeardownRequested(); };
    auto RecordTeardownAbort = [this](const char* stage, uint64_t abortGuid) noexcept {
        coordinator_.RecordTeardownAbort(stage, abortGuid);
    };
    auto AllocateRestartId = [this]() noexcept { return coordinator_.AllocateRestartId(); };
    auto StoreSession = [this](const DiceRestartSession& updatedSession) noexcept {
        coordinator_.StoreSession(updatedSession);
    };
    auto IsStopRequested = [this](uint64_t requestGuid) noexcept {
        return coordinator_.IsStopRequested(requestGuid);
    };
    auto IsRestartEpochCurrent = [this](uint64_t requestGuid, uint64_t restartId,
                                        FW::Generation generation) noexcept {
        return coordinator_.IsRestartEpochCurrent(requestGuid, restartId, generation);
    };

    if (TeardownRequested()) {
        RecordTeardownAbort("IdleClockApply", guid);
        return kIOReturnAborted;
    }

    const uint64_t restartId = AllocateRestartId();
    session.guid = guid;
    session.restartId = restartId;
    session.generation = topologyGeneration;
    session.topologyGeneration = topologyGeneration;
    session.reason = reason;
    session.desiredClock = desiredClock;
    session.terminalError = kIOReturnSuccess;
    ApplyTerminalPhase(session, DiceRestartPhase::kIdle, "reset_before_idle_apply");
    SetSessionState(session, DiceRestartState::kApplyingIdleClock, ToString(reason));
    SetSessionPhase(session, DiceRestartPhase::kPreparingDevice);
    StoreSession(session);

    const auto apply = WaitForAsyncResult<DICE::DiceClockApplyResult>(
        [&](auto callback) { diceProtocol.ApplyClockConfig(desiredClock, std::move(callback)); },
        kSyncBridgeTimeoutMs, kIOReturnTimeout, cancel_);
    if (apply.status != kIOReturnSuccess) {
        if (apply.status == kIOReturnAborted && TeardownRequested()) {
            RecordTeardownAbort("IdleClockApply", guid);
        }
        session.terminalError = apply.status;
        RecordIssue(session, session.lastFailure, DiceRestartPhase::kPreparingDevice,
                    DiceRestartErrorClass::kStageFailure, DiceRestartFailureCause::kIdleClockApply,
                    apply.status, IsRetryableStatus(apply.status), false, kIOReturnSuccess, true,
                    true);
        ApplyTerminalPhase(session, DiceRestartPhase::kFailed, "idle_apply_failed");
        StoreSession(session);
        LogTerminal(session);
        return apply.status;
    }
    if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
        session.terminalError = kIOReturnSuccess;
        RecordIssue(session, session.lastInvalidation, DiceRestartPhase::kPreparingDevice,
                    IsStopRequested(guid) ? DiceRestartErrorClass::kStopIntent
                                          : DiceRestartErrorClass::kEpochInvalidated,
                    DiceRestartFailureCause::kIdleClockApply, kIOReturnAborted, true, false,
                    kIOReturnSuccess, true, true);
        ClearFailureSnapshot(session);
        ApplyTerminalPhase(session, DiceRestartPhase::kIdle, "idle_apply_invalidated");
        StoreSession(session);
        LogInvalidation(session);
        LogTerminal(session);
        return kIOReturnAborted;
    }

    session.generation = apply.value.generation;
    session.appliedClock = apply.value.appliedClock;
    session.runtimeCaps = apply.value.runtimeCaps;
    session.terminalError = kIOReturnSuccess;
    ClearFailureSnapshot(session);
    ApplyTerminalPhase(session, DiceRestartPhase::kIdle, "idle_apply_complete");
    StoreSession(session);
    LogTerminal(session);
    return kIOReturnSuccess;
}

IOReturn DiceDuplexRestartCoordinator::RunDuplexStart(
    uint64_t guid, Discovery::DeviceRecord& record, DICE::IDICEDuplexProtocol& diceProtocol,
    DiceRestartSession& session, const DiceDesiredClockConfig& desiredClock,
    DiceRestartReason reason) noexcept {
    return DuplexStartTransaction{*this}.Run(
        DuplexStartTransaction::StartRequest{guid, record, diceProtocol, session, desiredClock,
                                             reason});
}

IOReturn DiceDuplexRestartCoordinator::RunDuplexStop(
    uint64_t guid, Discovery::DeviceRecord& record, DICE::IDICEDuplexProtocol& diceProtocol,
    DiceRestartSession& session) noexcept {
    return DuplexStartTransaction{*this}.Stop(
        DuplexStartTransaction::StopRequest{guid, record, diceProtocol, session});
}

IOReturn DiceDuplexRestartCoordinator::RunIdleClockApply(
    uint64_t guid, DICE::IDICEDuplexProtocol& diceProtocol, DiceRestartSession& session,
    FW::Generation topologyGeneration, const DiceDesiredClockConfig& desiredClock,
    DiceRestartReason reason) noexcept {
    return DuplexStartTransaction{*this}.ApplyIdleClock(
        DuplexStartTransaction::IdleClockApplyRequest{guid, diceProtocol, session,
                                                       topologyGeneration, desiredClock, reason});
}

Discovery::DeviceRecord* DiceDuplexRestartCoordinator::RequireDiceRecord(
    uint64_t guid, DICE::IDICEDuplexProtocol*& outDiceProtocol,
    std::shared_ptr<IDeviceProtocol>& outHold) noexcept {
    outDiceProtocol = nullptr;
    outHold.reset();
    auto* record = registry_.FindByGuid(guid);
    if (!record) {
        return nullptr;
    }

    auto protocol = runtime_.FindShared(guid);
    if (!protocol) {
        return nullptr;
    }

    outDiceProtocol = protocol->AsDiceDuplexProtocol();
    if (outDiceProtocol == nullptr) {
        return nullptr;
    }
    outDiceProtocol->SetTeardownCancelToken(cancel_);

    outHold = std::move(protocol);
    return record;
}

ASFW::Audio::Runtime::IDirectAudioBindingSource*
DiceDuplexRestartCoordinator::GetDirectAudioBindingSource(uint64_t guid) const noexcept {
    if (!bindingSourceProvider_) {
        return nullptr;
    }
    return bindingSourceProvider_(guid);
}

// FW-68: the per-GUID gating state + logic now lives in DuplexOperationGate (gate_). These
// entry points are thin forwarders to its self-locking API; behaviour is byte-for-byte the
// former inline bodies (gate_ borrows the same lock_).
bool DiceDuplexRestartCoordinator::TryAcquireGuid(uint64_t guid) noexcept {
    return gate_.Acquire(guid);
}

void DiceDuplexRestartCoordinator::ReleaseGuid(uint64_t guid) noexcept { gate_.Release(guid); }

void DiceDuplexRestartCoordinator::RequestStopIntent(uint64_t guid) noexcept {
    gate_.RequestStop(guid);
}

void DiceDuplexRestartCoordinator::ClearStopIntent(uint64_t guid) noexcept {
    gate_.ClearStop(guid);
}

bool DiceDuplexRestartCoordinator::IsStopRequested(uint64_t guid) const noexcept {
    return gate_.IsStopRequested(guid);
}

uint64_t DiceDuplexRestartCoordinator::AllocateRestartId() noexcept {
    return store_.AllocateRestartId();
}

bool DiceDuplexRestartCoordinator::IsRestartEpochCurrent(
    uint64_t guid, uint64_t restartId, FW::Generation topologyGeneration) const noexcept {
    if (guid == 0 || restartId == 0) {
        return false;
    }
    if (IsStopRequested(guid)) {
        return false;
    }

    if (!lock_) {
        return false;
    }

    IOLockLock(lock_);
    const auto* sessionPtr = store_.FindSessionLocked(guid);
    const bool sessionMatches = (sessionPtr != nullptr) && sessionPtr->restartId == restartId &&
                                sessionPtr->topologyGeneration == topologyGeneration;
    IOLockUnlock(lock_);
    if (!sessionMatches) {
        return false;
    }

    const auto* liveRecord = registry_.FindByGuid(guid);
    if (!liveRecord || liveRecord->gen != topologyGeneration) {
        return false;
    }

    return true;
}

bool DiceDuplexRestartCoordinator::TryConsumePendingClockRequest(uint64_t guid,
                                                                 PendingClockRequest& outRequest) noexcept {
    return clockRequests_.TryConsumePending(guid, outRequest);
}

bool DiceDuplexRestartCoordinator::TryTakeCompletedClockRequest(
    uint64_t guid,
    uint64_t token,
    DiceClockRequestCompletion& outCompletion) noexcept {
    return clockRequests_.TryTakeCompleted(guid, token, outCompletion);
}

void DiceDuplexRestartCoordinator::CompleteClockRequest(const DiceClockRequestCompletion& completion,
                                                        uint64_t guid) noexcept {
    clockRequests_.Complete(completion, guid);
}

void DiceDuplexRestartCoordinator::FailPendingClockRequest(uint64_t guid,
                                                           DiceClockRequestOutcome outcome,
                                                           IOReturn status) noexcept {
    clockRequests_.FailPending(guid, outcome, status);
}

void DiceDuplexRestartCoordinator::RecordTeardownAbort(const char* stage, uint64_t guid) noexcept {
    teardownAbortCount_.fetch_add(1, std::memory_order_acq_rel);
    ASFW_LOG(Audio,
             "DiceDuplexRestartCoordinator: recovery aborted by teardown stage=%{public}s "
             "GUID=%llx kr=0x%x",
             stage ? stage : "unknown", guid, kIOReturnAborted);
}

// FW-69b: session persistence + the restart-id allocator now live in RestartSessionStore
// (store_). These entry points are thin forwarders to its self-locking API; behaviour is
// byte-for-byte the former inline bodies (store_ borrows the same lock_).
DiceRestartSession DiceDuplexRestartCoordinator::LoadSession(uint64_t guid) const noexcept {
    return store_.LoadSession(guid);
}

void DiceDuplexRestartCoordinator::StoreSession(const DiceRestartSession& session) noexcept {
    store_.StoreSession(session);
}

} // namespace ASFW::Audio
