// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025 ASFW Project
//
// TopologySpeed.hpp — Self-ID derived path speed between two nodes.

#pragma once

#include "TopologyTypes.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ASFW::Driver {

/**
 * @brief Highest speed usable between two nodes, from Self-ID evidence alone.
 *
 * Walks the Self-ID link graph and takes the minimum PHY speed along the path,
 * which is the speed a packet between the two nodes must be sent at
 * (IEEE 1394-2008 §4.2.3: a repeating PHY cannot resend faster than it
 * received). Returns nullopt when the topology is not valid or the nodes are
 * not connected, so callers decide their own conservative fallback rather than
 * silently receiving S100.
 *
 * This is deliberately the *only* speed source appropriate for isochronous
 * traffic. Apple resolves isoch speed from the PHY
 * (IOFWIsochChannel.cpp:653 — `fControl->getLink()->getPhySpeed()`), and keeps
 * its per-node-pair `fSpeedVector` — which async transmit reads at
 * IOFireWireController.cpp:7058, and which `setNodeSpeed(..., FWSpeed(...) - 1)`
 * demotes when a scan fails (:2755-2759) — out of the isoch path entirely. A
 * device that mishandles async requests at S400 has told us nothing about what
 * its isochronous receiver can do.
 *
 * @note Uncapped by design. SpeedMapService clamps to S400 because the legacy
 *       SPEED_MAP CSR image is a conservative diagnostic surface; a transmit
 *       speed decision must not inherit that clamp.
 */
[[nodiscard]] inline std::optional<uint8_t> PathSpeedCodeBetween(const TopologySnapshot& topology,
                                                                 uint8_t nodeA,
                                                                 uint8_t nodeB) noexcept {
    if (topology.graphStatus != TopologyGraphStatus::Valid) {
        return std::nullopt;
    }
    if (nodeA >= kMaxPhysicalIds || nodeB >= kMaxPhysicalIds) {
        return std::nullopt;
    }

    const auto& nodes = topology.physical.nodes;
    const auto findNode = [&nodes](uint8_t id) -> const TopologyNodeRecord* {
        for (const auto& node : nodes) {
            if (node.physicalId == id) {
                return &node;
            }
        }
        return nullptr;
    };

    const auto* recordA = findNode(nodeA);
    const auto* recordB = findNode(nodeB);
    if (recordA == nullptr || !recordA->linkActive ||
        recordB == nullptr || !recordB->linkActive) {
        return std::nullopt;
    }

    if (nodeA == nodeB) {
        return static_cast<uint8_t>(recordA->speedCode);
    }

    // Breadth-first over the Self-ID link graph, carrying the running minimum.
    // The bus is a tree, so the first time a node is reached is along its only
    // path and no revisit can improve the answer. Intermediate repeating PHYs
    // are traversed regardless of linkActive (Linux core-topology.c:263–320:
    // PHYs repeat packets at hardware level even while the link layer is inactive).
    std::array<uint8_t, kMaxPhysicalIds> best{};
    std::array<bool, kMaxPhysicalIds> seen{};
    best.fill(0);
    seen.fill(false);

    std::array<uint8_t, kMaxPhysicalIds> queue{};
    size_t head = 0;
    size_t tail = 0;

    seen[nodeA] = true;
    best[nodeA] = static_cast<uint8_t>(recordA->speedCode);
    queue[tail++] = nodeA;

    while (head < tail) {
        const uint8_t current = queue[head++];

        const auto* record = findNode(current);
        if (record == nullptr) {
            continue;
        }

        for (uint8_t port = 0; port < record->portCount; ++port) {
            const auto& link = record->links[port];
            if (!link.connected) {
                continue;
            }

            const uint8_t neighbour = link.remoteNodeId;
            if (neighbour >= kMaxPhysicalIds || seen[neighbour]) {
                continue;
            }

            const auto* neighbourRecord = findNode(neighbour);
            if (neighbourRecord == nullptr) {
                continue;
            }

            const uint8_t neighbourSpeed = static_cast<uint8_t>(neighbourRecord->speedCode);
            seen[neighbour] = true;
            best[neighbour] = std::min(best[current], neighbourSpeed);

            if (neighbour == nodeB) {
                return best[neighbour];
            }

            queue[tail++] = neighbour;
        }
    }

    return std::nullopt;
}

} // namespace ASFW::Driver
