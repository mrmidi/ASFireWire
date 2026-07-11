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

#include <DriverKit/IOLib.h>

#include <atomic>
#include <memory>
#include <utility>

namespace ASFW::Audio {

// FW-66: the recovery-policy classification vocabulary now lives in DiceRecoveryPolicy.hpp.
using namespace ASFW::Audio::Backends;

namespace {

using ASFW::Audio::DICE::ClearRestartProgress;
using ASFW::Audio::AudioClockConfig;
using ASFW::Audio::DICE::HasAnyRestartState;
using ASFW::Audio::DICE::HasDeviceRestartState;
using ASFW::Audio::DICE::HasHostRestartState;
using ASFW::Audio::DICE::HasRestartIntent;

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
// runner owns the ordered stage sequence through protocol-neutral dependency
// seams. FW-73 will perform the remaining mechanical file/type rename.
// Device stream programming before GLOBAL_ENABLE is cross-validated with Linux
// dice-stream.c:326-374 and dice-interface.h:120-125; no source code is copied.
class DuplexStartTransaction final {
public:
    struct Dependencies {
        Discovery::DeviceRegistry& registry;
        AudioRuntimeRegistry& runtime;
        IIsochDuplexHostTransport& hostTransport;
        Driver::HardwareInterface& hardware;
        const std::atomic<bool>* cancel;
        const AudioDuplexCoordinator::DirectAudioBindingSourceProvider& bindingSourceProvider;
        Backends::DuplexOperationGate& gate;
        Backends::RestartSessionStore& sessionStore;
        std::atomic<uint64_t>& teardownAbortCount;
        uint32_t syncBridgeTimeoutMs;
        uint32_t globalClockLockTimeoutMs;
        uint32_t globalClockLockPollMs;
        uint32_t globalClockStableReads;
    };

    struct StartRequest {
        uint64_t guid;
        Discovery::DeviceRecord& record;
        IDuplexDeviceControl& deviceControl;
        DuplexRestartSession& session;
        const AudioClockConfig& desiredClock;
        DuplexRestartReason reason;
    };

    struct StopRequest {
        uint64_t guid;
        Discovery::DeviceRecord& record;
        IDuplexDeviceControl& deviceControl;
        DuplexRestartSession& session;
    };

    struct IdleClockApplyRequest {
        uint64_t guid;
        IDuplexDeviceControl& deviceControl;
        DuplexRestartSession& session;
        FW::Generation topologyGeneration;
        const AudioClockConfig& desiredClock;
        DuplexRestartReason reason;
    };

    explicit DuplexStartTransaction(Dependencies dependencies) noexcept : dependencies_(dependencies) {}

    [[nodiscard]] IOReturn Run(const StartRequest& request) noexcept;
    [[nodiscard]] IOReturn Stop(const StopRequest& request) noexcept;
    [[nodiscard]] IOReturn ApplyIdleClock(const IdleClockApplyRequest& request) noexcept;

private:
    [[nodiscard]] bool TeardownRequested() const noexcept;
    void RecordTeardownAbort(const char* stage, uint64_t guid) noexcept;
    [[nodiscard]] bool IsStopRequested(uint64_t guid) const noexcept;
    [[nodiscard]] bool IsRestartEpochCurrent(uint64_t guid, uint64_t restartId,
                                             FW::Generation topologyGeneration) const noexcept;
    [[nodiscard]] Runtime::IDirectAudioBindingSource* GetDirectAudioBindingSource(
        uint64_t guid) const noexcept;
    [[nodiscard]] IOReturn WaitForStableGlobalClock(
        uint64_t guid,
        IDuplexDeviceControl& deviceControl,
        FW::Generation topologyGeneration,
        const AudioClockConfig& desiredClock) noexcept;

    Dependencies dependencies_;
};

bool DuplexStartTransaction::TeardownRequested() const noexcept {
    return dependencies_.cancel != nullptr &&
           dependencies_.cancel->load(std::memory_order_acquire);
}

void DuplexStartTransaction::RecordTeardownAbort(const char* stage, uint64_t guid) noexcept {
    dependencies_.teardownAbortCount.fetch_add(1, std::memory_order_acq_rel);
    ASFW_LOG(Audio,
             "AudioDuplexCoordinator: recovery aborted by teardown stage=%{public}s "
             "GUID=%llx kr=0x%x",
             stage ? stage : "unknown", guid, kIOReturnAborted);
}

bool DuplexStartTransaction::IsStopRequested(uint64_t guid) const noexcept {
    return dependencies_.gate.IsStopRequested(guid);
}

bool DuplexStartTransaction::IsRestartEpochCurrent(
    uint64_t guid, uint64_t restartId, FW::Generation topologyGeneration) const noexcept {
    if (guid == 0 || restartId == 0 || IsStopRequested(guid)) {
        return false;
    }

    const DuplexRestartSession session = dependencies_.sessionStore.LoadSession(guid);
    if (session.restartId != restartId || session.topologyGeneration != topologyGeneration) {
        return false;
    }

    const auto* liveRecord = dependencies_.registry.FindByGuid(guid);
    return liveRecord != nullptr && liveRecord->gen == topologyGeneration;
}

Runtime::IDirectAudioBindingSource* DuplexStartTransaction::GetDirectAudioBindingSource(
    uint64_t guid) const noexcept {
    return dependencies_.bindingSourceProvider ? dependencies_.bindingSourceProvider(guid) : nullptr;
}

AudioDuplexCoordinator::AudioDuplexCoordinator(
    Discovery::DeviceRegistry& registry, AudioRuntimeRegistry& runtime,
    IIsochDuplexHostTransport& hostTransport, Driver::HardwareInterface& hardware,
    const std::atomic<bool>* cancel,
    DirectAudioBindingSourceProvider bindingSourceProvider) noexcept
    : registry_(registry), runtime_(runtime), hostTransport_(hostTransport), hardware_(hardware),
      cancel_(cancel), bindingSourceProvider_(std::move(bindingSourceProvider)) {
    lock_ = IOLockAlloc();
    if (!lock_) {
        ASFW_LOG_ERROR(Audio, "AudioDuplexCoordinator: failed to allocate lock");
        return;
    }
}

AudioDuplexCoordinator::~AudioDuplexCoordinator() noexcept {
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

IOReturn AudioDuplexCoordinator::StartStreaming(uint64_t guid) noexcept {
    if (guid == 0) {
        return kIOReturnBadArgument;
    }
    if (TeardownRequested()) {
        ASFW_LOG(Audio,
                 "AudioDuplexCoordinator: StartStreaming refused by teardown GUID=%llx",
                 guid);
        return kIOReturnAborted;
    }

    const DuplexRestartSession session = LoadSession(guid);
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
                       "AudioDuplexCoordinator: StartStreaming timed out waiting for GUID "
                       "claim GUID=%llx timeoutMs=%u",
                       guid, kSyncBridgeTimeoutMs);
        return kIOReturnTimeout;
    }

