#include "BusManager.hpp"
#include "Logging.hpp"
#include <algorithm>

namespace ASFW::Driver {

// IEEE 1394-1995 §8.4.2.4 - Self-ID Packet 0 Format
namespace SelfID {
    constexpr uint32_t kGapCountShift = 16;
    constexpr uint32_t kGapCountMask = 0x003F0000;  // Bits[21:16] before shift
    constexpr uint32_t kSelfIDTag = 0x80000000;     // Bits[31:30] = 10 (Self-ID identifier)
    constexpr uint32_t kPacket0Mask = 0x00C00000;   // Bits[23:22] = packet number
    constexpr uint32_t kPacket0Type = 0x00000000;   // Packet 0 has bits[23:22] = 00
}

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

void BusManager::SetForcedGapCount(uint8_t gapCount) {
    config_.forcedGapCount = gapCount;
    config_.forcedGapFlag = (gapCount > 0);
    ASFW_LOG(BusManager, "Forced gap count set to %u (flag=%d)", gapCount, config_.forcedGapFlag);
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
    const uint8_t irmNodeID = topology.irmNodeId.value_or(0xFF);

    bool localContender = false;
    bool otherContender = false;
    uint8_t otherContenderID = 0;
    bool badIRM = false;
    
    for (const auto& node : topology.nodes) {
        if (node.isIRMCandidate && node.linkActive) {
            if (node.nodeId == localNodeID) {
                localContender = true;
            } else {
                bool isBad = (node.nodeId < badIRMFlags.size() && badIRMFlags[node.nodeId]);
                if (!isBad) {
                    otherContender = true;
                    otherContenderID = node.nodeId;
                }
            }
        }
    }

    if (config_.rootPolicy == RootPolicy::ForceNode && config_.forcedRootNodeID != 0xFF) {
        if (rootNodeID == localNodeID && config_.forcedRootNodeID != localNodeID) {
            ASFW_LOG(BusManager, "Forcing root to node %u", config_.forcedRootNodeID);
            
            PhyConfigCommand cmd{};
            cmd.setContender = false; 
            cmd.forceRootNodeID = config_.forcedRootNodeID;
            return cmd;
        }
    }

    if (config_.delegateCycleMaster || !badIRMFlags.empty() || config_.rootPolicy == RootPolicy::Delegate) {
        
        if (otherContender) {
            if (rootNodeID == localNodeID && config_.delegateCycleMaster) {
                ASFW_LOG(BusManager, "🔄 Attempting to delegate root to node %u", otherContenderID);
                
                PhyConfigCommand cmd{};
                cmd.setContender = false;
                cmd.forceRootNodeID = otherContenderID;
                return cmd;
            }
        }
        else if (rootNodeID != localNodeID && localContender && !config_.delegateCycleMaster) {
            ASFW_LOG(BusManager, "Forcing local controller as root");
            
            PhyConfigCommand cmd{};
            cmd.setContender = true;
            cmd.forceRootNodeID = localNodeID;
            return cmd;
        }
        
        if (!badIRMFlags.empty()) {
            if (irmNodeID != 0xFF && irmNodeID < badIRMFlags.size() && badIRMFlags[irmNodeID]) {
                badIRM = true;
            }
            if (irmNodeID == 0xFF) {
                badIRM = true;
            }
            
            if (badIRM) {
                ASFW_LOG(BusManager, "⚠️  Bad IRM detected (node %u)", irmNodeID);
            }
        }
    }

    ASFW_LOG(BusManager, "✅ AssignCycleMaster: No action needed (root=%u IRM=%u local=%u)",
             rootNodeID, irmNodeID, localNodeID);
    return std::nullopt;
}

std::optional<BusManager::PhyConfigCommand> BusManager::OptimizeGapCount(
    const TopologySnapshot& topology,
    const std::vector<uint32_t>& selfIDs)
{
    if (!config_.enableGapOptimization) {
        return std::nullopt;
    }

    if (!topology.localNodeId.has_value() || !topology.rootNodeId.has_value()) {
        return std::nullopt;
    }

    const uint8_t localNodeID = *topology.localNodeId;
    const uint8_t rootNodeID = *topology.rootNodeId;

    if (topology.irmNodeId.has_value() && *topology.irmNodeId != localNodeID) {
        return std::nullopt;
    }

    uint8_t newGap = 0;

    if (config_.forcedGapFlag) {
        newGap = config_.forcedGapCount;
        ASFW_LOG(BusManager, "Using forced gap count: %u", newGap);
    } else {
        uint8_t maxHops = rootNodeID;
        if (maxHops > 25) maxHops = 25;
        uint8_t hopGap = CalculateGapFromHops(maxHops);

        uint8_t pingGap = hopGap;

        newGap = std::max(hopGap, pingGap);

        ASFW_LOG(BusManager, "Calculated gap: hops=%u→gap=%u final=%u",
                 maxHops, hopGap, newGap);
    }

    bool retoolGap = false;

    if (!AreGapsConsistent(selfIDs)) {
        ASFW_LOG(BusManager, "Gap counts inconsistent across Self-IDs");
        retoolGap = true;
    }

    if (!retoolGap && !selfIDs.empty()) {
        uint8_t currentGap = ExtractGapCount(selfIDs[0]);
        if (currentGap != newGap && currentGap != previousGap_) {
            ASFW_LOG(BusManager, "Gap mismatch: current=%u new=%u prev=%u",
                     currentGap, newGap, previousGap_);
            retoolGap = true;
        }
    }

    if (retoolGap) {
        ASFW_LOG(BusManager, "🔧 Applying gap count: %u (previous=%u)", newGap, previousGap_);
        previousGap_ = newGap;

        PhyConfigCommand cmd{};
        cmd.gapCount = newGap;
        return cmd;
    }

    ASFW_LOG(BusManager, "✅ Gap optimization: No action needed (gap=%u)", newGap);
    return std::nullopt;
}

std::optional<uint8_t> BusManager::FindOtherContender(
    const TopologySnapshot& topology,
    uint8_t excludeNodeID) const
{
    for (const auto& node : topology.nodes) {
        if (node.nodeId != excludeNodeID &&
            node.isIRMCandidate &&
            node.linkActive)
        {
            return node.nodeId;
        }
    }
    return std::nullopt;
}

uint8_t BusManager::SelectGoodRoot(
    const TopologySnapshot& topology,
    const std::vector<bool>& badIRMFlags,
    uint8_t badIRMNodeID) const
{
    if (topology.localNodeId.has_value()) {
        return *topology.localNodeId;
    }

    for (auto it = topology.nodes.rbegin(); it != topology.nodes.rend(); ++it) {
        if (it->isIRMCandidate &&
            it->linkActive &&
            it->nodeId != badIRMNodeID &&
            (it->nodeId >= badIRMFlags.size() || !badIRMFlags[it->nodeId]))
        {
            return it->nodeId;
        }
    }

    for (auto it = topology.nodes.rbegin(); it != topology.nodes.rend(); ++it) {
        if (it->linkActive && it->nodeId != badIRMNodeID) {
            return it->nodeId;
        }
    }

    return topology.rootNodeId.value_or(0);
}

BusManager::PhyConfigCommand BusManager::BuildPhyConfigCommand(
    std::optional<uint8_t> forceRootNodeID,
    std::optional<uint8_t> gapCount) const {
    PhyConfigCommand cmd{};
    cmd.forceRootNodeID = forceRootNodeID;
    cmd.gapCount = gapCount;
    return cmd;
}

uint8_t BusManager::CalculateGapFromHops(uint8_t maxHops) const {
    if (maxHops >= 26) maxHops = 25;
    return GAP_TABLE[maxHops];
}

uint8_t BusManager::CalculateGapFromPing(uint32_t maxPingNs) const {
    if (maxPingNs > 245) maxPingNs = 245;

    if (maxPingNs >= 29) {
        uint32_t index = (maxPingNs - 20) / 9;
        if (index >= 26) index = 25;
        return GAP_TABLE[index];
    } else {
        return 5;
    }
}

uint8_t BusManager::ExtractGapCount(uint32_t selfIDQuad) {
    if ((selfIDQuad & SelfID::kSelfIDTag) == SelfID::kSelfIDTag &&
        (selfIDQuad & SelfID::kPacket0Mask) == SelfID::kPacket0Type) {
        return (selfIDQuad & SelfID::kGapCountMask) >> SelfID::kGapCountShift;
    }
    return 0x3F;
}

bool BusManager::AreGapsConsistent(const std::vector<uint32_t>& selfIDs) {
    if (selfIDs.empty()) return true;

    std::optional<uint8_t> referenceGap;
    for (const uint32_t quad : selfIDs) {
        if ((quad & SelfID::kSelfIDTag) == SelfID::kSelfIDTag &&
            (quad & SelfID::kPacket0Mask) == SelfID::kPacket0Type) {
            if (!referenceGap.has_value()) {
                referenceGap = ExtractGapCount(quad);
            } else {
                uint8_t gap = ExtractGapCount(quad);
                if (gap != *referenceGap) {
                    return false;
                }
            }
        }
    }

    return true;
}

} // namespace ASFW::Driver
