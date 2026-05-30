#include "RolePolicy.hpp"

namespace ASFW::Driver::Role {

// Apple-compatible default policy (FW-17 is the primary reference).
//
//   * Root CMC is NOT a policy trigger. Apple's IOFireWireFamily never reads the
//     Config ROM CMC bit; it drives root/cycle-master from Self-ID contender/link
//     and empirical IRM probing. So CMC=1 is "accept", CMC=0 is diagnostics-only.
//   * Remote STATE_SET.cmstr is done by NEITHER Apple nor Linux. It lives behind
//     the top ladder rung (RemoteCmstrAllowed) as an explicit experiment.
//   * Force-root on verified CMC=0 is Linux-shaped, not Apple — opt-in only via
//     linuxStyleCmcForceRoot AND ForceRootAllowed AND local==IRM.
//   * Every mutating RoleAction is gated by FullBMActivityLevel; at ObserveOnly
//     (the default) we emit a verdict but request no hardware/bus side effect.
//
// See [[apple-ignores-cmc-irm-probing]] / Linear FW-16 (Linux) + FW-17 (Apple).

RoleAction EvaluateRolePolicy(const RoleInputs& in) noexcept {
    using Level = ASFW::FW::FullBMActivityLevel;
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
    const bool localIsIRM = localNode == in.irmNodeId;

    if (in.rootCap == RootCapability::Unknown) {
        action.kind = RoleAction::Kind::DeferForEvidence;
        action.reason = "awaiting root-capability / cycle-start evidence";
        return action;
    }

    // ---- Local node is root (Apple: enable our own cycle master) --------------
    if (localIsRoot) {
        if (in.localCmcCapable || in.rootCap == RootCapability::CapableByBIB) {
            // Apple couples cycle-master enable with active root/gap policy
            // (finishedBusScan). Gate it at GapPolicyAllowed; below that, report.
            if (in.activity >= Level::GapPolicyAllowed) {
                action.kind = RoleAction::Kind::EnableLocalCycleMaster;
                action.targetRoot = localNode;
                action.reason = "local root accepted: enable local cycleMaster";
            } else {
                action.kind = RoleAction::Kind::None;
                action.targetRoot = localNode;
                action.reason = "local root accepted; would enable cycleMaster (gated below GapPolicyAllowed)";
            }
            return action;
        }

        action.kind = RoleAction::Kind::MarkRootBadOrUnknown;
        action.reason = "local root is not known cycle-master-capable";
        return action;
    }

    // ---- Remote node is root --------------------------------------------------
    switch (in.rootCap) {
    case RootCapability::CapableByBIB:
        // Apple ignores CMC and would self-promote if it were IRM; Linux keeps a
        // CMC root. Both treat a working CMC root as fine — so accept it. Remote
        // CMSTR is experimental-only and never the default for a CMC root.
        if (in.activity >= Level::RemoteCmstrAllowed) {
            action.kind = RoleAction::Kind::EnableRemoteCycleMaster;
            action.targetRoot = rootNode;
            action.reason = "remote root CMC=1: experimental remote STATE_SET.cmstr (RemoteCmstrAllowed)";
        } else {
            action.kind = RoleAction::Kind::None;
            action.targetRoot = rootNode;
            action.reason = "remote root CMC=1: accept (Apple ignores CMC; no remote CMSTR)";
        }
        return action;

    case RootCapability::FunctioningByCycleStart:
        // Cycle-start observation is diagnostic evidence, never a policy trigger
        // (neither reference stack uses it). Accept and take no action.
        action.kind = RoleAction::Kind::None;
        action.targetRoot = rootNode;
        action.reason = "remote root accepted by cycle continuity (diagnostic only, no action)";
        return action;

    case RootCapability::IncapableByBIB:
        // Apple-compatible default: CMC is diagnostics-only — do NOT act on it.
        // Force-root here is Linux-shaped and strictly opt-in.
        if (in.linuxStyleCmcForceRoot && in.activity >= Level::ForceRootAllowed &&
            localIsIRM && in.localCmcCapable) {
            action.kind = RoleAction::Kind::ForceRootAndReset;
            action.targetRoot = localNode;
            action.reset = RoleResetFlavor::Short;
            action.reason = "remote root CMC=0: Linux-style self-promote (experimental, force-root unlocked)";
            return action;
        }
        action.kind = RoleAction::Kind::MarkRootBadOrUnknown;
        action.reason = "remote root BIB CMC=0; Apple-compatible policy does not act on CMC";
        return action;

    case RootCapability::BadOrNonResponsive:
        // Empirical non-responsive root (analogous to Apple fBadIRMsKnown). Apple
        // self-promotes when it is the IRM and capable. Gated by ForceRootAllowed.
        if (in.activity >= Level::ForceRootAllowed && localIsIRM && in.localCmcCapable) {
            action.kind = RoleAction::Kind::ForceRootAndReset;
            action.targetRoot = localNode;
            action.reset = RoleResetFlavor::Short;
            action.reason = "remote root bad/nonresponsive: self-promote local root (force-root unlocked)";
            return action;
        }
        action.kind = RoleAction::Kind::MarkRootBadOrUnknown;
        action.reason = "remote root bad/nonresponsive; force-root gated or local not capable IRM";
        return action;

    case RootCapability::Unknown:
        break;
    }

    action.kind = RoleAction::Kind::DeferForEvidence;
    action.reason = "awaiting root-capability / cycle-start evidence";
    return action;
}

} // namespace ASFW::Driver::Role
