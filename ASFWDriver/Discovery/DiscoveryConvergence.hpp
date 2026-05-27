#pragma once

#include <cstddef>

#include "../Controller/ControllerTypes.hpp"
#include "DiscoveryTypes.hpp"

namespace ASFW::Discovery {

[[nodiscard]] inline bool HasRemoteLinkActiveNode(const ASFW::Driver::TopologySnapshot& topology) {
    if (!topology.localNodeId.has_value()) {
        return false;
    }

    const uint8_t localNodeId = *topology.localNodeId;
    for (const auto& node : topology.nodes) {
        if (node.nodeId != localNodeId && node.linkActive) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool IsZeroRomScanInconclusive(
    Generation scanGeneration,
    std::size_t romCount,
    const ASFW::Driver::TopologySnapshot& topology) {
    return romCount == 0U && topology.generation == scanGeneration.value &&
           HasRemoteLinkActiveNode(topology);
}

} // namespace ASFW::Discovery
