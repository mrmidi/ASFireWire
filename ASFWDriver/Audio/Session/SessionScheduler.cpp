// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "SessionScheduler.hpp"

#include "SessionClock.hpp"

#include "../Protocols/IDeviceProtocol.hpp"

#include "../Core/AudioRuntimeRegistry.hpp"
#include "../Protocols/Duplex/AudioClockConfig.hpp"
#include "../../DeviceProfiles/Audio/ResolvedDevicePolicy.hpp"
#include "../../Logging/Logging.hpp"

#include <utility>

namespace ASFW::Audio::Session {

namespace {

[[nodiscard]] bool IsSupportedClockForRecord(const Discovery::DeviceRecord& record,
                                             const AudioClockConfig& clock) noexcept {
    const auto* policy = DeviceProfiles::Audio::CurrentAudioPolicy(record);
    if (policy != nullptr &&
        (policy->plan.profileBuilder == DeviceProfiles::Audio::ProfileBuilderId::MAudioFireWire1814 ||
         policy->plan.profileBuilder == DeviceProfiles::Audio::ProfileBuilderId::MAudioProjectMix)) {
        return IsSupportedMAudioSpecialClockConfig(clock);
    }
    return IsSupportedAudioClockConfig(clock);
}

// With no clock chosen yet, start at the rate the device is pinned to, or 48 kHz.
[[nodiscard]] uint32_t DefaultStartRate(const Discovery::DeviceRecord& record) noexcept {
    const auto* policy = DeviceProfiles::Audio::CurrentAudioPolicy(record);
    if (policy != nullptr && policy->plan.streamTraits.start.startRatePinHz != 0) {
        return policy->plan.streamTraits.start.startRatePinHz;
    }
    return 48000U;
}

[[nodiscard]] bool IsRetryableStatus(IOReturn status) noexcept {
    return status == kIOReturnTimeout || status == kIOReturnAborted ||
           status == kIOReturnNotReady || status == kIOReturnNoDevice;
}

[[nodiscard]] bool IsRuntimeFault(DuplexRestartReason reason) noexcept {
    switch (reason) {
    case DuplexRestartReason::kRecoverAfterTimingLoss:
    case DuplexRestartReason::kRecoverAfterCycleInconsistent:
    case DuplexRestartReason::kRecoverAfterLockLoss:
    case DuplexRestartReason::kRecoverAfterTxFault:
        return true;
    case DuplexRestartReason::kBusResetRebind:
    case DuplexRestartReason::kInitialStart:
    case DuplexRestartReason::kSampleRateChange:
    case DuplexRestartReason::kClockSourceChange:
    case DuplexRestartReason::kManualReconfigure:
    case DuplexRestartReason::kDeviceConfigChange:
        return false;
    }
    return false;
}

struct DeviceHandle {
    std::optional<Discovery::DeviceRecord> record;
    std::shared_ptr<IDeviceProtocol> protocol;
    FamilyDriver* family{nullptr};

