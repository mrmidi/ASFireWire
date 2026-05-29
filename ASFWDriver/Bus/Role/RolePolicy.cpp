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

    const uint8_t localNode = *in.topo->localNodeId;
    const uint8_t rootNode = *in.topo->rootNodeId;
    const bool localIsRoot = localNode == rootNode;

    if (in.rootCap == RootCapability::Unknown) {
        action.kind = RoleAction::Kind::DeferForEvidence;
        action.reason = "awaiting root-capability / cycle-start evidence";
        return action;
    }

    if (localIsRoot) {
        if (in.localCmcCapable || in.rootCap == RootCapability::CapableByBIB) {
            action.kind = RoleAction::Kind::EnableLocalCycleMaster;
            action.targetRoot = localNode;
            action.reason = "local root accepted: enable local cycleMaster";
            return action;
        }

        action.kind = RoleAction::Kind::MarkRootBadOrUnknown;
        action.reason = "local root is not known cycle-master-capable";
        return action;
    }

    switch (in.rootCap) {
    case RootCapability::CapableByBIB:
        action.kind = RoleAction::Kind::EnableRemoteCycleMaster;
        action.targetRoot = rootNode;
        action.reason = "remote root CMC=1: write remote STATE_SET.cmstr";
        return action;

    case RootCapability::FunctioningByCycleStart:
        action.kind = RoleAction::Kind::None;
        action.targetRoot = rootNode;
        action.reason = "remote root accepted by cycle continuity";
        return action;

    case RootCapability::IncapableByBIB:
        if (in.localCmcCapable) {
            action.kind = RoleAction::Kind::ForceRootAndReset;
            action.targetRoot = localNode;
            action.reset = RoleResetFlavor::Short;
            action.reason = "remote root CMC=0: force local capable root";
            return action;
        }
        action.kind = RoleAction::Kind::MarkRootBadOrUnknown;
        action.reason = "remote root CMC=0 and no capable local fallback";
        return action;

    case RootCapability::BadOrNonResponsive:
        if (in.localCmcCapable) {
            action.kind = RoleAction::Kind::ForceRootAndReset;
            action.targetRoot = localNode;
            action.reset = RoleResetFlavor::Short;
            action.reason = "remote root bad/nonresponsive: force local capable root";
            return action;
        }
        action.kind = RoleAction::Kind::MarkRootBadOrUnknown;
        action.reason = "remote root bad/nonresponsive and no capable local fallback";
        return action;

    case RootCapability::Unknown:
        break;
    }

    action.kind = RoleAction::Kind::DeferForEvidence;
    action.reason = "awaiting root-capability / cycle-start evidence";
    return action;
}

} // namespace ASFW::Driver::Role
