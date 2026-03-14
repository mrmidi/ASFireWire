#include "BusManager.hpp"
#include "Logging.hpp"

#include <algorithm>
#include <span>

namespace ASFW::Driver {

namespace {

struct CycleMasterInputs {
    uint8_t localNodeID{0};
    uint8_t rootNodeID{0};
    uint8_t irmNodeID{0xFF};
    bool localContender{false};
    std::optional<uint8_t> otherContenderID;
    bool badIRM{false};
};

[[nodiscard]] std::vector<uint8_t> ExtractObservedBaseGaps(const std::vector<uint32_t>& selfIDs) {
    std::vector<uint8_t> gaps;
    gaps.reserve(selfIDs.size());

    for (const uint32_t packet : selfIDs) {
        if (!IsSelfIDTag(packet) || IsExtended(packet)) {
            continue;
        }
        gaps.push_back(ExtractGapCount(packet));
    }

    return gaps;
}

[[nodiscard]] bool AreObservedGapsConsistent(std::span<const uint8_t> gaps) {
    if (gaps.empty()) {
        return true;
    }

    const uint8_t referenceGap = gaps.front();
    return std::all_of(gaps.begin(), gaps.end(),
                       [referenceGap](const uint8_t gap) { return gap == referenceGap; });
}

[[nodiscard]] bool AnyObservedGapIsZero(std::span<const uint8_t> gaps) {
    return std::any_of(gaps.begin(), gaps.end(), [](const uint8_t gap) { return gap == 0U; });
}

[[nodiscard]] bool AnyObservedGapNeedsRetool(std::span<const uint8_t> gaps, const uint8_t previousGap,
                                             const uint8_t targetGap) {
    return std::any_of(gaps.begin(), gaps.end(), [previousGap, targetGap](const uint8_t gap) {
        return gap != previousGap && gap != targetGap;
    });
}

[[nodiscard]] BusManager::PhyConfigCommand MakePhyConfigCommand(const std::optional<uint8_t> forceRootNodeID,
                                                               const std::optional<bool> setContender) {
    BusManager::PhyConfigCommand cmd{};
    cmd.forceRootNodeID = forceRootNodeID;
    cmd.setContender = setContender;
    return cmd;
}

[[nodiscard]] CycleMasterInputs CollectCycleMasterInputs(const TopologySnapshot& topology,
                                                        const std::vector<bool>& badIRMFlags,
                                                        const uint8_t localNodeID,
                                                        const uint8_t rootNodeID) {
    CycleMasterInputs inputs{};
    inputs.localNodeID = localNodeID;
    inputs.rootNodeID = rootNodeID;
    inputs.irmNodeID = topology.irmNodeId.value_or(0xFF);

    for (const auto& node : topology.nodes) {
        if (!node.isIRMCandidate || !node.linkActive) {
            continue;
        }

        if (node.nodeId == localNodeID) {
            inputs.localContender = true;
            continue;
        }

        const bool isBad = (node.nodeId < badIRMFlags.size() && badIRMFlags[node.nodeId]);
        if (!isBad) {
            inputs.otherContenderID = node.nodeId;
        }
    }

    if (badIRMFlags.empty()) {
        return inputs;
    }

    if (inputs.irmNodeID == 0xFF) {
        inputs.badIRM = true;
        return inputs;
    }

    if (inputs.irmNodeID < badIRMFlags.size() && badIRMFlags[inputs.irmNodeID]) {
        inputs.badIRM = true;
    }

    return inputs;
}

[[nodiscard]] bool ShouldEvaluateCycleMasterPolicy(const BusManager::Config& config,
                                                   const std::vector<bool>& badIRMFlags) {
    return config.delegateCycleMaster || !badIRMFlags.empty() ||
           config.rootPolicy == BusManager::RootPolicy::Delegate;
}

[[nodiscard]] std::optional<BusManager::PhyConfigCommand> MaybeForceConfiguredRoot(
    const BusManager::Config& config,
    const CycleMasterInputs& inputs) {
    if (config.rootPolicy != BusManager::RootPolicy::ForceNode || config.forcedRootNodeID == 0xFF) {
        return std::nullopt;
    }

    if (inputs.rootNodeID != inputs.localNodeID || config.forcedRootNodeID == inputs.localNodeID) {
        return std::nullopt;
    }

    ASFW_LOG(BusManager, "Forcing root to node %u", config.forcedRootNodeID);
    return MakePhyConfigCommand(config.forcedRootNodeID, false);
}

[[nodiscard]] std::optional<BusManager::PhyConfigCommand> MaybeDelegateOrClaimRoot(
    const BusManager::Config& config,
    const CycleMasterInputs& inputs) {
    if (inputs.otherContenderID.has_value()) {
        if (inputs.rootNodeID != inputs.localNodeID || !config.delegateCycleMaster) {
            return std::nullopt;
        }

        ASFW_LOG(BusManager, "🔄 Attempting to delegate root to node %u", *inputs.otherContenderID);
        return MakePhyConfigCommand(*inputs.otherContenderID, false);
    }

    if (inputs.rootNodeID == inputs.localNodeID || !inputs.localContender || config.delegateCycleMaster) {
        return std::nullopt;
    }

    ASFW_LOG(BusManager, "Forcing local controller as root");
    return MakePhyConfigCommand(inputs.localNodeID, true);
}

[[nodiscard]] std::optional<BusManager::PhyConfigCommand> MaybeRecoverBadIRM(
    const BusManager::Config& config,
    const CycleMasterInputs& inputs) {
    if (!inputs.badIRM && inputs.irmNodeID != 0xFF) {
        return std::nullopt;
    }

    if (!config.delegateCycleMaster) {
        ASFW_LOG(BusManager, "Forcing local node as IRM (bad IRM or no contenders)");
        return MakePhyConfigCommand(inputs.localNodeID, true);
    }

    if (inputs.otherContenderID.has_value()) {
        ASFW_LOG(BusManager, "Delegating IRM to node %u (bad IRM=%u)", *inputs.otherContenderID,
                 inputs.irmNodeID);
        return MakePhyConfigCommand(*inputs.otherContenderID, false);
    }

    ASFW_LOG(BusManager, "No IRM candidates — forcing local node as contender (Apple fallback)");
    return MakePhyConfigCommand(inputs.localNodeID, true);
}

} // namespace

// ============================================================================
// Configuration Methods
// ============================================================================

void BusManager::SetRootPolicy(RootPolicy policy) {
    config_.rootPolicy = policy;
    ASFW_LOG(BusManager, "Root policy set to %u", static_cast<uint8_t>(policy));
}

void BusManager::SetForcedRootNode(uint8_t nodeID) {
    config_.forcedRootNodeID = nodeID;
    ASFW_LOG(BusManager, "Forced root node set to %u", nodeID);
}

void BusManager::SetDelegateMode(bool enable) {
    config_.delegateCycleMaster = enable;
    ASFW_LOG(BusManager, "Delegate mode %{public}s", enable ? "enabled" : "disabled");
}

void BusManager::SetGapOptimizationEnabled(bool enable) {
    config_.enableGapOptimization = enable;
    ASFW_LOG(BusManager, "Gap optimization %{public}s", enable ? "enabled" : "disabled");
}

void BusManager::SetForcedGapCount(uint8_t gapCount) {
    config_.forcedGapCount = gapCount;
    config_.forcedGapFlag = (gapCount > 0);
    ASFW_LOG(BusManager, "Forced gap count set to %u (flag=%d)", gapCount, config_.forcedGapFlag);
}

const char* BusManager::GapDecisionReasonString(const GapDecisionReason reason) noexcept {
    switch (reason) {
    case GapDecisionReason::MismatchForce63:
        return "MismatchForce63";
    case GapDecisionReason::ForcedGap:
        return "ForcedGap";
    case GapDecisionReason::TargetGap:
        return "TargetGap";
    case GapDecisionReason::ZeroObservedGap:
        return "ZeroObservedGap";
    }

    return "Unknown";
}

// ============================================================================
// AssignCycleMaster Implementation
// ============================================================================

std::optional<BusManager::PhyConfigCommand> BusManager::AssignCycleMaster(
    const TopologySnapshot& topology,
    const std::vector<bool>& badIRMFlags)
{
    if (!topology.localNodeId.has_value() || !topology.rootNodeId.has_value()) {
        ASFW_LOG(BusManager, "AssignCycleMaster: Invalid topology (local=%d root=%d)",
                 topology.localNodeId.has_value(), topology.rootNodeId.has_value());
        return std::nullopt;
    }

    const uint8_t localNodeID = *topology.localNodeId;
    const uint8_t rootNodeID = *topology.rootNodeId;
    const CycleMasterInputs inputs = CollectCycleMasterInputs(topology, badIRMFlags, localNodeID, rootNodeID);

    if (const auto forcedRoot = MaybeForceConfiguredRoot(config_, inputs)) {
        return forcedRoot;
    }

    if (!ShouldEvaluateCycleMasterPolicy(config_, badIRMFlags)) {
        ASFW_LOG(BusManager, "✅ AssignCycleMaster: No action needed (root=%u IRM=%u local=%u)",
                 rootNodeID, inputs.irmNodeID, localNodeID);
        return std::nullopt;
    }

    if (const auto rootDecision = MaybeDelegateOrClaimRoot(config_, inputs)) {
        return rootDecision;
    }

    if (inputs.badIRM) {
        ASFW_LOG(BusManager, "⚠️  Bad IRM detected (node %u)", inputs.irmNodeID);
    }

    // Apple's AssignCycleMaster fallback (IOFireWireController.cpp):
    // If bad IRM or no IRM at all, we must ensure *somebody* becomes IRM.
    // DICE-class devices don't support IRM, so our node must take over
    // for isochronous resource management to work.
    if (const auto irmRecovery = MaybeRecoverBadIRM(config_, inputs)) {
        return irmRecovery;
    }

    ASFW_LOG(BusManager, "✅ AssignCycleMaster: No action needed (root=%u IRM=%u local=%u)",
             rootNodeID, inputs.irmNodeID, localNodeID);
    return std::nullopt;
}

std::optional<BusManager::GapDecision> BusManager::EvaluateGapPolicy(
    const TopologySnapshot& topology,
    const std::vector<uint32_t>& selfIDs)
{
    if (!config_.enableGapOptimization) {
        return std::nullopt;
    }

    if (!topology.localNodeId.has_value() || !topology.irmNodeId.has_value()) {
        return std::nullopt;
    }

    const uint8_t localNodeID = *topology.localNodeId;
    if (*topology.irmNodeId != localNodeID) {
        ASFW_LOG_V3(BusManager, "Skipping gap optimization because local node %u is not IRM %u",
                    localNodeID, *topology.irmNodeId);
        return std::nullopt;
    }

    const std::vector<uint8_t> observedGaps = ExtractObservedBaseGaps(selfIDs);
    if (observedGaps.empty()) {
        ASFW_LOG_V3(BusManager, "Skipping gap optimization because no validated packet-0 gaps exist");
        return std::nullopt;
    }

    // Apple IOFireWireFamily `processSelfIDs()` corrects validated packet-0
    // mismatches by forcing a conservative `gap_count = 63` before later
    // optimization is considered.
    if (!AreObservedGapsConsistent(observedGaps)) {
        ASFW_LOG(BusManager, "Gap mismatch across validated packet-0 Self-IDs; forcing gap 63");
        return GapDecision{0x3F, GapDecisionReason::MismatchForce63};
    }

    const uint8_t targetGap =
        config_.forcedGapFlag ? config_.forcedGapCount : CalculateGapFromHops(topology.maxHopsFromRoot);

    if (config_.forcedGapFlag) {
        if (targetGap == gapState_.lastConfirmedGap) {
            ASFW_LOG_V3(BusManager, "Forced gap %u already matches last confirmed gap", targetGap);
            return std::nullopt;
        }

        ASFW_LOG(BusManager, "Forcing gap count to %u (confirmed=%u)", targetGap,
                 gapState_.lastConfirmedGap);
        return GapDecision{targetGap, GapDecisionReason::ForcedGap};
    }

    // Apple IOFireWireFamily `finishedBusScan()` only retools after the bus is
    // stable if the observed packet-0 gaps are still unusable (zero) or do not
    // match either the previous programmed gap or the newly computed target.
    if (AnyObservedGapIsZero(observedGaps)) {
        ASFW_LOG(BusManager, "Observed zero gap_count; retooling to target gap %u", targetGap);
        return GapDecision{targetGap, GapDecisionReason::ZeroObservedGap};
    }

    if (AnyObservedGapNeedsRetool(observedGaps, gapState_.lastConfirmedGap, targetGap)) {
        ASFW_LOG(BusManager, "Retooling gap count to %u (confirmed=%u)", targetGap,
                 gapState_.lastConfirmedGap);
        return GapDecision{targetGap, GapDecisionReason::TargetGap};
    }

    ASFW_LOG_V3(BusManager,
                "Gap optimization stable: observed gaps already match confirmed %u or target %u",
                gapState_.lastConfirmedGap, targetGap);
    return std::nullopt;
}

void BusManager::NoteGapResetIssued(const uint8_t gapCount, const GapDecisionReason reason) {
    gapState_.inFlight = GapState::InFlightReset{gapCount, reason};
    ASFW_LOG_V3(BusManager, "Gap reset issued: target=%u reason=%{public}s", gapCount,
                GapDecisionReasonString(reason));
}

void BusManager::NoteStableGapObserved(const uint8_t observedGap) {
    const auto inFlight = gapState_.inFlight;
    gapState_.lastConfirmedGap = observedGap;
    gapState_.inFlight.reset();

    if (inFlight.has_value()) {
        ASFW_LOG_V3(BusManager,
                    "Stable packet-0 gap %u accepted after in-flight target %u (%{public}s)",
                    observedGap, inFlight->gapCount, GapDecisionReasonString(inFlight->reason));
        return;
    }

    ASFW_LOG_V3(BusManager, "Stable packet-0 gap %u accepted", observedGap);
}

void BusManager::ClearInFlightGapReset() {
    if (!gapState_.inFlight.has_value()) {
        return;
    }

    ASFW_LOG_V2(BusManager, "Discarding in-flight gap target %u after dispatch failure",
                gapState_.inFlight->gapCount);
    gapState_.inFlight.reset();
}

uint8_t BusManager::CalculateGapFromHops(uint8_t maxHops) const {
    if (maxHops >= 26) maxHops = 25;
    return GAP_TABLE[maxHops];
}

} // namespace ASFW::Driver
