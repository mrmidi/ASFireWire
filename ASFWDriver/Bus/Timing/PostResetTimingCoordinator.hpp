#pragma once

// PostResetTimingCoordinator.hpp — Milestone 2 timing core.
//
// Generation-scoped, value-in/value-out timing authority for post-reset bus
// management. Pure state + arithmetic: no DriverKit, no hardware, no locks, no
// allocation. The caller supplies the clock (nowNs) so the same logic runs in
// host tests (HostMonotonicNow) and the live driver (BusResetCoordinator::
// MonotonicNow). See PostResetTiming.hpp for the model and the five invariants.
//
// V0 (this milestone): gate checks only — no real IOTimerDispatchSource. Callers
// poll CheckGate/CheckBMGate at decision points. Deferred-callback timer tokens
// (V1) are intentionally out of scope.

#include <cstdint>

#include "PostResetTiming.hpp"
#include "PostResetTimingState.hpp"

namespace ASFW::Bus::Timing {

class PostResetTimingCoordinator final {
  public:
    PostResetTimingCoordinator() noexcept = default;

    // A new bus-reset edge invalidates all gates from older generations: no gate
    // opens again until OnSelfIDComplete is observed. `generation` is the
    // outgoing/known value and is informational only — the new generation is not
    // yet decoded at the reset edge (invariant 4).
    void OnBusResetStarted(uint32_t generation, uint64_t nowNs) noexcept;

    // Self-identify completion anchors the timing window for `generation` and
    // precomputes the four gate-open deadlines. Call this AT Self-ID completion,
    // BEFORE topology graph construction, so gates stay armed even if the graph
    // build later fails (invariants 1 and 2).
    void OnSelfIDComplete(uint32_t generation, uint64_t selfIdCompleteNs) noexcept;

    // Pure timing check for one gate. Missing anchor → Closed; generation
    // mismatch → ExpiredGeneration; otherwise Open/Closed by nowNs vs deadline.
    [[nodiscard]] TimingGateResult CheckGate(uint32_t generation, TimingGate gate,
                                             uint64_t nowNs) const noexcept;

    // BM gate keyed on candidate class: Incumbent → immediate gate, NonIncumbent
    // → +125 ms gate, NotCandidate → SuppressedByRolePolicy (never permitted).
    [[nodiscard]] TimingGateResult CheckBMGate(uint32_t generation,
                                               BMCandidateClass candidateClass,
                                               uint64_t nowNs) const noexcept;

    [[nodiscard]] TimingGateResult CheckNewIsoAllocationGate(uint32_t generation,
                                                             uint64_t nowNs) const noexcept;

    [[nodiscard]] const PostResetTimingState& State() const noexcept { return state_; }

    // Flat, self-describing snapshot for diagnostics/logging.
    [[nodiscard]] PostResetTimingDiagnostics Snapshot(uint64_t nowNs) const noexcept;

    // Counters surfaced in diagnostics. Incremented by future timer/action
    // consumers (M3+); exposed now so the reporting wiring is complete.
    void RecordStaleTimerFiring() noexcept { ++staleTimerFirings_; }
    void RecordRoleSuppression() noexcept { ++suppressedByRolePolicy_; }
    void RecordGenerationSuppression() noexcept { ++suppressedByGeneration_; }

  private:
    [[nodiscard]] TimingGateResult MakeMissingStateResult(uint32_t generation, TimingGate gate,
                                                          uint64_t nowNs) const noexcept;
    [[nodiscard]] static TimingGateResult MakeGateResult(uint32_t generation, TimingGate gate,
                                                         uint64_t nowNs, uint64_t selfIdCompleteNs,
                                                         uint64_t allowedAtNs) noexcept;

    PostResetTimingState state_{};

    uint32_t staleTimerFirings_{0};
    uint32_t suppressedByRolePolicy_{0};
    uint32_t suppressedByGeneration_{0};
};

} // namespace ASFW::Bus::Timing
