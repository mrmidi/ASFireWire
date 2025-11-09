#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ASFW::Driver {

enum class ControllerState : uint8_t {
    kStopped,
    kStarting,
    kRunning,
    kQuiescing,
    kFailed
};

struct StateTransition {
    ControllerState from{ControllerState::kStopped};
    ControllerState to{ControllerState::kStopped};
    std::string reason;
    uint64_t timestamp{0};
};

// Lightweight state tracker used by ControllerCore and surfaced via the IIG.
class ControllerStateMachine {
public:
    ControllerStateMachine();

    ControllerState CurrentState() const;
    std::optional<StateTransition> LastTransition() const;

    void Reset();
    void TransitionTo(ControllerState next, std::string_view reason, uint64_t now);

private:
    ControllerState state_;
    std::optional<StateTransition> last_;
};

std::string_view ToString(ControllerState state);

} // namespace ASFW::Driver

