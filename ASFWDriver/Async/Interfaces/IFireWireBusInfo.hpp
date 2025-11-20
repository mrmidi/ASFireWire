#pragma once
#include "../../Common/FWCommon.hpp"  // Generation, NodeId, FwSpeed
#include <cstdint>

namespace ASFW::Async {

/**
 * @brief Pure virtual interface for FireWire bus state queries.
 *
 * Separated from IFireWireBusOps to avoid circular dependencies:
 * - ControllerCore owns TopologyManager
 * - FireWireBusImpl queries TopologyManager for speed/hop count
 * - TopologyManager MUST NOT call back into IFireWireBus*
 *
 * Design Principle: Read-only, const methods only. No state mutation.
 */
class IFireWireBusInfo {
public:
    virtual ~IFireWireBusInfo() = default;

    /**
     * @brief Get negotiated speed between local controller and remote node.
     *
     * @param nodeId Target node (0-63)
     * @return FwSpeed Maximum usable speed, or S100 if unknown
     *
     * Internally uses TopologyManager to calculate:
     * min(local_speed, remote_speed, all_hop_speeds_on_path)
     */
    virtual FW::FwSpeed GetSpeed(FW::NodeId nodeId) const = 0;

    /**
     * @brief Calculate hop count between two nodes.
     *
     * @param nodeA First node (0-63)
     * @param nodeB Second node (0-63)
     * @return Hop count (0 = same node, 1+ = tree distance, UINT32_MAX = unknown)
     *
     * Uses self-ID topology data. Returns UINT32_MAX if topology incomplete.
     */
    virtual uint32_t HopCount(FW::NodeId nodeA, FW::NodeId nodeB) const = 0;

    /**
     * @brief Get current bus generation.
     *
     * @return Generation number (increments on each bus reset)
     *
     * Used for validating async operations. Mismatched generations cause kStaleGeneration status.
     */
    virtual FW::Generation GetGeneration() const = 0;

    /**
     * @brief Get local node ID.
     *
     * @return NodeId (0-63), or kInvalidNodeId if bus not initialized
     */
    virtual FW::NodeId GetLocalNodeID() const = 0;
};

} // namespace ASFW::Async