    [[nodiscard]] bool Known() const noexcept { return record.has_value() && family != nullptr; }
};

} // namespace

const char* ToString(SessionState state) noexcept {
    switch (state) {
    case SessionState::Idle: return "idle";
    case SessionState::Restarting: return "restarting";
    case SessionState::Running: return "running";
    case SessionState::Failed: return "failed";
    case SessionState::Faulted: return "faulted";
    }
    return "?";
}

SessionScheduler::SessionScheduler(uint64_t guid, Dependencies dependencies) noexcept
    : guid_(guid),
      deps_(std::move(dependencies)),
      stop_(deps_.registry, deps_.host, deps_.teardown, deps_.teardownAborts),
      restart_(RestartRoutine::Dependencies{
          .registry = deps_.registry,
          .host = deps_.host,
          .hardware = deps_.hardware,
          .stop = stop_,
          .teardown = deps_.teardown,
          .teardownAborts = deps_.teardownAborts,
      }) {
    lock_ = IOLockAlloc();
}

SessionScheduler::~SessionScheduler() noexcept {
    if (lock_ != nullptr) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

bool SessionScheduler::TeardownRequested() const noexcept {
    return deps_.teardown != nullptr && deps_.teardown->load(std::memory_order_acquire);
}

bool SessionScheduler::StartAllowed() const noexcept {
    return deps_.startGuard == nullptr || !*deps_.startGuard || (*deps_.startGuard)(guid_);
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

IOReturn SessionScheduler::Attach() noexcept {
    if (!StartAllowed()) {
        return kIOReturnNotReady;
    }
    if (TeardownRequested()) {
        ASFW_LOG(Audio, "[Session] attach refused by teardown GUID=%llx", guid_);
        return kIOReturnAborted;
    }
    if (IsRetired()) {
        return kIOReturnNoDevice;
    }
    CancelPendingRestart();
    return Submit([](Wanted& wanted, Actual& actual) {
        wanted.halAttached = true;
        // A new attach is a fresh chance for a session that gave up on faults.
        if (actual.state == SessionState::Faulted) {
            actual.state = SessionState::Failed;
        }
        actual.faultFailures = 0;
    });
}

IOReturn SessionScheduler::Detach() noexcept {
    if (TeardownRequested()) {
        ASFW_LOG(Audio, "[Session] detach refused by teardown GUID=%llx", guid_);
        return kIOReturnAborted;
    }
    CancelPendingRestart();
    return Submit([](Wanted& wanted, Actual&) { wanted.halAttached = false; });
}

IOReturn SessionScheduler::ChangeClock(const AudioClockConfig& clock, DuplexRestartReason reason) noexcept {
    if (!StartAllowed()) {
        return kIOReturnNotReady;
    }
    if (TeardownRequested()) {
        ASFW_LOG(Audio, "[Session] clock change refused by teardown GUID=%llx", guid_);
        return kIOReturnAborted;
    }
    const auto record = deps_.registry.SnapshotByGuid(guid_);
    if (!record) {
        return kIOReturnNotReady;
    }
    if (!IsSupportedClockForRecord(*record, clock)) {
        return kIOReturnUnsupported;
    }
    if (IsRetired()) {
        return kIOReturnAborted;
    }
    CancelPendingRestart();

    AudioClockConfig target{};
    const IOReturn status = Submit(
        [&](Wanted& wanted, Actual&) {
            wanted.clock = clock;
            wanted.clockDirty = true;
            wanted.clockReason = reason;
        },
        &target);
    // A later change reached the device first; this one was never applied.
    if (target.sampleRateHz != clock.sampleRateHz) {
        return kIOReturnAborted;
    }
    return status;
}

std::optional<IOReturn> SessionScheduler::RestartRefusal(DuplexRestartReason reason,
                                                        uint64_t observedRun) const noexcept {
    if (!StartAllowed()) {
        return kIOReturnNotReady;
    }
    if (TeardownRequested()) {
        ASFW_LOG(Audio, "[Session] restart refused by teardown GUID=%llx", guid_);
        return kIOReturnAborted;
    }
    if (IsRetired()) {
        return kIOReturnAborted;
    }

    const SessionSnapshot now = Snapshot();
    if (observedRun != 0 && (observedRun != now.run || now.state != SessionState::Running)) {
        ASFW_LOG(Audio, "[Session] stale restart dropped GUID=%llx reason=%u observedRun=%llu run=%llu",
                 guid_, static_cast<unsigned>(reason), observedRun, now.run);
        return kIOReturnAborted;
    }
    if (IsRuntimeFault(reason) && IsReconciling()) {
        // The fault is the running reconcile's own transition.
        ASFW_LOG(Audio, "[Session] fault dropped during reconcile GUID=%llx reason=%u", guid_,
                 static_cast<unsigned>(reason));
        return kIOReturnAborted;
    }
    if (!now.halAttached) {
        // Nothing should run, so there is nothing to recover.
        return kIOReturnUnsupported;
    }
    if (now.state == SessionState::Faulted) {
        return kIOReturnUnsupported;
    }
    if (now.state == SessionState::Failed && !IsRetryableStatus(now.lastStatus)) {
        return now.lastStatus;
    }
    return std::nullopt;
}

IOReturn SessionScheduler::RequestRestart(DuplexRestartReason reason, uint64_t observedRun) noexcept {
    if (const auto refusal = RestartRefusal(reason, observedRun)) {
        return *refusal;
    }
    if (const uint32_t quietMs = RestartQuietPeriodMs(); quietMs != 0) {
        return DeferRestart(reason, observedRun, quietMs);
    }
    return RunRestart(reason, observedRun);
}

IOReturn SessionScheduler::RunRestart(DuplexRestartReason reason, uint64_t observedRun) noexcept {
    (void)observedRun;
    const bool fault = IsRuntimeFault(reason);
    return Submit(
        [&](Wanted& wanted, Actual&) {
            wanted.restart = true;
            wanted.restartIsFault = fault;
            wanted.restartReason = reason;
        },
        nullptr, WaitMode::QueueBehindRunning);
}

uint32_t SessionScheduler::RestartQuietPeriodMs() const noexcept {
    const auto record = deps_.registry.SnapshotByGuid(guid_);
    const auto* policy = record ? DeviceProfiles::Audio::CurrentAudioPolicy(*record) : nullptr;
    const uint32_t quietMs =
        policy != nullptr ? policy->plan.streamTraits.start.restartQuietPeriodMs : 0;
    if (quietMs != 0 && (deps_.timer == nullptr || *deps_.timer == nullptr || deps_.queue == nullptr)) {
        // A composition bug: the family wants its events to settle, but no
        // timer was installed. Restart at once, as before the quiet period.
        ASFW_LOG_RL(Audio, "session/no-timer", 60000, OS_LOG_TYPE_ERROR,
                    "[Session] no timer for a %u ms quiet period GUID=%llx; restarting at once",
                    quietMs, guid_);
        return 0;
    }
    return quietMs;
}

IOReturn SessionScheduler::DeferRestart(DuplexRestartReason reason, uint64_t observedRun,
                                        uint32_t quietMs) noexcept {
    if (lock_ == nullptr) {
        return kIOReturnNoResources;
    }
    IOLockLock(lock_);
    const bool merged = pending_.active;
    const bool untied = observedRun == 0 || (merged && pending_.observedRun == 0);
    if (!merged || observedRun == 0 || pending_.observedRun != 0) {
        // Keep the reason of the strongest request: an untied one (a bus
        // reset) is never replaced by a later fault.
        pending_.reason = reason;
    }
    pending_.observedRun = untied ? 0 : observedRun;
    if (!merged) {
        pending_.runAtRequest = actual_.run;
    }
    pending_.active = true;
    const uint64_t generation = ++pending_.generation;
    const Scheduling::TimerToken previous = pending_.timer;
    pending_.timer = Scheduling::kInvalidTimerToken;
    IOLockUnlock(lock_);

    if (previous != Scheduling::kInvalidTimerToken) {
        (*deps_.timer)->Cancel(previous);
    }
    ArmPendingTimer(quietMs, generation);
    ASFW_LOG(Audio, "[Session] restart pending GUID=%llx reason=%u quiet=%ums%s", guid_,
             static_cast<unsigned>(reason), quietMs, merged ? " (merged)" : "");
    return kIOReturnSuccess;
}

void SessionScheduler::ArmPendingTimer(uint32_t quietMs, uint64_t generation) noexcept {
    std::weak_ptr<SessionScheduler> weak = weak_from_this();
    IODispatchQueue* queue = deps_.queue;
    const Scheduling::TimerToken token = (*deps_.timer)->ScheduleAfter(
        static_cast<uint64_t>(quietMs) * 1'000'000ULL, [weak, queue, generation] {
            // Default queue: never reconcile here, only hand over.
            auto self = weak.lock();
            if (!self) {
                return;
            }
            queue->DispatchAsync(^{
                self->FirePendingRestart(generation);
            });
        });
    IOLockLock(lock_);
    if (pending_.active && pending_.generation == generation) {
        pending_.timer = token;
    }
    IOLockUnlock(lock_);
}

void SessionScheduler::FirePendingRestart(uint64_t generation) noexcept {
    if (lock_ == nullptr) {
        return;
    }
    IOLockLock(lock_);
    if (!pending_.active || pending_.generation != generation) {
        IOLockUnlock(lock_);
        return;
    }
    if (reconciling_) {
        // A reconcile is rebuilding the streams now; wait out another period
        // rather than restart behind it (TCAT: "unless one is running").
        IOLockUnlock(lock_);
        const uint32_t quietMs = RestartQuietPeriodMs();
        if (quietMs != 0) {
            ArmPendingTimer(quietMs, generation);
            return;
        }
        IOLockLock(lock_);
    }
    const PendingRestart pending = pending_;
    pending_.active = false;
    pending_.timer = Scheduling::kInvalidTimerToken;
    const uint64_t runNow = actual_.run;
    IOLockUnlock(lock_);

    if (runNow != pending.runAtRequest) {
        ASFW_LOG(Audio, "[Session] pending restart covered GUID=%llx reason=%u run=%llu->%llu",
                 guid_, static_cast<unsigned>(pending.reason), pending.runAtRequest, runNow);
        return;
    }
    if (const auto refusal = RestartRefusal(pending.reason, pending.observedRun)) {
        ASFW_LOG(Audio, "[Session] pending restart dropped GUID=%llx reason=%u kr=0x%x", guid_,
                 static_cast<unsigned>(pending.reason), *refusal);
        return;
    }
    const IOReturn status = RunRestart(pending.reason, pending.observedRun);
    if (status != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio, "[Session] restart after quiet period failed GUID=%llx reason=%u kr=0x%x",
                       guid_, static_cast<unsigned>(pending.reason), status);
    }
}

void SessionScheduler::CancelPendingRestart() noexcept {
    if (lock_ == nullptr) {
        return;
    }
    IOLockLock(lock_);
    const Scheduling::TimerToken token = pending_.timer;
    pending_.active = false;
    pending_.timer = Scheduling::kInvalidTimerToken;
    ++pending_.generation;
    IOLockUnlock(lock_);
    if (token != Scheduling::kInvalidTimerToken && deps_.timer != nullptr && *deps_.timer != nullptr) {
        (*deps_.timer)->Cancel(token);
    }
}

bool SessionScheduler::HasPendingRestart() const noexcept {
    if (lock_ == nullptr) {
        return false;
    }
    IOLockLock(lock_);
    const bool active = pending_.active;
    IOLockUnlock(lock_);
    return active;
}

void SessionScheduler::Retire() noexcept {
    retired_.store(true, std::memory_order_release);
    CancelPendingRestart();
}

void SessionScheduler::Present() noexcept {
    retired_.store(false, std::memory_order_release);
    if (lock_ == nullptr) {
        return;
    }
    IOLockLock(lock_);
    actual_.faultFailures = 0;
    if (actual_.state == SessionState::Faulted) {
        actual_.state = SessionState::Failed;
    }
    IOLockUnlock(lock_);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool SessionScheduler::IsStreaming() const noexcept {
    return Snapshot().state == SessionState::Running;
}

bool SessionScheduler::IsReconciling() const noexcept {
    if (lock_ == nullptr) {
        return false;
    }
    IOLockLock(lock_);
    const bool reconciling = reconciling_;
    IOLockUnlock(lock_);
    return reconciling;
}

bool SessionScheduler::IsRetired() const noexcept {
    return retired_.load(std::memory_order_acquire);
}

uint64_t SessionScheduler::CurrentRun() const noexcept {
    return Snapshot().run;
}

uint64_t SessionScheduler::RunningRun() const noexcept {
    const SessionSnapshot now = Snapshot();
    return now.state == SessionState::Running ? now.run : kNotRunning;
}

SessionSnapshot SessionScheduler::Snapshot() const noexcept {
    SessionSnapshot out{};
    if (lock_ == nullptr) {
        return out;
    }
    IOLockLock(lock_);
    out.state = actual_.state;
    out.halAttached = wanted_.halAttached;
    out.clockChangePending = wanted_.clockDirty;
    out.run = actual_.run;
    out.desiredClock = wanted_.clock;
    out.appliedClock = actual_.appliedClock;
    out.runtimeCaps = actual_.runtimeCaps;
    out.channels = actual_.channels;
    out.lastStatus = actual_.lastStatus;
    IOLockUnlock(lock_);
    return out;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

template <typename Edit>
IOReturn SessionScheduler::Submit(Edit&& edit, AudioClockConfig* targetOut, WaitMode mode) noexcept {
    if (lock_ == nullptr) {
        return kIOReturnNoResources;
    }
    IOLockLock(lock_);
    edit(wanted_, actual_);
    halAttached_.store(wanted_.halAttached, std::memory_order_release);
    const uint64_t ticket = ++requested_;

    if (reconciling_ && mode == WaitMode::QueueBehindRunning) {
        // The running reconcile loops until it has seen this edit. Not waiting
        // also keeps a request raised on the reconciling thread itself (a
        // callback inside a device stage) from waiting on its own reconcile.
        IOLockUnlock(lock_);
        return kIOReturnSuccess;
    }
    if (reconciling_) {
        // Another caller is reconciling; it loops until it has seen this edit.
        const uint64_t deadline = UptimeMilliseconds() + kWaitTimeoutMs;
        while (completed_ < ticket) {
            IOLockUnlock(lock_);
            if (TeardownRequested()) {
                return kIOReturnAborted;
            }
            if (UptimeMilliseconds() >= deadline) {
                ASFW_LOG_ERROR(Audio, "[Session] request timed out waiting for reconcile GUID=%llx",
                               guid_);
                return kIOReturnTimeout;
            }
            IOSleep(1);
            IOLockLock(lock_);
        }
        const Result* result = FindResultLocked(ticket);
        const IOReturn status = result != nullptr ? result->status : actual_.lastStatus;
        if (targetOut != nullptr) {
            *targetOut = result != nullptr ? result->target : wanted_.clock;
        }
        IOLockUnlock(lock_);
        return status;
    }

    reconciling_ = true;
    std::optional<Result> mine;
    while (true) {
        const uint64_t covering = requested_;
        const Wanted snapshot = wanted_;
        wanted_.clockDirty = false;
        wanted_.restart = false;
        wanted_.restartIsFault = false;
        IOLockUnlock(lock_);

        const IOReturn status = Reconcile(snapshot);

        IOLockLock(lock_);
        completed_ = covering;
        RecordResultLocked(covering, status, snapshot.clock);
        if (!mine) {
            mine = Result{.upTo = covering, .status = status, .target = snapshot.clock};
        }
        if (requested_ == covering) {
            break;
        }
    }
    reconciling_ = false;
    IOLockUnlock(lock_);

    if (targetOut != nullptr) {
        *targetOut = mine->target;
    }
    return mine->status;
}

void SessionScheduler::RecordResultLocked(uint64_t coveredTicket, IOReturn status,
                                          AudioClockConfig target) noexcept {
    if (resultCount_ == kResultHistory) {
        for (uint32_t i = 1; i < kResultHistory; ++i) {
            results_[i - 1] = results_[i];
        }
        --resultCount_;
    }
    results_[resultCount_++] = Result{.upTo = coveredTicket, .status = status, .target = target};
}

const SessionScheduler::Result* SessionScheduler::FindResultLocked(uint64_t ticket) const noexcept {
    for (uint32_t i = 0; i < resultCount_; ++i) {
        if (results_[i].upTo >= ticket) {
            return &results_[i];
        }
    }
    return nullptr;
}

SessionScheduler::Actual SessionScheduler::LoadActual() const noexcept {
    IOLockLock(lock_);
    const Actual actual = actual_;
    IOLockUnlock(lock_);
    return actual;
}

void SessionScheduler::StoreActual(const Actual& actual) noexcept {
    IOLockLock(lock_);
    actual_ = actual;
    IOLockUnlock(lock_);
}

// ---------------------------------------------------------------------------
// Reconcile: make the device match what is wanted
// ---------------------------------------------------------------------------

IOReturn SessionScheduler::Reconcile(const Wanted& wanted) noexcept {
    if (TeardownRequested()) {
        deps_.teardownAborts.fetch_add(1, std::memory_order_acq_rel);
        ASFW_LOG(Audio, "[Session] reconcile refused by teardown GUID=%llx", guid_);
        return kIOReturnAborted;
    }

    const uint64_t startedMs = UptimeMilliseconds();
    const Actual before = LoadActual();

    DeviceHandle device{};
    device.record = deps_.registry.SnapshotByGuid(guid_);
    if (device.record && !Discovery::TryOperationalNodeId(device.record->nodeId).has_value()) {
        device.record.reset();
    }
    device.protocol = deps_.runtime.FindShared(guid_);
    device.family = device.protocol ? device.protocol->AsFamilyDriver() : nullptr;

    const char* action = "none";
    IOReturn status = kIOReturnSuccess;

    if (!wanted.halAttached) {
        if (before.needsStop) {
            action = "stop";
            if (!device.Known()) {
                // Nothing left to stop on a device that is gone; host cleanup
                // belongs to the removal path.
                Actual actual = before;
                actual.needsStop = false;
                actual.state = SessionState::Idle;
                StoreActual(actual);
                status = kIOReturnNoDevice;
            } else {
                status = StopStreams(*device.record, device.protocol);
            }
        }
        if (status == kIOReturnSuccess && wanted.clockDirty) {
            action = before.needsStop ? "stop+apply-clock" : "apply-clock";
            status = device.Known() ? ApplyClockIdle(wanted.clock, device.protocol) : kIOReturnNotReady;
        }
    } else if (before.state == SessionState::Running && !wanted.restart && !wanted.clockDirty) {
        action = "already-running";
    } else if (!device.Known()) {
        action = "start";
        status = kIOReturnNotReady;
    } else {
        action = before.needsStop ? "restart" : "start";
        if (before.needsStop) {
            status = StopStreams(*device.record, device.protocol);
        }
        if (status == kIOReturnSuccess) {
            if (TeardownRequested()) {
                status = kIOReturnAborted;
            } else if (IsRetired() || !halAttached_.load(std::memory_order_acquire)) {
                status = kIOReturnAborted;
            } else if (!StartAllowed()) {
                status = kIOReturnNotReady;
            } else {
                status = StartStreams(wanted, *device.record, device.protocol);
            }
        }
    }

    const Actual after = LoadActual();
    if (wanted.restart && status == kIOReturnSuccess && after.state == SessionState::Running &&
        after.run != before.run && deps_.restartObserver != nullptr && *deps_.restartObserver) {
        (*deps_.restartObserver)(guid_);
    }
    ASFW_LOG(Audio,
             "[Session] GUID=0x%016llx hal=%u clockDirty=%u restart=%u reason=%u action=%{public}s "
             "-> 0x%08x state=%{public}s run=%llu %llums",
             guid_, wanted.halAttached ? 1U : 0U, wanted.clockDirty ? 1U : 0U,
             wanted.restart ? 1U : 0U, static_cast<unsigned>(wanted.restartReason), action, status,
             ToString(after.state), after.run, UptimeMilliseconds() - startedMs);
    return status;
}

IOReturn SessionScheduler::StartStreams(const Wanted& wanted, const Discovery::DeviceRecord& record,
                                        const std::shared_ptr<IDeviceProtocol>& protocol) noexcept {
    Actual actual = LoadActual();

    AudioClockConfig clock{.sampleRateHz = DefaultStartRate(record)};
    if (IsSupportedClockForRecord(record, wanted.clock)) {
        clock = wanted.clock;
    } else if (IsSupportedClockForRecord(record, actual.appliedClock)) {
        clock = actual.appliedClock;
    }
    const DuplexRestartReason reason = wanted.restart     ? wanted.restartReason
                                       : wanted.clockDirty ? wanted.clockReason
                                                           : DuplexRestartReason::kInitialStart;

    ++actual.run;
    actual.state = SessionState::Restarting;
    StoreActual(actual);

    FamilyDriver& family = BindFamily(*protocol);
    const auto result = restart_.Run(RestartRoutine::Request{
        .guid = guid_,
        .record = record,
        .family = &family,
        .irm = deps_.irm != nullptr ? *deps_.irm : nullptr,
        .binding = deps_.bindingSource ? deps_.bindingSource(guid_) : nullptr,
        .clock = clock,
        .reason = reason,
        .superseded = [this] {
            return IsRetired() || !halAttached_.load(std::memory_order_acquire);
        },
    });

    if (result) {
        actual.state = SessionState::Running;
        actual.needsStop = true;
        actual.appliedClock = result->appliedClock;
        actual.runtimeCaps = result->runtimeCaps;
        actual.channels = result->channels;
        actual.lastStatus = kIOReturnSuccess;
        if (wanted.restartIsFault) {
            actual.faultFailures = 0;
        }
        StoreActual(actual);
        return kIOReturnSuccess;
    }

    actual.state = SessionState::Failed;
    actual.needsStop = !result.error().stopped;
    actual.runtimeCaps = result.error().runtimeCaps;
    actual.channels = result.error().channels;
    actual.lastStatus = result.error().status;
    if (wanted.restartIsFault && ++actual.faultFailures >= kMaxFaultRestartFailures) {
        actual.state = SessionState::Faulted;
        ASFW_LOG_ERROR(Audio,
                       "[Session] %u fault recoveries failed in a row; leaving streams stopped "
                       "until the next attach GUID=0x%016llx",
                       actual.faultFailures, guid_);
    }
    ASFW_LOG_ERROR(Audio, "[Session] start failed at %{public}s kr=0x%08x GUID=0x%016llx",
                   result.error().step, result.error().status, guid_);
    StoreActual(actual);
    return result.error().status;
}

FamilyDriver& SessionScheduler::BindFamily(IDeviceProtocol& protocol) noexcept {
    // Callers reach here only for a protocol whose AsFamilyDriver is non-null
    // (DeviceHandle::Known).
    FamilyDriver& family = *protocol.AsFamilyDriver();
    family.SetTeardownCancelToken(deps_.teardown);
    return family;
}

IOReturn SessionScheduler::StopStreams(const Discovery::DeviceRecord& record,
                                       const std::shared_ptr<IDeviceProtocol>& protocol) noexcept {
    Actual actual = LoadActual();
    actual.state = SessionState::Restarting;
    StoreActual(actual);

    FamilyDriver& family = BindFamily(*protocol);
    const IOReturn status = stop_.Run(guid_, record, family, actual.runtimeCaps, actual.channels);
    if (status == kIOReturnSuccess) {
        actual.state = SessionState::Idle;
        actual.needsStop = false;
    } else {
        actual.state = SessionState::Failed;
        ASFW_LOG_ERROR(Audio, "[Session] stop failed kr=0x%08x GUID=0x%016llx", status, guid_);
    }
    actual.lastStatus = status;
    StoreActual(actual);
    return status;
}

IOReturn SessionScheduler::ApplyClockIdle(const AudioClockConfig& clock,
                                          const std::shared_ptr<IDeviceProtocol>& protocol) noexcept {
    Actual actual = LoadActual();
    FamilyDriver& family = BindFamily(*protocol);
    const auto applied = family.ApplyClockIdle(clock);
    if (!applied) {
        actual.state = SessionState::Failed;
        actual.lastStatus = applied.error();
        StoreActual(actual);
        return applied.error();
    }
    actual.state = SessionState::Idle;
    actual.appliedClock = applied->appliedClock;
    actual.runtimeCaps = applied->runtimeCaps;
    actual.lastStatus = kIOReturnSuccess;
    StoreActual(actual);
    return kIOReturnSuccess;
}

} // namespace ASFW::Audio::Session