    const IOReturn status = RunStartStreaming(guid);
    ReleaseGuid(guid);
    return status;
}

IOReturn AudioDuplexCoordinator::StopStreaming(uint64_t guid) noexcept {
    if (guid == 0) {
        return kIOReturnBadArgument;
    }
    if (TeardownRequested()) {
        ASFW_LOG(Audio, "AudioDuplexCoordinator: StopStreaming refused by teardown GUID=%llx",
                 guid);
        return kIOReturnAborted;
    }

    const DuplexRestartSession session = LoadSession(guid);
    LogFsmEvent("stop", guid, session.restartId, session.topologyGeneration, session.state,
                session.phase, session.reason);

    RequestStopIntent(guid);
    FailPendingClockRequest(guid, DuplexClockRequestOutcome::kAbortedByStop, kIOReturnAborted);

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
                       "AudioDuplexCoordinator: StopStreaming timed out waiting for GUID "
                       "claim GUID=%llx timeoutMs=%u",
                       guid, kSyncBridgeTimeoutMs);
        ClearStopIntent(guid);
        return kIOReturnTimeout;
    }

    const IOReturn status = RunStopStreaming(guid);
    FailPendingClockRequest(guid, DuplexClockRequestOutcome::kAbortedByStop, kIOReturnAborted);
    ClearStopIntent(guid);
    ReleaseGuid(guid);
    return status;
}

IOReturn AudioDuplexCoordinator::RequestClockConfig(
    uint64_t guid, const AudioClockConfig& desiredClock, DuplexRestartReason reason) noexcept {
    if (guid == 0) {
        return kIOReturnBadArgument;
    }
    if (TeardownRequested()) {
        ASFW_LOG(Audio,
                 "AudioDuplexCoordinator: RequestClockConfig refused by teardown GUID=%llx",
                 guid);
        return kIOReturnAborted;
    }
    if (!IsSupportedAudioClockConfig(desiredClock)) {
        return kIOReturnUnsupported;
    }

    PendingClockRequest request{
        .desiredClock = desiredClock,
        .reason = reason,
    };

    bool shouldLaunchLoop = false;
    std::optional<PendingClockRequest> supersededRequest{};
    DuplexRestartSession session = LoadSession(guid);
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
            DuplexClockRequestCompletion{
                .token = supersededRequest->token,
                .desiredClock = supersededRequest->desiredClock,
                .reason = supersededRequest->reason,
                .outcome = DuplexClockRequestOutcome::kSuperseded,
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
        DuplexClockRequestCompletion completion{};
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

IOReturn AudioDuplexCoordinator::RecoverStreaming(uint64_t guid,
                                                        DuplexRestartReason reason) noexcept {
    if (guid == 0) {
        return kIOReturnBadArgument;
    }
    if (TeardownRequested()) {
        ASFW_LOG(Audio,
                 "AudioDuplexCoordinator: RecoverStreaming refused by teardown GUID=%llx",
                 guid);
        return kIOReturnAborted;
    }

    const DuplexRestartSession session = LoadSession(guid);
    LogFsmEvent("recover", guid, session.restartId, session.topologyGeneration, session.state,
                session.phase, reason);

    FailPendingClockRequest(guid, DuplexClockRequestOutcome::kAbortedByStop, kIOReturnAborted);

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
                       "AudioDuplexCoordinator: RecoverStreaming timed out waiting for GUID "
                       "claim GUID=%llx timeoutMs=%u",
                       guid, kSyncBridgeTimeoutMs);
        return kIOReturnTimeout;
    }

    const IOReturn status = RunRecoveryStreaming(guid, reason);
    ReleaseGuid(guid);
    return status;
}

