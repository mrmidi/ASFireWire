#pragma once

// PostResetTiming.hpp — Milestone 2 timing core (constants + enums).
//
// A central post-reset settling model anchored to Self-ID completion. It only
// answers "given this generation and now, is action X allowed yet?" — it performs
// NO bus management: no election, no IRM mutation, no force-root, no gap-count
// change, no remote STATE_SET.cmstr. It is consumed later by the Role/BM/Isoch
// layers; here it just prevents them from acting too early.
//
// IEEE 1394-2008 post-reset timing (Annex H / §8.x), anchored to self-identify
// completion (T):
//   T + 0 ms      incumbent bus manager may contend; passive/topology work runs
//   T + 125 ms    a non-incumbent bus-manager candidate may contend
//   T + 625 ms    IRM fallback management may be checked
//   T + 1000 ms   new isochronous resource allocation may begin
//
// Invariants (also restated on PostResetTimingCoordinator):
//   1. Timing is anchored to Self-ID completion, not topology validation.
//   2. A valid topology does NOT imply BM/IRM actions are allowed yet.
//   3. Every gate is generation-scoped.
//   4. A newer bus reset invalidates all gates from older generations.
//   5. Gates only permit; they never decide policy or trigger actions.

#include <cstdint>

namespace ASFW::Bus::Timing {

inline constexpr uint64_t kNanosecondsPerMillisecond = 1'000'000ULL;

// Non-incumbent bus-manager contention delay after Self-ID completion.
inline constexpr uint64_t kBMNonIncumbentDelayNs = 125ULL * kNanosecondsPerMillisecond;

// IRM fallback-management check delay after Self-ID completion.
inline constexpr uint64_t kIRMFallbackDelayNs = 625ULL * kNanosecondsPerMillisecond;

// New isochronous resource allocation delay after Self-ID completion.
inline constexpr uint64_t kNewIsoAllocationDelayNs = 1000ULL * kNanosecondsPerMillisecond;

// The post-reset actions that are time-gated. Each maps to one delay above;
// BMIncumbentContention is permitted immediately at Self-ID completion (T+0).
enum class TimingGate : uint8_t {
    BMIncumbentContention = 0,
    BMNonIncumbentContention = 1,
    IRMFallbackCheck = 2,
    NewIsoAllocation = 3,
};

// Result classification for a gate check. Open/Closed are pure timing; the
// Suppressed*/ExpiredGeneration values let diagnostics distinguish "not yet"
// from "never (for this generation/role)".
enum class TimingGateState : uint8_t {
    Unknown = 0,
    Closed = 1,            // anchored, but the delay has not elapsed yet
    Open = 2,              // delay elapsed; timing permits the action
    ExpiredGeneration = 3, // caller's generation != the anchored generation
    SuppressedByRolePolicy = 4,
    SuppressedByTopology = 5,
};

// BM candidate classification supplied by the caller (derived from RoleMode +
// current BM ownership). Selects which BM gate applies; NotCandidate is never
// permitted, regardless of timing.
enum class BMCandidateClass : uint8_t {
    NotCandidate = 0,
    Incumbent = 1,
    NonIncumbent = 2,
};

[[nodiscard]] constexpr const char* TimingGateStateString(TimingGateState state) noexcept {
    switch (state) {
    case TimingGateState::Unknown:
        return "Unknown";
    case TimingGateState::Closed:
        return "Closed";
    case TimingGateState::Open:
        return "Open";
    case TimingGateState::ExpiredGeneration:
        return "ExpiredGeneration";
    case TimingGateState::SuppressedByRolePolicy:
        return "SuppressedByRolePolicy";
    case TimingGateState::SuppressedByTopology:
        return "SuppressedByTopology";
    }
    return "?";
}

[[nodiscard]] constexpr const char* TimingGateString(TimingGate gate) noexcept {
    switch (gate) {
    case TimingGate::BMIncumbentContention:
        return "BMIncumbentContention";
    case TimingGate::BMNonIncumbentContention:
        return "BMNonIncumbentContention";
    case TimingGate::IRMFallbackCheck:
        return "IRMFallbackCheck";
    case TimingGate::NewIsoAllocation:
        return "NewIsoAllocation";
    }
    return "?";
}

} // namespace ASFW::Bus::Timing
