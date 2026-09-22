// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025 ASFW Project
//
// TopologySpeed.hpp — Self-ID derived path speed between two nodes.

#pragma once

#include "TopologyTypes.hpp"
#include "../Common/FWTypes.hpp"

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

/**
 * @brief Resolve the isochronous transmission speed for a node.
 *
 * Walks the Self-ID topology graph for the maximum PHY path speed between local
 * node and target node, then bounds it by the validated operational link speed
 * (@p operationalLimit, i.e. policy.localToNode).
 *
 * Rationale & Change History:
 * Commit 86324deef previously decoupled isochronous speed from async operational speed,
 * forcing isoch to the raw Self-ID PHY speed (S400) under the assumption that:
 *   "SpeedPolicy demotes localToNode when a request times out, which is right
 *    for async and wrong for isoch: charging isoch at S200 costs twice the
 *    bandwidth units of S400 for a device whose PHY was never the problem."
 *
 * That assumption was flawed:
 * 1. The IRM bandwidth exhaustion at S200 was actually caused by an erroneous 512-unit
 *    per-stream gap overhead charge against BANDWIDTH_AVAILABLE on unoptimized buses.
 *    With Apple IOFWIsochChannel wire parity restored (commit 84426354), zero gap overhead
 *    is subtracted from the IRM ledger. All 4 streams of the Midas Venice F24 at S200
 *    consume only 3,232 units out of 4,915, fitting comfortably on any bus.
 * 2. Real-world links and device link layers may fail when driven faster than their
 *    verified operational speed. Forcing S400 on devices whose physical link or hardware
 *    cannot reliably sustain S400 (e.g. Midas Venice F24, which runs stably at S200 under
 *    Apple's native IOFireWireFamily) causes packet corruption, timestamp timeouts, and
 *    bus reset loops.
 *
 * Reference Stack Alignment:
 * - Linux (drivers/firewire/core-device.c:615-641, sound/firewire/amdtp-stream.c, dice-stream.c:194):
 *   Linux derives `device->max_speed` from PHY path speed, but actively checks Config ROM
 *   `link_spd` and steps down `device->max_speed--` if trial quadlet reads fail. Linux sound
 *   drivers then use `device->max_speed` for both IRM reservations and isochronous streaming.
 * - Apple IOFireWireFamily (IOFireWireDevice.cpp:2097-2102, IOFireWireController.cpp:2746-2760,
 *   IOFWIsochChannel.cpp:653):
 *   Apple steps down `setNodeSpeed()` during discovery when speed verification fails, and
 *   allows device property overrides via `fMaxSpeed`.
 *
 * While IEEE 1394 isochronous broadcast packets carry no destination node ID in the packet header
 * and their only strict hardware PHY constraint is repeater port capability (IEEE Std 1394-2008),
 * bounding isochronous transmission to the verified operational link speed is a safe, reference-aligned
 * policy that avoids overdriving fragile hardware or cables.
 */
[[nodiscard]] inline FW::FwSpeed ResolveIsochSpeed(const std::optional<TopologySnapshot>& topology,
                                                   uint8_t nodeId,
                                                   FW::FwSpeed operationalLimit) noexcept {
    if (!topology.has_value() || topology->localNodeId == kInvalidPhysicalId) {
        return operationalLimit;
    }

    const auto pathSpeed = PathSpeedCodeBetween(*topology, topology->localNodeId, nodeId);
    if (!pathSpeed.has_value()) {
        return operationalLimit;
    }

    const auto phySpeed = static_cast<FW::FwSpeed>(*pathSpeed);
    return std::min(phySpeed, operationalLimit);
}

} // namespace ASFW::Driver
