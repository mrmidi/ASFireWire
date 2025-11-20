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

    std::optional<PhyConfigCommand> pendingCommand;
    
    // State variables for decision logic
    bool localContender = false;
    bool otherContender = false;
    uint8_t otherContenderID = 0;
    bool badIRM = false;
    
    // Scan topology for contenders (similar to IOFireWireController)
    for (const auto& node : topology.nodes) {
        if (node.isIRMCandidate && node.linkActive) {
            if (node.nodeId == localNodeID) {
                localContender = true;
            } else {
                // Check if this node is a valid contender (not bad IRM)
                bool isBad = (node.nodeId < badIRMFlags.size() && badIRMFlags[node.nodeId]);
                if (!isBad) {
                    otherContender = true;
                    otherContenderID = node.nodeId; // Use highest found (loop order)
                }
            }
        }
    }

    // ========================================================================
    // Scenario A: Explicit Forced Root (ForceNode Policy)
    // ========================================================================
    if (config_.rootPolicy == RootPolicy::ForceNode && config_.forcedRootNodeID != 0xFF) {
        // Only act if we're currently root but forced node is different
        if (rootNodeID == localNodeID && config_.forcedRootNodeID != localNodeID) {
            ASFW_LOG(BusManager, "Forcing root to node %u (policy=ForceNode)", config_.forcedRootNodeID);
            
            // We must NOT be a contender if we are forcing someone else
            PhyConfigCommand cmd{};
            cmd.setContender = false; 
            cmd.forceRootNodeID = config_.forcedRootNodeID;
            return cmd;
        }
        // If we are not root, we might still need to ensure we are not a contender?
        // Apple driver logic is primarily about "if we are root, give it up".
    }

    // ========================================================================
    // Scenario B: Delegate or Bad IRM Recovery (Auto/Delegate Policy)
    // ========================================================================
    if (config_.delegateCycleMaster || !badIRMFlags.empty() || config_.rootPolicy == RootPolicy::Delegate) {
        
        if (otherContender) {
            // Case 1: We are root, but we want to delegate
            if (rootNodeID == localNodeID && config_.delegateCycleMaster) {
                ASFW_LOG(BusManager, "🔄 Attempting to delegate root to node %u (delegate mode)", otherContenderID);
                
                PhyConfigCommand cmd{};
                cmd.setContender = false; // Clear our contender bit
                cmd.forceRootNodeID = otherContenderID; // Force the other node
                return cmd;
            }
        }
        else if (rootNodeID != localNodeID && localContender && !config_.delegateCycleMaster) {
            // Case 2: We are NOT root, we are a contender, and we do NOT want to delegate
            // (e.g. ForceLocal policy or Auto with no other options)
            ASFW_LOG(BusManager, "Forcing local controller as root (policy=ForceLocal/Auto)");
            
            PhyConfigCommand cmd{};
            cmd.setContender = true; // Ensure we are a contender
            cmd.forceRootNodeID = localNodeID;
            return cmd;
        }
        
        // Check for Bad IRM
        if (!badIRMFlags.empty()) {
            // Check if current IRM is bad
            if (irmNodeID != 0xFF && irmNodeID < badIRMFlags.size() && badIRMFlags[irmNodeID]) {
                badIRM = true;
            }
            // Also check if IRM is missing (no IRM node ID)
            if (irmNodeID == 0xFF) {
                badIRM = true;
            }
            
            if (badIRM) {
                ASFW_LOG(BusManager, "⚠️  Bad IRM detected (node %u) - Recovery DISABLED for debugging", irmNodeID);
            }

            /* 
               DISABLED FOR DEBUGGING: This logic causes infinite reset loops if the lock test fails repeatedly.
               We want to investigate the lock failure without resetting the bus.
            
            if (badIRM || (!localContender && !otherContender)) {
                uint8_t newRoot = 0;
                bool setLocalContender = false;
                
                if (!config_.delegateCycleMaster) {
                    // Mac wants to be root
                    newRoot = localNodeID;
                    setLocalContender = true;
                } else if (otherContender) {
                    newRoot = otherContenderID;
                } else {
                    // Nobody else can do it, Mac must do it
                    newRoot = localNodeID;
                    setLocalContender = true;
                }
                
                ASFW_LOG(BusManager, "Replacing bad/missing IRM (current=%u): forcing root to %u", 
                         irmNodeID, newRoot);
                
                PhyConfigCommand cmd{};
                if (setLocalContender) {
                    cmd.setContender = true;
                }
                cmd.forceRootNodeID = newRoot;
                return cmd;
            }
            */
        } else {
             ASFW_LOG(BusManager, "BadIRMFlags is empty");
        }
    } else {
        ASFW_LOG(BusManager, "Skipping Scenario B (delegate=%d badIRMFlags=%zu policy=%d)", 
                 config_.delegateCycleMaster, badIRMFlags.size(), static_cast<int>(config_.rootPolicy));
    }

    ASFW_LOG(BusManager, "✅ AssignCycleMaster: No action needed (root=%u IRM=%u local=%u)",
             rootNodeID, irmNodeID, localNodeID);
    return std::nullopt;
}

