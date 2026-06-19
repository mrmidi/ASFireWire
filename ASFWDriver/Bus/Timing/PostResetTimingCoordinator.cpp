// PostResetTimingCoordinator.cpp — see PostResetTiming.hpp for the model.
//
// IEEE 1394-2008 post-reset timing (Annex H / §8.x): incumbent BM may contend at
// self-identify completion, a non-incumbent candidate waits 125 ms, IRM fallback
// management is checked after 625 ms, and new isochronous allocations are held
// until one second after self-identify completion.

#include "PostResetTimingCoordinator.hpp"

namespace ASFW::Bus::Timing {

void PostResetTimingCoordinator::OnBusResetStarted(uint32_t generation, uint64_t nowNs) noexcept {
    // New reset edge invalidates all prior gates. No Annex H action is permitted
    // again until Self-ID completion is observed for the new generation.
    state_ = PostResetTimingState{};
    state_.generation = generation;
    state_.valid = false;
    state_.selfIdComplete = false;
    state_.createdAtNs = nowNs;
    state_.invalidatedAtNs = nowNs;
}

void PostResetTimingCoordinator::OnSelfIDComplete(uint32_t generation,
                                                  uint64_t selfIdCompleteNs) noexcept {
    state_ = PostResetTimingState{};
    state_.generation = generation;
    state_.valid = true;
    state_.selfIdComplete = true;
    state_.selfIdCompleteNs = selfIdCompleteNs;
    state_.createdAtNs = selfIdCompleteNs;

    state_.bmIncumbentAllowedNs = selfIdCompleteNs;
    state_.bmNonIncumbentAllowedNs = selfIdCompleteNs + kBMNonIncumbentDelayNs;
    state_.irmFallbackAllowedNs = selfIdCompleteNs + kIRMFallbackDelayNs;
    state_.newIsoAllocationAllowedNs = selfIdCompleteNs + kNewIsoAllocationDelayNs;
}

TimingGateResult PostResetTimingCoordinator::MakeMissingStateResult(uint32_t generation,
                                                                    TimingGate gate,
                                                                    uint64_t nowNs) const noexcept {
    TimingGateResult result{};
    result.generation = generation;
    result.gate = gate;
    result.nowNs = nowNs;
    result.state = TimingGateState::Closed;
    result.allowed = false;
    return result;
}

TimingGateResult PostResetTimingCoordinator::MakeGateResult(uint32_t generation, TimingGate gate,
                                                            uint64_t nowNs,
                                                            uint64_t selfIdCompleteNs,
                                                            uint64_t allowedAtNs) noexcept {
    TimingGateResult result{};
    result.generation = generation;
    result.gate = gate;
    result.nowNs = nowNs;
    result.selfIdCompleteNs = selfIdCompleteNs;
    result.allowedAtNs = allowedAtNs;
    result.ageSinceSelfIdNs = nowNs >= selfIdCompleteNs ? nowNs - selfIdCompleteNs : 0;

    if (nowNs >= allowedAtNs) {
        result.state = TimingGateState::Open;
        result.allowed = true;
        result.remainingNs = 0;
    } else {
        result.state = TimingGateState::Closed;
        result.allowed = false;
        result.remainingNs = allowedAtNs - nowNs;
    }
    return result;
}

TimingGateResult PostResetTimingCoordinator::CheckGate(uint32_t generation, TimingGate gate,
                                                       uint64_t nowNs) const noexcept {
    if (!state_.valid || !state_.selfIdComplete) {
        return MakeMissingStateResult(generation, gate, nowNs);
    }

    if (generation != state_.generation) {
        TimingGateResult result{};
        result.generation = generation;
        result.gate = gate;
        result.nowNs = nowNs;
        result.selfIdCompleteNs = state_.selfIdCompleteNs;
        result.state = TimingGateState::ExpiredGeneration;
        result.allowed = false;
        return result;
    }

    uint64_t allowedAtNs = state_.selfIdCompleteNs;
    switch (gate) {
    case TimingGate::BMIncumbentContention:
        allowedAtNs = state_.bmIncumbentAllowedNs;
        break;
    case TimingGate::BMNonIncumbentContention:
        allowedAtNs = state_.bmNonIncumbentAllowedNs;
        break;
    case TimingGate::IRMFallbackCheck:
        allowedAtNs = state_.irmFallbackAllowedNs;
        break;
    case TimingGate::NewIsoAllocation:
        allowedAtNs = state_.newIsoAllocationAllowedNs;
        break;
    }

    return MakeGateResult(generation, gate, nowNs, state_.selfIdCompleteNs, allowedAtNs);
}

TimingGateResult PostResetTimingCoordinator::CheckBMGate(uint32_t generation,
                                                         BMCandidateClass candidateClass,
                                                         uint64_t nowNs) const noexcept {
    switch (candidateClass) {
    case BMCandidateClass::Incumbent:
        return CheckGate(generation, TimingGate::BMIncumbentContention, nowNs);
    case BMCandidateClass::NonIncumbent:
        return CheckGate(generation, TimingGate::BMNonIncumbentContention, nowNs);
    case BMCandidateClass::NotCandidate:
    default: {
        // Not a BM candidate at all: report the timing for context, but the
        // action is suppressed by role policy regardless of the clock.
        TimingGateResult result =
            CheckGate(generation, TimingGate::BMNonIncumbentContention, nowNs);
        result.state = TimingGateState::SuppressedByRolePolicy;
        result.allowed = false;
        return result;
    }
    }
}

TimingGateResult
PostResetTimingCoordinator::CheckNewIsoAllocationGate(uint32_t generation,
                                                      uint64_t nowNs) const noexcept {
    return CheckGate(generation, TimingGate::NewIsoAllocation, nowNs);
}

PostResetTimingDiagnostics PostResetTimingCoordinator::Snapshot(uint64_t nowNs) const noexcept {
    PostResetTimingDiagnostics d{};
    d.valid = state_.valid;
    d.selfIdComplete = state_.selfIdComplete;
    d.generation = state_.generation;
    d.selfIdCompleteNs = state_.selfIdCompleteNs;
    d.nowNs = nowNs;

    // Evaluate each gate against the anchored generation so the snapshot reflects
    // the current window (not a caller-supplied generation).
    const uint32_t gen = state_.generation;
    const TimingGateResult inc = CheckGate(gen, TimingGate::BMIncumbentContention, nowNs);
    const TimingGateResult nonInc = CheckGate(gen, TimingGate::BMNonIncumbentContention, nowNs);
    const TimingGateResult irm = CheckGate(gen, TimingGate::IRMFallbackCheck, nowNs);
    const TimingGateResult iso = CheckGate(gen, TimingGate::NewIsoAllocation, nowNs);

    d.ageSinceSelfIdNs = inc.ageSinceSelfIdNs;
    d.incumbentBMGate = inc.state;
    d.nonIncumbentBMGate = nonInc.state;
    d.irmFallbackGate = irm.state;
    d.newIsoAllocationGate = iso.state;

    d.nonIncumbentBMRemainingNs = nonInc.remainingNs;
    d.irmFallbackRemainingNs = irm.remainingNs;
    d.newIsoAllocationRemainingNs = iso.remainingNs;

    d.staleTimerFirings = staleTimerFirings_;
    d.suppressedByRolePolicy = suppressedByRolePolicy_;
    d.suppressedByGeneration = suppressedByGeneration_;
    return d;
}

} // namespace ASFW::Bus::Timing