void AudioDuplexCoordinator::ClearSession(uint64_t guid) noexcept {
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

std::optional<DuplexRestartSession>
AudioDuplexCoordinator::GetSession(uint64_t guid) const noexcept {
    return store_.GetSession(guid);
}

IOReturn AudioDuplexCoordinator::RunStartStreaming(uint64_t guid) noexcept {
    if (TeardownRequested()) {
        return kIOReturnAborted;
    }

    IDuplexDeviceControl* deviceControl = nullptr;
    std::shared_ptr<IDeviceProtocol> protoHold; // keeps the protocol alive for this op
    auto* record = RequireDuplexRecord(guid, deviceControl, protoHold);
    if (!record || !deviceControl) {
        FailPendingClockRequest(guid, DuplexClockRequestOutcome::kFailed, kIOReturnNotReady);
        return kIOReturnNotReady;
    }

    DuplexRestartSession session = LoadSession(guid);
    const AudioClockConfig desiredClock{
        .sampleRateHz = 48000U,
    };
    const DuplexRestartReason reason = HasRestartIntent(session)
                                         ? DICE::ClassifyRestartReason(&session, desiredClock)
                                         : DuplexRestartReason::kInitialStart;

    const IOReturn status =
        RunDuplexStart(guid, *record, *deviceControl, session, desiredClock, reason);
    if (status != kIOReturnSuccess) {
        FailPendingClockRequest(guid, DuplexClockRequestOutcome::kFailed, status);
        return status;
    }

    if (IsStopRequested(guid)) {
        FailPendingClockRequest(guid, DuplexClockRequestOutcome::kAbortedByStop, kIOReturnAborted);
        return kIOReturnSuccess;
    }

    PendingClockRequest pending{};
    while (TryConsumePendingClockRequest(guid, pending)) {
        if (IsStopRequested(guid)) {
            const DuplexRestartSession completionSession = LoadSession(guid);
            CompleteClockRequest(
                DuplexClockRequestCompletion{
                    .token = pending.token,
                    .desiredClock = pending.desiredClock,
                    .reason = pending.reason,
                    .outcome = DuplexClockRequestOutcome::kAbortedByStop,
                    .status = kIOReturnAborted,
                    .restartId = completionSession.restartId,
                    .generation = completionSession.topologyGeneration,
                },
                guid);
            FailPendingClockRequest(guid, DuplexClockRequestOutcome::kAbortedByStop,
                                    kIOReturnAborted);
            break;
        }

        const IOReturn pendingStatus = ApplyClockRequest(guid, pending);
        if (IsStopRequested(guid)) {
            const DuplexRestartSession completionSession = LoadSession(guid);
            CompleteClockRequest(
                DuplexClockRequestCompletion{
                    .token = pending.token,
                    .desiredClock = pending.desiredClock,
                    .reason = pending.reason,
                    .outcome = DuplexClockRequestOutcome::kAbortedByStop,
                    .status = kIOReturnAborted,
                    .restartId = completionSession.restartId,
                    .generation = completionSession.topologyGeneration,
                },
                guid);
            FailPendingClockRequest(guid, DuplexClockRequestOutcome::kAbortedByStop,
                                    kIOReturnAborted);
            break;
        }

        const DuplexRestartSession completionSession = LoadSession(guid);
        CompleteClockRequest(
            DuplexClockRequestCompletion{
                .token = pending.token,
                .desiredClock = pending.desiredClock,
                .reason = pending.reason,
                .outcome = (pendingStatus == kIOReturnSuccess) ? DuplexClockRequestOutcome::kApplied
                                                               : DuplexClockRequestOutcome::kFailed,
                .status = pendingStatus,
                .restartId = completionSession.restartId,
                .generation = completionSession.topologyGeneration,
            },
            guid);
    }

    return kIOReturnSuccess;
}

IOReturn AudioDuplexCoordinator::RunStopStreaming(uint64_t guid) noexcept {
    if (TeardownRequested()) {
        return kIOReturnAborted;
    }

    IDuplexDeviceControl* deviceControl = nullptr;
    std::shared_ptr<IDeviceProtocol> protoHold; // keeps the protocol alive for this op
    auto* record = RequireDuplexRecord(guid, deviceControl, protoHold);
    if (!record || !deviceControl) {
        DuplexRestartSession session = LoadSession(guid);
        ClearFailureSnapshot(session);
        ClearRestartProgress(session);
        StoreSession(session);
        LogTerminal(session);
        return kIOReturnNotReady;
    }

    DuplexRestartSession session = LoadSession(guid);
    return RunDuplexStop(guid, *record, *deviceControl, session);
}

IOReturn AudioDuplexCoordinator::RunRecoveryStreaming(uint64_t guid,
                                                            DuplexRestartReason reason) noexcept {
    if (TeardownRequested()) {
        return kIOReturnAborted;
    }

    IDuplexDeviceControl* deviceControl = nullptr;
    std::shared_ptr<IDeviceProtocol> protoHold; // keeps the protocol alive for this op
    auto* record = RequireDuplexRecord(guid, deviceControl, protoHold);
    DuplexRestartSession session = LoadSession(guid);
    session.guid = guid;
    if (IsRecoveryReason(reason)) {
        RecordIssue(session, session.lastInvalidation, session.phase,
                    DuplexRestartErrorClass::kEpochInvalidated, FailureCauseForReason(reason),
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
        .hasProtocol = (deviceControl != nullptr),
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
        const bool missingDependency = (!record || !deviceControl);
        session.terminalError = missingDependency
                                    ? kIOReturnNotReady
                                    : (session.lastFailure.has_value() ? session.lastFailure->status
                                                                       : kIOReturnUnsupported);
        if (missingDependency) {
            RecordIssue(session, session.lastFailure, session.phase,
                        DuplexRestartErrorClass::kMissingDependency, FailureCauseForReason(reason),
                        session.terminalError, false, false, kIOReturnSuccess, false, false);
        }
        ApplyTerminalPhase(session, DuplexRestartPhase::kFailed, ToString(decision.reason));
        StoreSession(session);
        LogTerminal(session);
        return session.terminalError;
    }

    if (!record || !deviceControl) {
        return kIOReturnNotReady;
    }

    const AudioClockConfig desiredClock =
        (session.desiredClock.sampleRateHz != 0)
            ? session.desiredClock
            : ((session.appliedClock.sampleRateHz != 0)
                   ? session.appliedClock
                   : AudioClockConfig{
                         .sampleRateHz = 48000U,
                     });

    if (HasAnyRestartState(session) || session.phase == DuplexRestartPhase::kRunning) {
        if (TeardownRequested()) {
            return kIOReturnAborted;
        }
        const IOReturn stopStatus = RunDuplexStop(guid, *record, *deviceControl, session);
        if (stopStatus != kIOReturnSuccess) {
            return stopStatus;
        }
    }

    if (IsStopRequested(guid) || TeardownRequested()) {
        return kIOReturnAborted;
    }

    return RunDuplexStart(guid, *record, *deviceControl, session, desiredClock, reason);
}

IOReturn
AudioDuplexCoordinator::RunClockRequestLoop(uint64_t guid,
                                                  PendingClockRequest initialRequest) noexcept {
    PendingClockRequest current = initialRequest;
    IOReturn lastStatus = kIOReturnSuccess;

    while (true) {
        if (IsStopRequested(guid) || TeardownRequested()) {
            const DuplexRestartSession completionSession = LoadSession(guid);
            CompleteClockRequest(
                DuplexClockRequestCompletion{
                    .token = current.token,
                    .desiredClock = current.desiredClock,
                    .reason = current.reason,
                    .outcome = DuplexClockRequestOutcome::kAbortedByStop,
                    .status = kIOReturnAborted,
                    .restartId = completionSession.restartId,
                    .generation = completionSession.topologyGeneration,
                },
                guid);
            FailPendingClockRequest(guid, DuplexClockRequestOutcome::kAbortedByStop,
                                    kIOReturnAborted);
            break;
        }

        lastStatus = ApplyClockRequest(guid, current);

        if (IsStopRequested(guid) || TeardownRequested()) {
            const DuplexRestartSession completionSession = LoadSession(guid);
            CompleteClockRequest(
                DuplexClockRequestCompletion{
                    .token = current.token,
                    .desiredClock = current.desiredClock,
                    .reason = current.reason,
                    .outcome = DuplexClockRequestOutcome::kAbortedByStop,
                    .status = kIOReturnAborted,
                    .restartId = completionSession.restartId,
                    .generation = completionSession.topologyGeneration,
                },
                guid);
            FailPendingClockRequest(guid, DuplexClockRequestOutcome::kAbortedByStop,
                                    kIOReturnAborted);
            break;
        }

        const DuplexRestartSession completionSession = LoadSession(guid);
        CompleteClockRequest(
            DuplexClockRequestCompletion{
                .token = current.token,
                .desiredClock = current.desiredClock,
                .reason = current.reason,
                .outcome = (lastStatus == kIOReturnSuccess) ? DuplexClockRequestOutcome::kApplied
                                                            : DuplexClockRequestOutcome::kFailed,
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
AudioDuplexCoordinator::ApplyClockRequest(uint64_t guid,
                                                const PendingClockRequest& request) noexcept {
    if (IsStopRequested(guid) || TeardownRequested()) {
        return kIOReturnAborted;
    }

    IDuplexDeviceControl* deviceControl = nullptr;
    std::shared_ptr<IDeviceProtocol> protoHold; // keeps the protocol alive for this op
    auto* record = RequireDuplexRecord(guid, deviceControl, protoHold);
    if (!record || !deviceControl) {
        return kIOReturnNotReady;
    }
    if (!IsSupportedAudioClockConfig(request.desiredClock)) {
        return kIOReturnUnsupported;
    }

    DuplexRestartSession session = LoadSession(guid);
    if (HasAnyRestartState(session) || session.phase == DuplexRestartPhase::kRunning ||
        session.state == DuplexRestartState::kRunning ||
        session.state == DuplexRestartState::kRecovering ||
        session.state == DuplexRestartState::kFailed) {
        if (TeardownRequested()) {
            return kIOReturnAborted;
        }
        const IOReturn stopStatus = RunDuplexStop(guid, *record, *deviceControl, session);
        if (stopStatus != kIOReturnSuccess) {
            return stopStatus;
        }
        if (TeardownRequested()) {
            return kIOReturnAborted;
        }
        return RunDuplexStart(guid, *record, *deviceControl, session, request.desiredClock,
                              request.reason);
    }

    if (TeardownRequested()) {
        return kIOReturnAborted;
    }

    return RunIdleClockApply(guid, *deviceControl, session, record->gen, request.desiredClock,
                             request.reason);
}

IOReturn DuplexStartTransaction::Run(const StartRequest& request) noexcept {
    const uint64_t guid = request.guid;
    Discovery::DeviceRecord& record = request.record;
    IDuplexDeviceControl& deviceControl = request.deviceControl;
    DuplexRestartSession& session = request.session;
    const AudioClockConfig& desiredClock = request.desiredClock;
    const DuplexRestartReason reason = request.reason;
    auto& runtime_ = dependencies_.runtime;
    auto& hostTransport_ = dependencies_.hostTransport;
    auto& hardware_ = dependencies_.hardware;
    const std::atomic<bool>* const cancel_ = dependencies_.cancel;
    const uint32_t kSyncBridgeTimeoutMs = dependencies_.syncBridgeTimeoutMs;

    auto TeardownRequested = [this]() noexcept { return this->TeardownRequested(); };
    auto RecordTeardownAbort = [this](const char* stage, uint64_t abortGuid) noexcept {
        this->RecordTeardownAbort(stage, abortGuid);
    };
    auto AllocateRestartId = [this]() noexcept { return dependencies_.sessionStore.AllocateRestartId(); };
    auto StoreSession = [this](const DuplexRestartSession& updatedSession) noexcept {
        dependencies_.sessionStore.StoreSession(updatedSession);
    };
    auto IsStopRequested = [this](uint64_t requestGuid) noexcept {
        return this->IsStopRequested(requestGuid);
    };
    auto IsRestartEpochCurrent = [this](uint64_t requestGuid, uint64_t restartId,
                                        FW::Generation generation) noexcept {
        return this->IsRestartEpochCurrent(requestGuid, restartId, generation);
    };
    auto GetDirectAudioBindingSource = [this](uint64_t requestGuid) noexcept {
        return this->GetDirectAudioBindingSource(requestGuid);
    };
    auto RunDuplexStop = [this](uint64_t stopGuid, Discovery::DeviceRecord& stopRecord,
                                IDuplexDeviceControl& stopDevice,
                                DuplexRestartSession& stopSession) noexcept {
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

    // Pre-read the device's static stream geometry so channel resolution and IRM reservation see
    // the real stream count. EnsureRuntimeStreamGeometry publishes the per-stream
    // caps that DuplexStreamProfileResolver consumes via GetRuntimeAudioStreamCaps.
    // Non-fatal: on failure we fall back to the legacy single-stream resolution
    // and PrepareDuplex will surface any genuine device error. A multi-stream
    // device needs this to allocate a channel per stream. The DICE implementation is
    // cross-validated with FFADO dice_avdevice.cpp prepare() (m_nb_rx/m_nb_tx).
    const auto geometryLoad = WaitForAsyncResult<bool>(
        [&](auto callback) {
            deviceControl.EnsureRuntimeStreamGeometry(
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

    auto finalizeFailure = [&](IOReturn failureStatus, DuplexRestartPhase failedPhase,
                               DuplexRestartFailureCause cause, DuplexRestartErrorClass errorClass,
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
        ApplyTerminalPhase(session, DuplexRestartPhase::kFailed, ToString(errorClass));
        StoreSession(session);
        LogTerminal(session);
        return failureStatus;
    };

    auto rollbackToFailure = [&](IOReturn failureStatus, DuplexRestartPhase failedPhase,
                                 DuplexRestartFailureCause cause) noexcept {
        if (failureStatus == kIOReturnAborted && TeardownRequested()) {
            return failureStatus;
        }
        const IOReturn rollbackStatus = RunDuplexStop(guid, record, deviceControl, session);
        return finalizeFailure(failureStatus, failedPhase, cause,
                               DuplexRestartErrorClass::kStageFailure, true, rollbackStatus, true,
                               true);
    };

    auto rollbackToInvalidation = [&](IOReturn invalidationStatus, DuplexRestartPhase failedPhase,
                                      DuplexRestartFailureCause cause) noexcept {
        if (invalidationStatus == kIOReturnAborted && TeardownRequested()) {
            return invalidationStatus;
        }
        const IOReturn rollbackStatus = RunDuplexStop(guid, record, deviceControl, session);
        if (rollbackStatus != kIOReturnSuccess) {
            return finalizeFailure(
                rollbackStatus, DuplexRestartPhase::kStopping, DuplexRestartFailureCause::kStop,
                DuplexRestartErrorClass::kStageFailure, true, rollbackStatus, true, true);
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
                    IsStopRequested(guid) ? DuplexRestartErrorClass::kStopIntent
                                          : DuplexRestartErrorClass::kEpochInvalidated,
                    cause, invalidationStatus, true, true, rollbackStatus, true, true);
        ClearFailureSnapshot(session);
        ApplyTerminalPhase(session, DuplexRestartPhase::kIdle,
                           ToString(session.lastInvalidation->errorClass));
        StoreSession(session);
        LogInvalidation(session);
        LogTerminal(session);
        return invalidationStatus;
    };

    auto* irmClient = deviceControl.GetIRMClient();
    if (irmClient == nullptr) {
        ASFW_LOG_ERROR(Audio, "AudioDuplexCoordinator: device control missing IRM client GUID=%llx",
                       guid);
        return finalizeFailure(kIOReturnNotReady, DuplexRestartPhase::kPreparingDevice,
                               DuplexRestartFailureCause::kPrepare,
                               DuplexRestartErrorClass::kMissingDependency, false, kIOReturnSuccess,
                               false, false);
    }

    auto* bindingSource = GetDirectAudioBindingSource(guid);
    if (!bindingSource) {
        return finalizeFailure(kIOReturnNotReady, DuplexRestartPhase::kPreparingDevice,
                               DuplexRestartFailureCause::kPrepare,
                               DuplexRestartErrorClass::kMissingDependency, false, kIOReturnSuccess,
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
    ApplyTerminalPhase(session, DuplexRestartPhase::kIdle, "reset_before_start");
    SetSessionState(session, RestartStateForStartReason(reason), ToString(reason));
    SetSessionPhase(session, DuplexRestartPhase::kPreparingDevice);
    StoreSession(session);
    LogFsmEvent("start", guid, restartId, topologyGeneration, session.state, session.phase, reason);
    ASFW_LOG(Audio, "AudioDuplexCoordinator: using isoch channels d2h=%u h2d=%u GUID=%llx",
             channels.deviceToHostIsoChannel, channels.hostToDeviceIsoChannel, guid);

    if (abortIfTeardown("BeginSplitDuplex")) {
        return kIOReturnAborted;
    }
    const kern_return_t claimStatus = hostTransport_.BeginSplitDuplex(guid);
    if (claimStatus != kIOReturnSuccess) {
        return finalizeFailure(
            claimStatus, DuplexRestartPhase::kPreparingDevice, DuplexRestartFailureCause::kPrepare,
            DuplexRestartErrorClass::kStageFailure, false, kIOReturnSuccess, true, true);
    }
    session.hostDuplexClaimed = true;
    StoreSession(session);

    if (abortIfTeardown("PreparingDevice")) {
        return kIOReturnAborted;
    }
    const auto prepare = WaitForAsyncResult<DuplexPrepareResult>(
        [&](auto callback) {
            deviceControl.PrepareDuplex(channels, desiredClock, std::move(callback));
        },
        kSyncBridgeTimeoutMs, kIOReturnTimeout, cancel_);
    if (prepare.status != kIOReturnSuccess) {
        if (prepare.status == kIOReturnAborted && TeardownRequested()) {
            RecordTeardownAbort("PreparingDevice", guid);
        }
        return rollbackToFailure(prepare.status, DuplexRestartPhase::kPreparingDevice,
                                 DuplexRestartFailureCause::kPrepare);
    }
    if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
        return rollbackToInvalidation(kIOReturnAborted, DuplexRestartPhase::kPreparingDevice,
                                      DuplexRestartFailureCause::kPrepare);
    }
    session.ownerClaimed = true;
    session.devicePrepared = true;
    session.generation = prepare.value.generation;
    session.appliedClock = prepare.value.appliedClock;
    session.runtimeCaps = prepare.value.runtimeCaps;
    const DuplexStreamProfile streamProfile =
        DuplexStreamProfileResolver::Resolve(record, session.runtimeCaps, channels);
    SetSessionPhase(session, DuplexRestartPhase::kPrepared);
    StoreSession(session);

    // Reserve IRM bandwidth + channel for EVERY playback (host IT -> device RX)
    // stream. A multi-stream device needs all of its playback isoch channels reserved before
    // the device-enable stage; single-stream devices loop once.
    if (abortIfTeardown("ReservingPlaybackResources")) {
        return kIOReturnAborted;
    }
    for (uint32_t i = 0; i < channels.playbackStreamCount; ++i) {
        const kern_return_t reservePlaybackStatus = hostTransport_.ReservePlaybackResources(
            guid, *irmClient, channels.PlaybackChannel(i), streamProfile.playbackBandwidthUnits);
        if (reservePlaybackStatus != kIOReturnSuccess) {
            return rollbackToFailure(reservePlaybackStatus,
                                     DuplexRestartPhase::kReservingPlaybackResources,
                                     DuplexRestartFailureCause::kReservePlayback);
        }
        if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
            return rollbackToInvalidation(kIOReturnAborted,
                                          DuplexRestartPhase::kReservingPlaybackResources,
                                          DuplexRestartFailureCause::kReservePlayback);
        }
    }
    SetSessionPhase(session, DuplexRestartPhase::kReservingPlaybackResources);
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
                                     DuplexRestartPhase::kReservingCaptureResources,
                                     DuplexRestartFailureCause::kReserveCapture);
        }
        if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
            return rollbackToInvalidation(kIOReturnAborted,
                                          DuplexRestartPhase::kReservingCaptureResources,
                                          DuplexRestartFailureCause::kReserveCapture);
        }
    }
    SetSessionPhase(session, DuplexRestartPhase::kReservingCaptureResources);
    session.hostCaptureReserved = true;
    StoreSession(session);

    ASFW_LOG(Audio,
             "AUDIO DUPLEX START guid=0x%016llx ir=%u it=%u inCh=%u outCh=%u inSlots=%u outSlots=%u "
             "mode=blocking rxFmt=%u txFmt=%u",
             guid, channels.deviceToHostIsoChannel, channels.hostToDeviceIsoChannel,
             session.runtimeCaps.hostInputPcmChannels, session.runtimeCaps.hostOutputPcmChannels,
             session.runtimeCaps.deviceToHostAm824Slots, session.runtimeCaps.hostToDeviceAm824Slots,
             static_cast<uint32_t>(streamProfile.captureWireFormat),
             static_cast<uint32_t>(streamProfile.playbackWireFormat));

    // Saffire.kext allocates every local/remote isoch port before it reports
    // the assigned channels to the device adapter. Keep the expensive DMA setup before the
    // device-enable stage as well.
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
                                         DuplexRestartPhase::kStartingHostReceive,
                                         DuplexRestartFailureCause::kStartReceive);
            }
            if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
                return rollbackToInvalidation(kIOReturnAborted,
                                              DuplexRestartPhase::kStartingHostReceive,
                                              DuplexRestartFailureCause::kStartReceive);
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
                    return rollbackToFailure(status, DuplexRestartPhase::kStartingHostReceive,
                                             DuplexRestartFailureCause::kStartReceive);
                }
                if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
                    return rollbackToInvalidation(kIOReturnAborted,
                                                  DuplexRestartPhase::kStartingHostReceive,
                                                  DuplexRestartFailureCause::kStartReceive);
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
            return rollbackToFailure(prepareTransmitStatus, DuplexRestartPhase::kStartingHostTransmit,
                                     DuplexRestartFailureCause::kStartTransmit);
        }
        if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
            return rollbackToInvalidation(kIOReturnAborted, DuplexRestartPhase::kStartingHostTransmit,
                                          DuplexRestartFailureCause::kStartTransmit);
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
                return rollbackToFailure(status, DuplexRestartPhase::kStartingHostTransmit,
                                         DuplexRestartFailureCause::kStartTransmit);
            }
            if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
                return rollbackToInvalidation(kIOReturnAborted,
                                              DuplexRestartPhase::kStartingHostTransmit,
                                              DuplexRestartFailureCause::kStartTransmit);
            }
        }
    }

    SetSessionPhase(session, DuplexRestartPhase::kWaitingGlobalClock);
    StoreSession(session);
    if (abortIfTeardown("WaitingGlobalClock")) {
        return kIOReturnAborted;
    }
    const IOReturn clockLockStatus =
        WaitForStableGlobalClock(guid, deviceControl, topologyGeneration, desiredClock);
    if (clockLockStatus != kIOReturnSuccess) {
        return rollbackToFailure(clockLockStatus, DuplexRestartPhase::kWaitingGlobalClock,
                                 DuplexRestartFailureCause::kGlobalClockLock);
    }
    if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
        return rollbackToInvalidation(kIOReturnAborted, DuplexRestartPhase::kWaitingGlobalClock,
                                      DuplexRestartFailureCause::kGlobalClockLock);
    }

    if (abortIfTeardown("ProgrammingDeviceRx")) {
        return kIOReturnAborted;
    }
    const auto programRx = WaitForAsyncResult<DuplexStageResult>(
        [&](auto callback) { deviceControl.ProgramRx(std::move(callback)); }, kSyncBridgeTimeoutMs,
        kIOReturnTimeout, cancel_);
    if (programRx.status != kIOReturnSuccess) {
        if (programRx.status == kIOReturnAborted && TeardownRequested()) {
            RecordTeardownAbort("ProgrammingDeviceRx", guid);
        }
        return rollbackToFailure(programRx.status, DuplexRestartPhase::kProgrammingDeviceRx,
                                 DuplexRestartFailureCause::kProgramRx);
    }
    if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
        return rollbackToInvalidation(kIOReturnAborted, DuplexRestartPhase::kProgrammingDeviceRx,
                                      DuplexRestartFailureCause::kProgramRx);
    }
    session.generation = programRx.value.generation;
    SetSessionPhase(session, DuplexRestartPhase::kProgrammingDeviceRx);
    session.deviceRxProgrammed = true;
    session.runtimeCaps = programRx.value.runtimeCaps.hostInputPcmChannels != 0
                              ? programRx.value.runtimeCaps
                              : session.runtimeCaps;
    SetSessionPhase(session, DuplexRestartPhase::kDeviceRxProgrammed);
    StoreSession(session);

    if (abortIfTeardown("ProgrammingDeviceTx")) {
        return kIOReturnAborted;
    }
    const auto programTx = WaitForAsyncResult<DuplexStageResult>(
        [&](auto callback) { deviceControl.ProgramTxAndEnableDuplex(std::move(callback)); },
        kSyncBridgeTimeoutMs, kIOReturnTimeout, cancel_);
    if (programTx.status != kIOReturnSuccess) {
        if (programTx.status == kIOReturnAborted && TeardownRequested()) {
            RecordTeardownAbort("ProgrammingDeviceTx", guid);
        }
        return rollbackToFailure(programTx.status, DuplexRestartPhase::kProgrammingDeviceTx,
                                 DuplexRestartFailureCause::kProgramTx);
    }
    if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
        return rollbackToInvalidation(kIOReturnAborted, DuplexRestartPhase::kProgrammingDeviceTx,
                                      DuplexRestartFailureCause::kProgramTx);
    }
    session.generation = programTx.value.generation;
    SetSessionPhase(session, DuplexRestartPhase::kProgrammingDeviceTx);
    session.deviceTxArmed = true;
    session.runtimeCaps = programTx.value.runtimeCaps.hostInputPcmChannels != 0
                              ? programTx.value.runtimeCaps
                              : session.runtimeCaps;
    SetSessionPhase(session, DuplexRestartPhase::kDeviceTxArmed);
    StoreSession(session);

    // The device-control contract completes its enable stage before the already-allocated host
    // isoch channels start.
    IOSleep(streamProfile.startOrder.postDeviceEnableDelayMs);
    if (abortIfTeardown("PostGlobalEnableDelay")) {
        return kIOReturnAborted;
    }

    // A profile may start host receive first so the device is already transmitting before host
    // transmit begins, avoiding a window without the expected clock reference.
    for (const DuplexHostDirection direction : streamProfile.startOrder.startOrder) {
        if (direction == DuplexHostDirection::kReceive) {
            if (abortIfTeardown("StartingHostReceive")) {
                return kIOReturnAborted;
            }
            const kern_return_t startReceiveStatus = hostTransport_.StartPreparedReceive();
            if (startReceiveStatus != kIOReturnSuccess) {
                return rollbackToFailure(startReceiveStatus, DuplexRestartPhase::kStartingHostReceive,
                                         DuplexRestartFailureCause::kStartReceive);
            }
            if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
                return rollbackToInvalidation(kIOReturnAborted,
                                              DuplexRestartPhase::kStartingHostReceive,
                                              DuplexRestartFailureCause::kStartReceive);
            }
            SetSessionPhase(session, DuplexRestartPhase::kStartingHostReceive);
            session.hostReceiveStarted = true;
            StoreSession(session);

            continue;
        }

        if (abortIfTeardown("StartingHostTransmit")) {
            return kIOReturnAborted;
        }
        const kern_return_t startTransmitStatus = hostTransport_.StartPreparedTransmit();
        if (startTransmitStatus != kIOReturnSuccess) {
            return rollbackToFailure(startTransmitStatus, DuplexRestartPhase::kStartingHostTransmit,
                                     DuplexRestartFailureCause::kStartTransmit);
        }
        if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
            return rollbackToInvalidation(kIOReturnAborted, DuplexRestartPhase::kStartingHostTransmit,
                                          DuplexRestartFailureCause::kStartTransmit);
        }
        SetSessionPhase(session, DuplexRestartPhase::kStartingHostTransmit);
        session.hostTransmitStarted = true;
        StoreSession(session);
    }

    if (abortIfTeardown("ConfirmingDeviceStart")) {
    return kIOReturnAborted;
}
const auto confirm = WaitForAsyncResult<DuplexConfirmResult>(
    [&](auto callback) { deviceControl.ConfirmDuplexStart(std::move(callback)); },
    kSyncBridgeTimeoutMs, kIOReturnTimeout, cancel_);
