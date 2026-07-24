#include "ControllerStateMachine.hpp"

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
    IOLock* lock_{nullptr};
};

constexpr std::string_view kStoppedStr{"Stopped"};
constexpr std::string_view kStartingStr{"Starting"};
constexpr std::string_view kRunningStr{"Running"};
constexpr std::string_view kQuiescingStr{"Quiescing"};
constexpr std::string_view kSuspendedStr{"Suspended"};
constexpr std::string_view kRevokedStr{"Revoked"};
constexpr std::string_view kFailedStr{"Failed"};
constexpr std::string_view kAppliedStr{"Applied"};
constexpr std::string_view kIdempotentStr{"Idempotent"};
constexpr std::string_view kRejectedStr{"Rejected"};

} // namespace

ControllerStateMachine::ControllerStateMachine() : lock_(IOLockAlloc()) {}

ControllerStateMachine::~ControllerStateMachine() {
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

ControllerState ControllerStateMachine::CurrentState() const {
    IOLockGuard guard(lock_);
    return state_;
}

std::optional<StateTransition> ControllerStateMachine::LastTransition() const {
    IOLockGuard guard(lock_);
    return last_;
}

bool ControllerStateMachine::CanTransitionTo(ControllerState next) const {
    IOLockGuard guard(lock_);
    return next == state_ || IsLegalTransition(state_, next);
}

TransitionDisposition ControllerStateMachine::TransitionTo(ControllerState next,
                                                           std::string_view reason,
                                                           uint64_t now) {
    IOLockGuard guard(lock_);

    if (next == state_) {
        return TransitionDisposition::kIdempotent;
    }

    if (!IsLegalTransition(state_, next)) {
        return TransitionDisposition::kRejected;
    }

    last_ = StateTransition{state_, next, std::string(reason), now};
    state_ = next;
    return TransitionDisposition::kApplied;
}

void ControllerStateMachine::Reset() {
    IOLockGuard guard(lock_);
    state_ = ControllerState::kStopped;
    last_.reset();
}

bool ControllerStateMachine::IsLegalTransition(ControllerState from,
                                               ControllerState to) noexcept {
    switch (from) {
    case ControllerState::kStopped:
        return to == ControllerState::kStarting;
    case ControllerState::kStarting:
        return to == ControllerState::kRunning || to == ControllerState::kFailed;
    case ControllerState::kRunning:
        return to == ControllerState::kQuiescing || to == ControllerState::kRevoked;
    case ControllerState::kQuiescing:
        return to == ControllerState::kStopped || to == ControllerState::kSuspended;
    case ControllerState::kSuspended:
        return to == ControllerState::kStarting;
    case ControllerState::kRevoked:
        return to == ControllerState::kStopped;
    case ControllerState::kFailed:
        return to == ControllerState::kQuiescing;
    }
    return false;
}

std::string_view ToString(ControllerState state) {
    switch (state) {
    case ControllerState::kStopped:
        return kStoppedStr;
    case ControllerState::kStarting:
        return kStartingStr;
    case ControllerState::kRunning:
        return kRunningStr;
    case ControllerState::kQuiescing:
        return kQuiescingStr;
    case ControllerState::kSuspended:
        return kSuspendedStr;
    case ControllerState::kRevoked:
        return kRevokedStr;
    case ControllerState::kFailed:
        return kFailedStr;
    }
    return kFailedStr;
}

std::string_view ToString(TransitionDisposition disposition) {
    switch (disposition) {
    case TransitionDisposition::kApplied:
        return kAppliedStr;
    case TransitionDisposition::kIdempotent:
        return kIdempotentStr;
    case TransitionDisposition::kRejected:
        return kRejectedStr;
    }
    return kRejectedStr;
}

} // namespace ASFW::Driver
