#include "RuntimeLifecycleCoordinator.hpp"

#include <DriverKit/IOLib.h>

namespace ASFW::Driver {

namespace {
class IOLockGuard final {
public:
    explicit IOLockGuard(IOLock* lock) noexcept : lock_(lock) {
        if (lock_) {
            IOLockLock(lock_);
        }
    }
    ~IOLockGuard() {
        if (lock_) {
            IOLockUnlock(lock_);
        }
    }
    IOLockGuard(const IOLockGuard&) = delete;
    IOLockGuard& operator=(const IOLockGuard&) = delete;

private:
    IOLock* lock_;
};
} // namespace

RuntimeLifecycleCoordinator::RuntimeLifecycleCoordinator(
    std::shared_ptr<ControllerStateMachine> stateMachine)
    : stateMachine_(std::move(stateMachine)), lock_(IOLockAlloc()) {}

RuntimeLifecycleCoordinator::~RuntimeLifecycleCoordinator() {
    if (lock_) {
        IOLockFree(lock_);
    }
}

bool RuntimeLifecycleCoordinator::BeginStart(std::string_view reason, uint64_t now) noexcept {
    IOLockGuard guard(lock_);
    if (!stateMachine_) {
        return false;
    }
    const auto disposition = stateMachine_->TransitionTo(ControllerState::kStarting, reason, now);
    if (disposition == TransitionDisposition::kApplied) {
        completedStartStage_ = StartStage::kNone;
        return true;
    }
    // A duplicate Start request is state-idempotent, but it must not rerun the
    // resource pipeline while the original start still owns it.
    return false;
}

void RuntimeLifecycleCoordinator::MarkStageComplete(StartStage stage) noexcept {
    IOLockGuard guard(lock_);
    if (static_cast<uint8_t>(stage) > static_cast<uint8_t>(completedStartStage_)) {
        completedStartStage_ = stage;
    }
}

bool RuntimeLifecycleCoordinator::CompleteStart(std::string_view reason, uint64_t now) noexcept {
    IOLockGuard guard(lock_);
    if (!stateMachine_) {
        return false;
    }
    const auto disposition = stateMachine_->TransitionTo(ControllerState::kRunning, reason, now);
    if (disposition == TransitionDisposition::kApplied ||
        disposition == TransitionDisposition::kIdempotent) {
        if (static_cast<uint8_t>(StartStage::kRunning) >
            static_cast<uint8_t>(completedStartStage_)) {
            completedStartStage_ = StartStage::kRunning;
        }
        return true;
    }
    return false;
}

std::optional<QuiescePlan>
RuntimeLifecycleCoordinator::BeginFailedStart(std::string_view reason, uint64_t now) noexcept {
    IOLockGuard guard(lock_);
    if (!stateMachine_ ||
        stateMachine_->TransitionTo(ControllerState::kFailed, reason, now) ==
            TransitionDisposition::kRejected) {
        return std::nullopt;
    }
    const auto disposition =
        stateMachine_->TransitionTo(ControllerState::kQuiescing, "failed-start cleanup", now);
    if (disposition == TransitionDisposition::kRejected) {
        return std::nullopt;
    }
    QuiescePlan plan{.reason = QuiesceReason::kStartFailure,
                     .stateBefore = ControllerState::kFailed,
                     .completedStartStage = completedStartStage_,
                     .runTeardown = !teardownActive_ && disposition == TransitionDisposition::kApplied};
    if (plan.runTeardown) {
        teardownActive_ = true;
    }
    return plan;
}

std::optional<QuiescePlan> RuntimeLifecycleCoordinator::BeginQuiesce(
    QuiesceReason reason, std::string_view detail, uint64_t now) noexcept {
    IOLockGuard guard(lock_);
    if (!stateMachine_) {
        return std::nullopt;
    }

    const ControllerState stateBefore = stateMachine_->CurrentState();
    const ControllerState target = IsProviderRevocation(reason) ? ControllerState::kRevoked
                                                                 : ControllerState::kQuiescing;
    const auto disposition = stateMachine_->TransitionTo(target, detail, now);
    if (disposition == TransitionDisposition::kRejected) {
        return std::nullopt;
    }

    QuiescePlan plan{.reason = reason,
                     .stateBefore = stateBefore,
                     .completedStartStage = completedStartStage_,
                     .runTeardown = !teardownActive_ && disposition == TransitionDisposition::kApplied,
                     .revokeImmediately = IsProviderRevocation(reason)};
    if (plan.runTeardown) {
        teardownActive_ = true;
    }
    return plan;
}

void RuntimeLifecycleCoordinator::CompleteQuiesce(const QuiescePlan& plan,
                                                  std::string_view detail,
                                                  uint64_t now) noexcept {
    IOLockGuard guard(lock_);
    if (!stateMachine_ || !plan.runTeardown) {
        return;
    }

    teardownActive_ = false;
    const ControllerState state = stateMachine_->CurrentState();
    const ControllerState target = state == ControllerState::kRevoked
                                       ? ControllerState::kStopped
                                       : (IsSuspendReason(plan.reason) ? ControllerState::kSuspended
                                                                       : ControllerState::kStopped);
    (void)stateMachine_->TransitionTo(target, detail, now);
    if (target == ControllerState::kStopped) {
        completedStartStage_ = StartStage::kNone;
    }
}

bool RuntimeLifecycleCoordinator::AdmitsNormalWork() const noexcept {
    return CurrentState() == ControllerState::kRunning;
}

bool RuntimeLifecycleCoordinator::AdmitsBringupInterrupts() const noexcept {
    const auto state = CurrentState();
    return state == ControllerState::kStarting || state == ControllerState::kRunning;
}

ControllerState RuntimeLifecycleCoordinator::CurrentState() const noexcept {
    return stateMachine_ ? stateMachine_->CurrentState() : ControllerState::kStopped;
}

StartStage RuntimeLifecycleCoordinator::CompletedStartStage() const noexcept {
    IOLockGuard guard(lock_);
    return completedStartStage_;
}

bool RuntimeLifecycleCoordinator::IsSuspendReason(QuiesceReason reason) noexcept {
    return reason == QuiesceReason::kSystemSuspend || reason == QuiesceReason::kWakeRebuild;
}

bool RuntimeLifecycleCoordinator::IsProviderRevocation(QuiesceReason reason) noexcept {
    return reason == QuiesceReason::kProviderRevoked;
}

} // namespace ASFW::Driver
