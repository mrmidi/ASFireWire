#include "RoleCoordinator.hpp"

namespace ASFW::Driver::Role {

void RoleCoordinator::OnTopologyChanged(uint32_t generation, const TopologySnapshot& topo) {
    const uint64_t fp = TopologyFingerprint(topo);

    // Reset the same-topology retry counter only when the physical topology
    // actually changed (Linux resets bm_retries on topology change).
    if (!haveFingerprint_ || (fp != topologyFingerprint_)) {
        topologyFingerprint_ = fp;
        haveFingerprint_ = true;
        resetRetries_ = 0;
    }

    // New generation ⇒ fresh evidence. Stale BIB/cycle results for older
    // generations must never be applied to this one.
    generation_ = generation;
    topo_ = topo;
    haveTopology_ = true;
    rootCap_ = RootCapability::Unknown;
    cycles_ = CycleObservation{};

    Reevaluate();
}

void RoleCoordinator::OnRootCapability(uint32_t generation, RootCapability verdict) {
    if (!haveTopology_ || (generation != generation_)) {
        return; // stale or pre-topology: drop by construction
    }
    rootCap_ = verdict;
    Reevaluate();
}

void RoleCoordinator::OnCycleStartEvidence(uint32_t generation, CycleObservation obs) {
    if (!haveTopology_ || (generation != generation_)) {
        return; // stale: drop
    }
    cycles_ = obs;
    Reevaluate();
}

void RoleCoordinator::OnResetComplete(uint32_t generation) {
    // Reserved hook (FW-9). Intentionally a no-op in the skeleton: a role-policy
    // reset produces a new generation/topology, which arrives via
    // OnTopologyChanged. Kept so the call-site boundary is stable for FW-9.
    (void)generation;
}

void RoleCoordinator::Reevaluate() {
    const RoleInputs in{
        .generation = generation_,
        .topo = haveTopology_ ? &topo_ : nullptr,
        .rootCap = rootCap_,
        .cycles = cycles_,
        .localCmcCapable = localCmcCapable_,
        .resetRetriesThisTopology = resetRetries_,
    };
    lastAction_ = policy_(in);
    Dispatch();
}

void RoleCoordinator::Dispatch() {
    switch (lastAction_.kind) {
        case RoleAction::Kind::None:
        case RoleAction::Kind::DeferForEvidence:
        case RoleAction::Kind::MarkRootBadOrUnknown:
            // No hardware side effect. (Defer is rescheduled by the live wiring;
            // MarkRootBadOrUnknown only records state for the next evaluation.)
            break;

        case RoleAction::Kind::EnableLocalCycleMaster:
            if (executors_.contender != nullptr) {
                executors_.contender->EnableLocalCycleMaster(generation_);
            }
            break;

        case RoleAction::Kind::EnableRemoteCycleMaster:
            if (executors_.csr != nullptr) {
                executors_.csr->EnableRemoteCycleMaster(lastAction_.targetRoot, generation_);
            }
            break;

        case RoleAction::Kind::ForceRootAndReset:
            if (resetRetries_ >= kMaxSameTopologyResets) {
                lastAction_.kind = RoleAction::Kind::None;
                lastAction_.reason = "ping-pong guard: max same-topology resets reached";
                break;
            }
            if (executors_.reset != nullptr) {
                executors_.reset->ForceRootAndReset(lastAction_.targetRoot, lastAction_.reset,
                                                    lastAction_.gapCount, generation_);
            }
            ++resetRetries_;
            break;

        case RoleAction::Kind::ClearContenderAndDelegate:
            if (resetRetries_ >= kMaxSameTopologyResets) {
                lastAction_.kind = RoleAction::Kind::None;
                lastAction_.reason = "ping-pong guard: max same-topology resets reached";
                break;
            }
            if (executors_.contender != nullptr) {
                executors_.contender->ClearLocalContenderAndDelegate(lastAction_.targetRoot,
                                                                     generation_);
            }
            ++resetRetries_;
            break;
    }
}

uint64_t RoleCoordinator::TopologyFingerprint(const TopologySnapshot& topo) noexcept {
    // Skeleton fingerprint of the role-relevant physical topology. A full
    // Self-ID digest can replace this later (FW-9) without changing the boundary.
    const auto orSentinel = [](const std::optional<uint8_t>& v) -> uint64_t {
        return v.has_value() ? v.value() : uint64_t{0xFF};
    };
    uint64_t fp = 0;
    fp = (fp << 8) | topo.nodeCount;
    fp = (fp << 8) | orSentinel(topo.rootNodeId);
    fp = (fp << 8) | orSentinel(topo.irmNodeId);
    fp = (fp << 8) | orSentinel(topo.localNodeId);
    fp = (fp << 8) | (topo.gapCountConsistent ? 1ULL : 0ULL);
    fp = (fp << 8) | topo.gapCount;
    return fp;
}

} // namespace ASFW::Driver::Role
