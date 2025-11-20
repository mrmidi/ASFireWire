#include "ControllerStateMachine.hpp"

#include <string>

namespace ASFW::Driver {

namespace {
constexpr std::string_view kStoppedStr{"Stopped"};
constexpr std::string_view kStartingStr{"Starting"};
constexpr std::string_view kRunningStr{"Running"};
constexpr std::string_view kQuiescingStr{"Quiescing"};
constexpr std::string_view kFailedStr{"Failed"};
} // namespace

ControllerStateMachine::ControllerStateMachine()
    : state_(ControllerState::kStopped) {}

ControllerState ControllerStateMachine::CurrentState() const {
    return state_;
}

std::optional<StateTransition> ControllerStateMachine::LastTransition() const {
    return last_;
}

void ControllerStateMachine::Reset() {
    state_ = ControllerState::kStopped;
    last_.reset();
}

void ControllerStateMachine::TransitionTo(ControllerState next, std::string_view reason, uint64_t now) {
    StateTransition transition{state_, next, std::string(reason), now};
    last_ = transition;
    state_ = next;
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
    case ControllerState::kFailed:
        return kFailedStr;
    }
    return kFailedStr;
}

} // namespace ASFW::Driver