if (confirm.status != kIOReturnSuccess) {
    if (confirm.status == kIOReturnAborted && TeardownRequested()) {
        RecordTeardownAbort("ConfirmingDeviceStart", guid);
    }
    return rollbackToFailure(confirm.status, DuplexRestartPhase::kConfirmingDeviceStart,
                             DuplexRestartFailureCause::kConfirmStart);
}
if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
    return rollbackToInvalidation(kIOReturnAborted, DuplexRestartPhase::kConfirmingDeviceStart,
                                  DuplexRestartFailureCause::kConfirmStart);
}

SetSessionPhase(session, DuplexRestartPhase::kConfirmingDeviceStart);
SetSessionPhase(session, DuplexRestartPhase::kRunning);
SetSessionState(session, DuplexRestartState::kRunning, "confirmed_running");
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
    uint64_t guid, IDuplexDeviceControl& deviceControl, const FW::Generation topologyGeneration,
    const AudioClockConfig& desiredClock) noexcept {
    const std::atomic<bool>* const cancel_ = dependencies_.cancel;
    const uint32_t kGlobalClockLockTimeoutMs = dependencies_.globalClockLockTimeoutMs;
    const uint32_t kGlobalClockLockPollMs = dependencies_.globalClockLockPollMs;
    const uint32_t kGlobalClockStableReads = dependencies_.globalClockStableReads;
    auto TeardownRequested = [this]() noexcept { return this->TeardownRequested(); };
    auto RecordTeardownAbort = [this](const char* stage, uint64_t abortGuid) noexcept {
        this->RecordTeardownAbort(stage, abortGuid);
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
        const auto health = WaitForAsyncResult<DuplexHealthResult>(
            [&](auto callback) { deviceControl.ReadDuplexHealth(std::move(callback)); },
            std::max(remainingMs, 1U), kIOReturnTimeout, cancel_);
        if (health.status != kIOReturnSuccess) {
            if (health.status == kIOReturnAborted && TeardownRequested()) {
                RecordTeardownAbort("WaitingGlobalClock", guid);
                return health.status;
            }
            ASFW_LOG_ERROR(Audio, "Device clock health read failed before isoch start kr=0x%x",
                           health.status);
            return health.status;
        }
        if (health.value.generation != topologyGeneration) {
            ASFW_LOG_ERROR(
                Audio,
                "Device clock generation changed before isoch start expected=%u actual=%u",
                GenerationValue(topologyGeneration), GenerationValue(health.value.generation));
            return kIOReturnOffline;
        }

        const bool lockedAtTarget =
            health.value.sourceLocked &&
            health.value.nominalRateHz == desiredClock.sampleRateHz &&
            health.value.appliedClock.sampleRateHz == desiredClock.sampleRateHz;
        consecutiveLockedReads = lockedAtTarget ? consecutiveLockedReads + 1 : 0;

        if (consecutiveLockedReads >= kGlobalClockStableReads) {
            ASFW_LOG(Audio,
                     "Device clock stable before isoch start rate=%u status=0x%08x reads=%u",
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
                   "Device clock failed to stabilize before isoch start rate=%u timeoutMs=%u",
                   desiredClock.sampleRateHz, kGlobalClockLockTimeoutMs);
    return kIOReturnTimeout;
}

IOReturn DuplexStartTransaction::Stop(const StopRequest& request) noexcept {
    const uint64_t guid = request.guid;
    Discovery::DeviceRecord& record = request.record;
    IDuplexDeviceControl& deviceControl = request.deviceControl;
    DuplexRestartSession& session = request.session;
    auto& hostTransport_ = dependencies_.hostTransport;
    auto TeardownRequested = [this]() noexcept { return this->TeardownRequested(); };
    auto RecordTeardownAbort = [this](const char* stage, uint64_t abortGuid) noexcept {
        this->RecordTeardownAbort(stage, abortGuid);
    };
    auto StoreSession = [this](const DuplexRestartSession& updatedSession) noexcept {
        dependencies_.sessionStore.StoreSession(updatedSession);
    };

    (void)record;

    if (TeardownRequested()) {
        RecordTeardownAbort("RunDuplexStop", guid);
        return kIOReturnAborted;
    }

    IOReturn result = kIOReturnSuccess;
    SetSessionPhase(session, DuplexRestartPhase::kStopping);
    SetSessionState(session, DuplexRestartState::kStopping, "stop_requested");
    StoreSession(session);

    const kern_return_t hostStatus = hostTransport_.StopAll();
    if (hostStatus != kIOReturnSuccess) {
        result = hostStatus;
    }

    const IOReturn deviceStatus = deviceControl.StopDuplex();
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
        ApplyTerminalPhase(session, DuplexRestartPhase::kIdle, "stop_complete");
    } else {
        session.terminalError = result;
        RecordIssue(session, session.lastFailure, DuplexRestartPhase::kStopping,
                    DuplexRestartErrorClass::kStageFailure, DuplexRestartFailureCause::kStop, result,
                    IsRetryableStatus(result), false, kIOReturnSuccess, true, true);
        ApplyTerminalPhase(session, DuplexRestartPhase::kFailed, "stop_failed");
    }
    StoreSession(session);
    LogTerminal(session);
    return result;
}

IOReturn DuplexStartTransaction::ApplyIdleClock(const IdleClockApplyRequest& request) noexcept {
    const uint64_t guid = request.guid;
    IDuplexDeviceControl& deviceControl = request.deviceControl;
    DuplexRestartSession& session = request.session;
    const FW::Generation topologyGeneration = request.topologyGeneration;
    const AudioClockConfig& desiredClock = request.desiredClock;
    const DuplexRestartReason reason = request.reason;
    const std::atomic<bool>* const cancel_ = dependencies_.cancel;
    const uint32_t kSyncBridgeTimeoutMs = dependencies_.syncBridgeTimeoutMs;
    auto TeardownRequested = [this]() noexcept { return this->TeardownRequested(); };
    auto RecordTeardownAbort = [this](const char* stage, uint64_t abortGuid) noexcept {
        this->RecordTeardownAbort(stage, abortGuid);
    };
    auto AllocateRestartId = [this]() noexcept { return dependencies_.sessionStore.AllocateRestartId(); };
    auto StoreSession = [this](const DuplexRestartSession& updatedSession) noexcept {
        dependencies_.sessionStore.StoreSession(updatedSession);
    };
    auto IsStopRequested = [this](uint64_t requestGuid) noexcept {
        return this->IsStopRequested(requestGuid);
    };
    auto IsRestartEpochCurrent = [this](uint64_t requestGuid, uint64_t restartId,
                                        FW::Generation generation) noexcept {
        return this->IsRestartEpochCurrent(requestGuid, restartId, generation);
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
    ApplyTerminalPhase(session, DuplexRestartPhase::kIdle, "reset_before_idle_apply");
    SetSessionState(session, DuplexRestartState::kApplyingIdleClock, ToString(reason));
    SetSessionPhase(session, DuplexRestartPhase::kPreparingDevice);
    StoreSession(session);

    const auto apply = WaitForAsyncResult<ClockApplyResult>(
        [&](auto callback) { deviceControl.ApplyClockConfig(desiredClock, std::move(callback)); },
        kSyncBridgeTimeoutMs, kIOReturnTimeout, cancel_);
    if (apply.status != kIOReturnSuccess) {
        if (apply.status == kIOReturnAborted && TeardownRequested()) {
            RecordTeardownAbort("IdleClockApply", guid);
        }
        session.terminalError = apply.status;
        RecordIssue(session, session.lastFailure, DuplexRestartPhase::kPreparingDevice,
                    DuplexRestartErrorClass::kStageFailure, DuplexRestartFailureCause::kIdleClockApply,
                    apply.status, IsRetryableStatus(apply.status), false, kIOReturnSuccess, true,
                    true);
        ApplyTerminalPhase(session, DuplexRestartPhase::kFailed, "idle_apply_failed");
        StoreSession(session);
        LogTerminal(session);
        return apply.status;
    }
    if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
        session.terminalError = kIOReturnSuccess;
        RecordIssue(session, session.lastInvalidation, DuplexRestartPhase::kPreparingDevice,
                    IsStopRequested(guid) ? DuplexRestartErrorClass::kStopIntent
                                          : DuplexRestartErrorClass::kEpochInvalidated,
                    DuplexRestartFailureCause::kIdleClockApply, kIOReturnAborted, true, false,
                    kIOReturnSuccess, true, true);
        ClearFailureSnapshot(session);
        ApplyTerminalPhase(session, DuplexRestartPhase::kIdle, "idle_apply_invalidated");
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
    ApplyTerminalPhase(session, DuplexRestartPhase::kIdle, "idle_apply_complete");
    StoreSession(session);
    LogTerminal(session);
    return kIOReturnSuccess;
}

IOReturn AudioDuplexCoordinator::RunDuplexStart(
    uint64_t guid, Discovery::DeviceRecord& record, IDuplexDeviceControl& deviceControl,
    DuplexRestartSession& session, const AudioClockConfig& desiredClock,
    DuplexRestartReason reason) noexcept {
    DuplexStartTransaction transaction{DuplexStartTransaction::Dependencies{
        registry_, runtime_, hostTransport_, hardware_, cancel_, bindingSourceProvider_, gate_, store_,
        teardownAbortCount_, kSyncBridgeTimeoutMs, kGlobalClockLockTimeoutMs,
        kGlobalClockLockPollMs, kGlobalClockStableReads}};
    return transaction.Run(
        DuplexStartTransaction::StartRequest{guid, record, deviceControl, session, desiredClock,
                                             reason});
}

IOReturn AudioDuplexCoordinator::RunDuplexStop(
    uint64_t guid, Discovery::DeviceRecord& record, IDuplexDeviceControl& deviceControl,
    DuplexRestartSession& session) noexcept {
    DuplexStartTransaction transaction{DuplexStartTransaction::Dependencies{
        registry_, runtime_, hostTransport_, hardware_, cancel_, bindingSourceProvider_, gate_, store_,
        teardownAbortCount_, kSyncBridgeTimeoutMs, kGlobalClockLockTimeoutMs,
        kGlobalClockLockPollMs, kGlobalClockStableReads}};
    return transaction.Stop(
        DuplexStartTransaction::StopRequest{guid, record, deviceControl, session});
}

IOReturn AudioDuplexCoordinator::RunIdleClockApply(
    uint64_t guid, IDuplexDeviceControl& deviceControl, DuplexRestartSession& session,
    FW::Generation topologyGeneration, const AudioClockConfig& desiredClock,
    DuplexRestartReason reason) noexcept {
    DuplexStartTransaction transaction{DuplexStartTransaction::Dependencies{
        registry_, runtime_, hostTransport_, hardware_, cancel_, bindingSourceProvider_, gate_, store_,
        teardownAbortCount_, kSyncBridgeTimeoutMs, kGlobalClockLockTimeoutMs,
        kGlobalClockLockPollMs, kGlobalClockStableReads}};
    return transaction.ApplyIdleClock(
        DuplexStartTransaction::IdleClockApplyRequest{guid, deviceControl, session,
                                                       topologyGeneration, desiredClock, reason});
}

Discovery::DeviceRecord* AudioDuplexCoordinator::RequireDuplexRecord(
    uint64_t guid, IDuplexDeviceControl*& outDeviceControl,
    std::shared_ptr<IDeviceProtocol>& outHold) noexcept {
    outDeviceControl = nullptr;
    outHold.reset();
    auto* record = registry_.FindByGuid(guid);
    if (!record) {
        return nullptr;
    }

    auto protocol = runtime_.FindShared(guid);
    if (!protocol) {
        return nullptr;
    }

    outDeviceControl = protocol->AsDuplexDeviceControl();
    if (outDeviceControl == nullptr) {
        return nullptr;
    }
    outDeviceControl->SetTeardownCancelToken(cancel_);

    outHold = std::move(protocol);
    return record;
}

ASFW::Audio::Runtime::IDirectAudioBindingSource*
AudioDuplexCoordinator::GetDirectAudioBindingSource(uint64_t guid) const noexcept {
    if (!bindingSourceProvider_) {
        return nullptr;
    }
    return bindingSourceProvider_(guid);
}

// FW-68: the per-GUID gating state + logic now lives in DuplexOperationGate (gate_). These
// entry points are thin forwarders to its self-locking API; behaviour is byte-for-byte the
// former inline bodies (gate_ borrows the same lock_).
bool AudioDuplexCoordinator::TryAcquireGuid(uint64_t guid) noexcept {
    return gate_.Acquire(guid);
}

void AudioDuplexCoordinator::ReleaseGuid(uint64_t guid) noexcept { gate_.Release(guid); }

void AudioDuplexCoordinator::RequestStopIntent(uint64_t guid) noexcept {
    gate_.RequestStop(guid);
}

void AudioDuplexCoordinator::ClearStopIntent(uint64_t guid) noexcept {
    gate_.ClearStop(guid);
}

bool AudioDuplexCoordinator::IsStopRequested(uint64_t guid) const noexcept {
    return gate_.IsStopRequested(guid);
}

uint64_t AudioDuplexCoordinator::AllocateRestartId() noexcept {
    return store_.AllocateRestartId();
}

bool AudioDuplexCoordinator::IsRestartEpochCurrent(
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

bool AudioDuplexCoordinator::TryConsumePendingClockRequest(uint64_t guid,
                                                                 PendingClockRequest& outRequest) noexcept {
    return clockRequests_.TryConsumePending(guid, outRequest);
}

bool AudioDuplexCoordinator::TryTakeCompletedClockRequest(
    uint64_t guid,
    uint64_t token,
    DuplexClockRequestCompletion& outCompletion) noexcept {
    return clockRequests_.TryTakeCompleted(guid, token, outCompletion);
}

void AudioDuplexCoordinator::CompleteClockRequest(const DuplexClockRequestCompletion& completion,
                                                        uint64_t guid) noexcept {
    clockRequests_.Complete(completion, guid);
}

void AudioDuplexCoordinator::FailPendingClockRequest(uint64_t guid,
                                                           DuplexClockRequestOutcome outcome,
                                                           IOReturn status) noexcept {
    clockRequests_.FailPending(guid, outcome, status);
}

void AudioDuplexCoordinator::RecordTeardownAbort(const char* stage, uint64_t guid) noexcept {
    teardownAbortCount_.fetch_add(1, std::memory_order_acq_rel);
    ASFW_LOG(Audio,
             "AudioDuplexCoordinator: recovery aborted by teardown stage=%{public}s "
             "GUID=%llx kr=0x%x",
             stage ? stage : "unknown", guid, kIOReturnAborted);
}

// FW-69b: session persistence + the restart-id allocator now live in RestartSessionStore
// (store_). These entry points are thin forwarders to its self-locking API; behaviour is
// byte-for-byte the former inline bodies (store_ borrows the same lock_).
DuplexRestartSession AudioDuplexCoordinator::LoadSession(uint64_t guid) const noexcept {
    return store_.LoadSession(guid);
}

void AudioDuplexCoordinator::StoreSession(const DuplexRestartSession& session) noexcept {
    store_.StoreSession(session);
}

} // namespace ASFW::Audio