// ============================================================================
// Gap Count Optimization Implementation
// ============================================================================

std::optional<BusManager::PhyConfigCommand> BusManager::OptimizeGapCount(
    const TopologySnapshot& topology,
    const std::vector<uint32_t>& selfIDs)
{
    if (!config_.enableGapOptimization) {
        ASFW_LOG_DEBUG(BusManager, "Gap optimization disabled");
        return std::nullopt;
    }

    if (!topology.localNodeId.has_value() || !topology.rootNodeId.has_value()) {
        ASFW_LOG(BusManager, "OptimizeGapCount: Invalid topology");
        return std::nullopt;
    }

    const uint8_t localNodeID = *topology.localNodeId;
    const uint8_t rootNodeID = *topology.rootNodeId;

    // Only optimize if we're the IRM (highest contender)
    if (topology.irmNodeId.has_value() && *topology.irmNodeId != localNodeID) {
        ASFW_LOG(BusManager, "Not IRM, skipping gap optimization (IRM=%u local=%u)",
                 *topology.irmNodeId, localNodeID);
        return std::nullopt;
    }

    uint8_t newGap = 0;

    // ========================================================================
    // Calculate Optimal Gap Count
    // ========================================================================
    if (config_.forcedGapFlag) {
        newGap = config_.forcedGapCount;
        ASFW_LOG(BusManager, "Using forced gap count: %u", newGap);
    } else {
        // Method 1: Hop count based (conservative)
        uint8_t maxHops = rootNodeID;  // Assumes daisy chain (worst case)
        if (maxHops > 25) maxHops = 25;
        uint8_t hopGap = CalculateGapFromHops(maxHops);

        // Method 2: Ping time based (accurate, if available)
        uint8_t pingGap = hopGap;  // Fallback to hop-based if ping unavailable

        // TODO: Add ping time support when HardwareInterface::GetPingTimes() is implemented
        // For now, we'll use hop-based calculation only

        // Take the larger (safer) value
        newGap = std::max(hopGap, pingGap);

        ASFW_LOG(BusManager, "Calculated gap: hops=%u→gap=%u ping=%u→gap=%u final=%u",
                 maxHops, hopGap, 0, pingGap, newGap);
    }

    // ========================================================================
    // Check if Gap Count Needs Updating
    // ========================================================================
    bool retoolGap = false;

    // Check consistency across all Self-IDs
    if (!AreGapsConsistent(selfIDs)) {
        ASFW_LOG(BusManager, "Gap counts inconsistent across Self-IDs");
        retoolGap = true;
    }

    // Check if current gap matches our desired gap or previous gap
    if (!retoolGap && !selfIDs.empty()) {
        uint8_t currentGap = ExtractGapCount(selfIDs[0]);
        if (currentGap != newGap && currentGap != previousGap_) {
            ASFW_LOG(BusManager, "Gap mismatch: current=%u new=%u prev=%u",
                     currentGap, newGap, previousGap_);
            retoolGap = true;
        }
    }

    // ========================================================================
    // Apply New Gap Count
    // ========================================================================
    if (retoolGap) {
        ASFW_LOG(BusManager, "🔧 Applying gap count: %u (previous=%u)", newGap, previousGap_);
        previousGap_ = newGap;

        // Send PHY packet with ONLY gap count update (no force root)
        // Gap count can be updated independently of root selection
        PhyConfigCommand cmd{};
        cmd.gapCount = newGap;
        return cmd;
    }

    ASFW_LOG(BusManager, "✅ Gap optimization: No action needed (gap=%u)", newGap);
    return std::nullopt;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

std::optional<uint8_t> BusManager::FindOtherContender(
    const TopologySnapshot& topology,
    uint8_t excludeNodeID) const
{
    // Scan nodes for contenders (IRM candidates) with active links
    for (const auto& node : topology.nodes) {
        if (node.nodeId != excludeNodeID &&
            node.isIRMCandidate &&
            node.linkActive)
        {
            ASFW_LOG(BusManager, "Found other contender: node %u", node.nodeId);
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
    // Preference order:
    // 1. Local controller (most reliable)
    if (topology.localNodeId.has_value()) {
        return *topology.localNodeId;
    }

    // 2. Highest contender that's not marked bad
    for (auto it = topology.nodes.rbegin(); it != topology.nodes.rend(); ++it) {
        if (it->isIRMCandidate &&
            it->linkActive &&
            it->nodeId != badIRMNodeID &&
            (it->nodeId >= badIRMFlags.size() || !badIRMFlags[it->nodeId]))
        {
            return it->nodeId;
        }
    }

    // 3. Any node with active link (last resort)
    for (auto it = topology.nodes.rbegin(); it != topology.nodes.rend(); ++it) {
        if (it->linkActive && it->nodeId != badIRMNodeID) {
            return it->nodeId;
        }
    }

    // Fallback to root (shouldn't happen)
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
    if (maxHops >= 26) maxHops = 25;  // Table only goes to 25
    return GAP_TABLE[maxHops];
}

uint8_t BusManager::CalculateGapFromPing(uint32_t maxPingNs) const {
    if (maxPingNs > 245) maxPingNs = 245;  // Cap at table limit

    if (maxPingNs >= 29) {
        uint32_t index = (maxPingNs - 20) / 9;
        if (index >= 26) index = 25;
        return GAP_TABLE[index];
    } else {
        return 5;  // Minimum gap for short distances
    }
}

uint8_t BusManager::ExtractGapCount(uint32_t selfIDQuad) {
    // Self-ID packet 0 has gap count in bits[21:16]
    // Check for Self-ID tag (bits[31:30] = 10) and packet 0 (bits[23:22] = 00)
    if ((selfIDQuad & SelfID::kSelfIDTag) == SelfID::kSelfIDTag &&
        (selfIDQuad & SelfID::kPacket0Mask) == SelfID::kPacket0Type) {
        // Mask first, then shift (correct bit extraction order)
        return (selfIDQuad & SelfID::kGapCountMask) >> SelfID::kGapCountShift;
    }
    return 0x3F;  // Invalid, return default
}

bool BusManager::AreGapsConsistent(const std::vector<uint32_t>& selfIDs) {
    if (selfIDs.empty()) return true;

    // Extract gap from first packet 0
    std::optional<uint8_t> referenceGap;
    for (const uint32_t quad : selfIDs) {
        // Check for Self-ID tag and packet 0
        if ((quad & SelfID::kSelfIDTag) == SelfID::kSelfIDTag &&
            (quad & SelfID::kPacket0Mask) == SelfID::kPacket0Type) {
            if (!referenceGap.has_value()) {
                referenceGap = ExtractGapCount(quad);
            } else {
                uint8_t gap = ExtractGapCount(quad);
                if (gap != *referenceGap) {
                    return false;  // Mismatch detected
                }
            }
        }
    }

    return true;  // All gaps match (or only one packet 0)
}

} // namespace ASFW::Driver
