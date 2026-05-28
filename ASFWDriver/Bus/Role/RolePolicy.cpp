#include "RolePolicy.hpp"

namespace ASFW::Driver::Role {

RoleAction EvaluateRolePolicy(const RoleInputs& in) noexcept {
    RoleAction action{};

    // No usable topology yet → nothing to decide.
    if ((in.topo == nullptr) || !in.topo->localNodeId.has_value() ||
        !in.topo->rootNodeId.has_value()) {
        action.kind = RoleAction::Kind::None;
        action.reason = "no topology";
        return action;
    }

    // SKELETON (FW-6): until root-capability (FW-8) and cycle-start (FW-7)
    // evidence exist, defer instead of guessing. FW-9 replaces the block below
    // with the full matrix in Linux bm_work decision ORDER:
    //   gap-inconsistent → root-null → not-probed-defer → cmc-keep → not-cmc-force
    // (firewire/core-card.c:432-531). The order is load-bearing for anti-ping-pong.
    if ((in.rootCap == RootCapability::Unknown) && !in.cycles.cycleStartObserved) {
        action.kind = RoleAction::Kind::DeferForEvidence;
        action.reason = "awaiting root-capability / cycle-start evidence";
        return action;
    }

    // Evidence exists, but no policy decisions are implemented yet (FW-9).
    // Deliberately inert: the skeleton must never issue a reset or cycle-master
    // change. Local cycleMaster is NOT a role-policy output (see FW-7 note).
    action.kind = RoleAction::Kind::None;
    action.reason = "skeleton: no policy decisions implemented yet (FW-9)";
    return action;
}

} // namespace ASFW::Driver::Role
