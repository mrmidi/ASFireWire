#pragma once

#include <cstdint>
#include <optional>
#include <vector>
#include "TopologyTypes.hpp"
#include "../Controller/ControllerTypes.hpp"

namespace ASFW::Driver {

class BusResetCoordinator;

/**
 * BusManager - Manages FireWire bus topology optimization and resource allocation.
 *
 * This class implements three critical bus initialization features from Apple's
 * IOFireWireController:
 *
 * 1. AssignCycleMaster - Intelligent root/cycle master node selection
 * 2. Gap Count Optimization - Dynamic gap count tuning for maximum throughput
 * 3. IRM Capability Verification - CAS-based validation of IRM nodes
 *
 * Design Philosophy:
 * - Stateless operations (topology passed in, not cached)
 * - Policy-driven (configurable root selection behavior)
 * - Hardware abstraction (uses HardwareInterface for PHY packets)
 *
 * Usage:
 *   BusManager busManager;
 *   busManager.SetRootPolicy(BusManager::RootPolicy::Delegate);
 *
 *   // After bus reset and topology build:
 *   if (auto cmd = busManager.AssignCycleMaster(topology, badIRMFlags)) {
 *       StagePhyPacket(*cmd);
 *   }
 *
 *   // Gap optimization after topology is stable:
 *   if (auto gapCmd = busManager.OptimizeGapCount(topology, selfIDs)) {
 *       StagePhyPacket(*gapCmd);
 *   }
 *
 * References:
 * - Apple IOFireWireController.cpp (AssignCycleMaster, finishedBusScan)
 * - IEEE 1394-1995 §8.4 (Self-ID and Arbitration)
 * - IEEE 1394a-2000 §C.2 (Gap Count Optimization)
 * - docs/Bus-Initialization-Features.md
 */
class BusManager {
public:
    struct PhyConfigCommand {
        std::optional<uint8_t> gapCount;
        std::optional<uint8_t> forceRootNodeID;
        std::optional<bool> setContender;
    };
    /**
     * Root node selection policy.
     *
     * Determines how the driver selects the root node (which is also the Cycle Master).
     */
    enum class RootPolicy : uint8_t {
        /**
         * Auto-select based on delegate mode and bad IRM recovery.
         * Default behavior: prefer external devices if capable.
         */
        Auto = 0,

        /**
         * Always force local controller as root.
         * Useful for debugging or when all external devices are unreliable.
         */
        ForceLocal = 1,

        /**
         * Force specific node ID as root.
         * Used when user explicitly specifies a node via configuration.
         */
        ForceNode = 2,

        /**
         * Prefer external devices as root (delegate mode).
         * Offloads cycle master duties to capable external devices.
         */
        Delegate = 3
    };

    /**
     * Configuration for bus management policies.
     */
    struct Config {
        /// Root selection policy
        RootPolicy rootPolicy = RootPolicy::Delegate;

        /// Specific node ID to force as root (only used with ForceNode policy)
        uint8_t forcedRootNodeID = 0xFF;

        /// Enable delegate mode (prefer external devices as root)
        bool delegateCycleMaster = true;

        /// Enable gap count optimization
        bool enableGapOptimization = false;

        /// Force specific gap count (0 = auto-calculate)
        uint8_t forcedGapCount = 0;

        /// Enable forced gap count override
        bool forcedGapFlag = false;
    };

    BusManager() = default;
    ~BusManager() = default;

    /**
     * Set root node selection policy.
     *
     * @param policy Root selection policy
     */
    void SetRootPolicy(RootPolicy policy);

    /**
     * Set forced root node ID (only used with ForceNode policy).
     *
     * @param nodeID Node ID to force as root (0-62)
     */
    void SetForcedRootNode(uint8_t nodeID);

    /**
     * Enable or disable delegate mode.
     *
     * @param enable true = prefer external devices as root
     */
    void SetDelegateMode(bool enable);

    /**
     * Set forced gap count.
     *
     * @param gapCount Gap count value (0-63, 0 = auto-calculate)
     */
    void SetForcedGapCount(uint8_t gapCount);

    /**
     * Get current configuration.
     *
     * @return Current configuration
     */
    const Config& GetConfig() const { return config_; }

