#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct IOLock;

namespace ASFW::Driver {

enum class ControllerState : uint8_t {
    kStopped,
    kStarting,
    kRunning,
    kQuiescing,
    kSuspended,
    kRevoked,
    kFailed,
};

enum class TransitionDisposition : uint8_t {
    kApplied,
    kIdempotent,
    kRejected,
};

struct StateTransition {
    ControllerState from{ControllerState::kStopped};
    ControllerState to{ControllerState::kStopped};
    std::string reason;
    uint64_t timestamp{0};
};

// Thread-safe root runtime state authority.
//
// This class validates the lifecycle graph; it does not merely record whatever
// transition a caller requests. Duplicate requests for the current state are
// idempotent. Illegal requests are rejected without changing state.
class ControllerStateMachine {
public:
    ControllerStateMachine();
    ~ControllerStateMachine();

    ControllerStateMachine(const ControllerStateMachine&) = delete;
    ControllerStateMachine& operator=(const ControllerStateMachine&) = delete;

    [[nodiscard]] ControllerState CurrentState() const;
    [[nodiscard]] std::optional<StateTransition> LastTransition() const;
    [[nodiscard]] bool CanTransitionTo(ControllerState next) const;

    // Returns kApplied for a legal state change, kIdempotent when next already
    // equals the current state, and kRejected for every illegal edge.
    TransitionDisposition TransitionTo(ControllerState next,
                                       std::string_view reason,
                                       uint64_t now);

    // Reserved for construction/test reuse when no runtime resources are live.
    // Runtime teardown must reach kStopped through legal transitions.
    void Reset();

private:
    [[nodiscard]] static bool IsLegalTransition(ControllerState from,
                                                ControllerState to) noexcept;

    mutable IOLock* lock_{nullptr};
    ControllerState state_{ControllerState::kStopped};
    std::optional<StateTransition> last_;
};

std::string_view ToString(ControllerState state);
std::string_view ToString(TransitionDisposition disposition);

} // namespace ASFW::Driver
