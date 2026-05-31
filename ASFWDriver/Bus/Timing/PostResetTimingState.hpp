#pragma once

// PostResetTimingState.hpp — value types for the post-reset timing core.
// Plain data; no DriverKit, no hardware, no allocation. See PostResetTiming.hpp
// for the model and invariants.

#include <cstdint>

#include "PostResetTiming.hpp"

namespace ASFW::Bus::Timing {

// Outcome of a single gate check. Self-describing for diagnostics: carries the
// inputs (generation/now), the anchor (selfIdCompleteNs), and the computed
// allowedAtNs/remainingNs so neither callers nor the report need extra math.
struct TimingGateResult {
    TimingGateState state{TimingGateState::Unknown};
    TimingGate gate{TimingGate::NewIsoAllocation};

    uint32_t generation{0};

    uint64_t nowNs{0};
    uint64_t selfIdCompleteNs{0};
    uint64_t allowedAtNs{0};

    uint64_t remainingNs{0};
    uint64_t ageSinceSelfIdNs{0};

    bool allowed{false};
};

// Generation-scoped anchor + precomputed gate-open deadlines. Rebuilt wholesale
// on every reset edge / Self-ID completion (never mutated field-by-field), so a
// stale generation can never leave a half-updated deadline behind.
struct PostResetTimingState {
    uint32_t generation{0};

    bool valid{false};
    bool selfIdComplete{false};

    uint64_t selfIdCompleteNs{0};

    uint64_t bmIncumbentAllowedNs{0};
    uint64_t bmNonIncumbentAllowedNs{0};
    uint64_t irmFallbackAllowedNs{0};
    uint64_t newIsoAllocationAllowedNs{0};

    uint64_t createdAtNs{0};
    uint64_t invalidatedAtNs{0};
};

// Flat snapshot for diagnostics/logging (PostResetTimingCoordinator::Snapshot).
// Field-for-field parallel to the ASFWDiagPostResetTiming wire struct, so the
// DiagnosticsService collector is a straight copy.
struct PostResetTimingDiagnostics {
    bool valid{false};
    bool selfIdComplete{false};

    uint32_t generation{0};

    uint64_t selfIdCompleteNs{0};
    uint64_t nowNs{0};
    uint64_t ageSinceSelfIdNs{0};

    TimingGateState incumbentBMGate{TimingGateState::Unknown};
    TimingGateState nonIncumbentBMGate{TimingGateState::Unknown};
    TimingGateState irmFallbackGate{TimingGateState::Unknown};
    TimingGateState newIsoAllocationGate{TimingGateState::Unknown};

    uint64_t nonIncumbentBMRemainingNs{0};
    uint64_t irmFallbackRemainingNs{0};
    uint64_t newIsoAllocationRemainingNs{0};

    uint32_t staleTimerFirings{0};
    uint32_t suppressedByRolePolicy{0};
    uint32_t suppressedByGeneration{0};
};

} // namespace ASFW::Bus::Timing
