#pragma once

// IsoAllocationGate.hpp — central guard for new isochronous resource allocation.
//
// Combines the +1000 ms post-reset timing gate (IEEE 1394-2008 Annex H / §8.x)
// with a topology-validity precondition. Pure, inline, value-in/value-out.
//
// MILESTONE 2 SCOPE: this helper is surfaced in diagnostics only. It is NOT yet
// enforced on the live audio-allocation path — wiring it into the stream
// reservation path is deliberately deferred (see the plan / out-of-scope list)
// so M2 stays non-behavioral.

#include <cstdint>

#include "PostResetTiming.hpp"
#include "PostResetTimingCoordinator.hpp"

namespace ASFW::Bus::Timing {

enum class IsoAllocationGateStatus : uint8_t {
    Allowed = 0,
    WaitingForOneSecondGate = 1,
    NoSelfIDCompletion = 2,
    GenerationMismatch = 3,
    TopologyInvalid = 4,
};

struct IsoAllocationGateResult {
    IsoAllocationGateStatus status{IsoAllocationGateStatus::NoSelfIDCompletion};
    uint32_t generation{0};
    uint64_t remainingNs{0};
};

[[nodiscard]] inline IsoAllocationGateResult
CheckIsoAllocationAllowed(const PostResetTimingCoordinator& timing, uint32_t generation,
                          bool topologyValid, uint64_t nowNs) noexcept {
    IsoAllocationGateResult result{};
    result.generation = generation;

    // A valid topology is required even after the timing gate opens (invariant 2:
    // valid topology does not imply readiness, and here readiness needs topology).
    if (!topologyValid) {
        result.status = IsoAllocationGateStatus::TopologyInvalid;
        return result;
    }

    if (!timing.State().valid || !timing.State().selfIdComplete) {
        result.status = IsoAllocationGateStatus::NoSelfIDCompletion;
        return result;
    }

    const TimingGateResult gate = timing.CheckGate(generation, TimingGate::NewIsoAllocation, nowNs);
    result.remainingNs = gate.remainingNs;

    if (gate.state == TimingGateState::ExpiredGeneration) {
        result.status = IsoAllocationGateStatus::GenerationMismatch;
        return result;
    }
    if (!gate.allowed) {
        result.status = IsoAllocationGateStatus::WaitingForOneSecondGate;
        return result;
    }

    result.status = IsoAllocationGateStatus::Allowed;
    return result;
}

} // namespace ASFW::Bus::Timing
