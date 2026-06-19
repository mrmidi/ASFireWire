#pragma once

#include <expected>
#include <vector>

#include "TopologyTypes.hpp"

namespace ASFW::Driver {

/**
 * @class SelfIDTopologyNormalizer
 * @brief Implements IEEE 1394-2008 Annex P topology reconstruction and normalization.
 *
 * This class provides two primary transformations:
 * 1. Building a PhysicalTopologyGraph (undirected tree with bidirectional links)
 *    using the Annex P stack algorithm.
 * 2. Normalizing that graph from a local observer's perspective (directed tree).
 */
class SelfIDTopologyNormalizer {
public:
    /**
     * Build the physical topology graph from ordered node records.
     *
     * @param records Ordered Self-ID node records from SelfIDStreamParser.
     * @param localPhysicalId The physical ID of the local node.
     * @return The physical graph, or a build error.
     */
    static std::expected<PhysicalTopologyGraph, TopologyBuildError>
    BuildPhysicalGraph(const std::vector<SelfIDNodeRecord>& records,
                       uint8_t localPhysicalId);

    /**
     * Normalize a physical graph from the perspective of the local observer.
     *
     * @param physical The physical graph to normalize.
     * @param localPhysicalId The physical ID of the local node (observer).
     * @return The normalized graph, or a build error.
     */
    static std::expected<NormalizedTopologyGraph, TopologyBuildError>
    NormalizeFromLocal(const PhysicalTopologyGraph& physical,
                       uint8_t localPhysicalId);

private:
    static std::optional<uint8_t>
    FirstUnresolvedParentPort(const TopologyNodeRecord& node) noexcept;

    static void ConnectBidirectional(TopologyNodeRecord& parent,
                                     uint8_t parentPort,
                                     TopologyNodeRecord& child,
                                     uint8_t childPort) noexcept;

    /**
     * Compute the bus diameter in cable hops: the longest path between any two
     * nodes of the reconstructed (acyclic) physical tree. This is the value used
     * for IEEE 1394-2008 Table E.1 gap_count optimization. Returns 0 for a
     * single-node (or empty) bus.
     */
    static uint8_t ComputeBusDiameter(const PhysicalTopologyGraph& graph) noexcept;
};

} // namespace ASFW::Driver
