#pragma once

#include <optional>
#include <vector>

#include "../Controller/ControllerTypes.hpp"
#include "SelfIDCapture.hpp"

namespace ASFW::Driver {

// Transforms decoded Self-ID data into immutable topology snapshots and offers
// diffing support so the service can log concise bus changes.
class TopologyManager {
public:
    TopologyManager();

    void Reset();
    std::optional<TopologySnapshot> UpdateFromSelfID(const SelfIDCapture::Result& result,
                                                     uint64_t timestamp,
                                                     uint32_t nodeIDReg);

    std::optional<TopologySnapshot> LatestSnapshot() const;
    std::optional<TopologySnapshot> CompareAndSwap(std::optional<TopologySnapshot> previous);

    /**
     * Mark a node as having a non-functional IRM implementation.
     *
     * Called when IRM capability verification fails (read/CAS test).
     * Bad IRM nodes will be avoided when AssignCycleMaster selects root.
     *
     * @param nodeID Node ID to mark as bad IRM (0-62)
     *
     * Reference: Apple IOFireWireController.cpp:2697 - sets scan->fIRMisBad
     */
    void MarkNodeAsBadIRM(uint8_t nodeID);

    /**
     * Check if a node is marked as having bad IRM.
     *
     * @param nodeID Node ID to check (0-62)
     * @return true if node is marked as bad IRM
     */
    bool IsNodeBadIRM(uint8_t nodeID) const;

    /**
     * Get bad IRM flags for all nodes.
     *
     * Returns a vector indexed by node ID, where true = bad IRM.
     * Used by BusManager::AssignCycleMaster() to avoid bad nodes.
     *
     * @return Vector of bad IRM flags (size 63, indexed by node ID)
     */
    const std::vector<bool>& GetBadIRMFlags() const { return badIRMFlags_; }

    /**
     * Clear all bad IRM flags (called on bus reset).
     *
     * IRM verification must be re-done after each bus reset since
     * node IDs may change and previously-bad devices may have been
     * replaced or fixed.
     */
    void ClearBadIRMFlags();

    /**
     * Extract gap count values from Self-ID packets.
     *
     * Gap count is encoded in bits 16-21 of Self-ID packet 0 (6 bits).
     * Per IEEE 1394-1995 §8.4.6.2.2, all nodes should advertise the
     * same gap count after bus arbitration completes.
     *
     * This method extracts the gap count from each Self-ID sequence,
     * allowing GapCountOptimizer to detect inconsistencies or invalid
     * values (gap=0).
     *
     * @param selfIDs Raw Self-ID packets (from SelfIDCapture::Result)
     * @return Vector of gap counts (one per node, indexed by node ID)
     *
     * Reference:
     * - IEEE 1394-1995 Figure 8-7 (Self-ID packet format)
     * - Apple IOFireWireController.cpp:3378-3401 (gap consistency check)
     */
    static std::vector<uint8_t> ExtractGapCounts(const std::vector<uint32_t>& selfIDs);

private:
    std::optional<TopologySnapshot> latest_;

    /// Per-node bad IRM flags (indexed by node ID, 0-62)
    /// true = node failed IRM verification (read/CAS test)
    std::vector<bool> badIRMFlags_;
};

} // namespace ASFW::Driver