    /**
     * Assign cycle master (root node selection).
     *
     * Implements intelligent root node selection based on policy and bus state.
     * Returns a PHY configuration command when the root should change; callers
     * stage the packet and trigger a reset once the bus is quiesced.
     *
     * Scenarios:
     * 1. Forced Root: User explicitly specified a node as root
     * 2. Delegate Mode: Prefer external devices if capable
     * 3. Bad IRM Recovery: Replace root if current IRM is non-functional
     *
     * @param topology Current bus topology snapshot
     * @param badIRMFlags Per-node bad IRM flags (indexed by node ID)
     * @return PHY packet command (root/gap) to emit, or nullopt if no change needed
     *
     * Reference: Apple IOFireWireController::AssignCycleMaster()
     */
    [[nodiscard]] std::optional<PhyConfigCommand> AssignCycleMaster(
        const TopologySnapshot& topology,
        const std::vector<bool>& badIRMFlags);

    /**
     * Optimize gap count based on bus topology.
     *
     * Calculates optimal gap count using hop count and ping times,
     * then builds a PHY packet to apply the new value if needed.
     *
     * @param topology Current bus topology snapshot
     * @param selfIDs Raw Self-ID quadlets for gap consistency check
     * @return PHY packet command containing the new gap/root settings, or nullopt
     *
     * Reference: Apple IOFireWireController::finishedBusScan()
     */
    [[nodiscard]] std::optional<PhyConfigCommand> OptimizeGapCount(
        const TopologySnapshot& topology,
        const std::vector<uint32_t>& selfIDs);

private:
    Config config_;

    /// Previous gap count (to detect if already optimized)
    uint8_t previousGap_ = 0x3F;

    // IEEE 1394a Table C-2: Gap count lookup table
    // Index = max hops or (maxPing - 20) / 9
    // Value = required gap count
    static constexpr uint8_t GAP_TABLE[26] = {
        63, 5, 7, 8, 10, 13, 16, 18, 21, 24, 26, 29, 32, 35, 37, 40,
        43, 46, 48, 51, 54, 57, 59, 62, 63, 63
    };

    /**
     * Find another contender node (excluding specified node).
     *
     * Scans topology for nodes with isIRMCandidate=true and linkActive=true.
     *
     * @param topology Bus topology
     * @param excludeNodeID Node ID to exclude from search
     * @return Node ID of contender, or nullopt if none found
     */
    std::optional<uint8_t> FindOtherContender(
        const TopologySnapshot& topology,
        uint8_t excludeNodeID
    ) const;

    /**
     * Select a good root node (avoiding bad IRM).
     *
     * Picks a suitable root when current IRM is known to be bad.
     * Preference order:
     * 1. Local controller (most reliable)
     * 2. Highest contender that's not marked bad
     * 3. Any node with active link
     *
     * @param topology Bus topology
     * @param badIRMFlags Per-node bad IRM flags
     * @param badIRMNodeID Node ID of known bad IRM
     * @return Node ID to make root
     */
    uint8_t SelectGoodRoot(
        const TopologySnapshot& topology,
        const std::vector<bool>& badIRMFlags,
        uint8_t badIRMNodeID
    ) const;

    [[nodiscard]] PhyConfigCommand BuildPhyConfigCommand(
        std::optional<uint8_t> forceRootNodeID = std::nullopt,
        std::optional<uint8_t> gapCount = std::nullopt) const;

    /**
     * Calculate gap count from hop count.
     *
     * Uses IEEE 1394a Table C-2 to map hop count to safe gap value.
     *
     * @param maxHops Maximum hop count (0-25)
     * @return Gap count (5-63)
     */
    uint8_t CalculateGapFromHops(uint8_t maxHops) const;

    /**
     * Calculate gap count from ping time.
     *
     * Uses IEEE 1394a Table C-2 formula:
     *   if (maxPing >= 29): gap = GAP_TABLE[(maxPing - 20) / 9]
     *   else: gap = 5
     *
     * @param maxPingNs Maximum ping time in nanoseconds
     * @return Gap count (5-63)
     */
    uint8_t CalculateGapFromPing(uint32_t maxPingNs) const;

    /**
     * Extract gap count from Self-ID packet.
     *
     * Self-ID packet 0 format (IEEE 1394-1995 §8.4.2.4):
     *   Bits[31:24] = Node PHY ID
     *   Bits[23:16] = Gap count
     *   ...
     *
     * @param selfIDQuad Self-ID quadlet (packet 0 only)
     * @return Gap count (0-63)
     */
    static uint8_t ExtractGapCount(uint32_t selfIDQuad);

    /**
     * Check if gap counts are consistent across all Self-IDs.
     *
     * Per OHCI spec, all nodes should report same gap count after
     * a PHY packet sets it. Inconsistent gaps trigger re-optimization.
     *
     * @param selfIDs Raw Self-ID quadlets
     * @return true if all gap counts match
     */
    static bool AreGapsConsistent(const std::vector<uint32_t>& selfIDs);
};

} // namespace ASFW::Driver
