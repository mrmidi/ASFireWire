// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// SpeedMapService.cpp — see SpeedMapService.hpp

#include "SpeedMapService.hpp"
#include <algorithm>
#include <cstring>
#include <queue>

namespace ASFW::Bus {

namespace {
constexpr uint32_t kSpeedMapCSRQuadlets = 256;
constexpr uint32_t kSpeedMapPayloadQuadlets = kSpeedMapCSRQuadlets - 1;
constexpr uint32_t kSpeedMapEntriesPerQuadlet = 16;
constexpr uint32_t kSpeedMapAddressableEntries =
    kSpeedMapPayloadQuadlets * kSpeedMapEntriesPerQuadlet;
} // namespace

SpeedMapService::SpeedMapService() noexcept {
    Invalidate(0);
}

void SpeedMapService::Invalidate(uint32_t generation) noexcept {
    snapshot_ = {};
    snapshot_.generation = generation;
    snapshot_.status = SpeedMapStatus::Invalid;

    encoded_.fill(0);
    // SPEED_MAP is obsolete in IEEE 1394-2008; keep the legacy 0x400-byte CSR
    // window self-consistent instead of advertising a larger unavailable image.
    encoded_[0] = (kSpeedMapPayloadQuadlets << 16) | (generation & 0xFFu);
    encodedQuadletCount_ = kSpeedMapCSRQuadlets;
}

bool SpeedMapService::PublishFromTopology(const ASFW::Driver::TopologySnapshot& topology) noexcept {
    snapshot_.generation = topology.generation;
    snapshot_.nodeCount = topology.nodeCount;
    snapshot_.localNodeId = topology.localNodeId;
    snapshot_.rootNodeId = topology.rootNodeId;
    snapshot_.topologyValid = (topology.graphStatus == ASFW::Driver::TopologyGraphStatus::Valid);
    snapshot_.betaRepeatersPresent = topology.betaRepeatersPresent;

    if (!snapshot_.topologyValid || snapshot_.nodeCount == 0) {
        Invalidate(topology.generation);
        return false;
    }

    if (ComputeMatrix(topology)) {
        snapshot_.status = SpeedMapStatus::Valid;
    } else {
        snapshot_.status = SpeedMapStatus::ConservativeFallback;
    }

    EncodeCSRImage();
    return true;
}

bool SpeedMapService::ReadQuadlet(uint32_t offsetWithinSpeedMap,
                                  uint32_t& outValue) const noexcept {
    const uint32_t quadIndex = offsetWithinSpeedMap / 4;
    if (quadIndex >= encodedQuadletCount_) {
        return false;
    }

    outValue = encoded_[quadIndex];
    return true;
}

bool SpeedMapService::ComputeMatrix(const ASFW::Driver::TopologySnapshot& topology) noexcept {
    // 1. Initialize matrix with Unknown
    for (int i = 0; i < 64; ++i) {
        for (int j = 0; j < 64; ++j) {
            snapshot_.speedMatrix[i][j] = FireWireSpeedCode::Unknown;
        }
        // Self-speed is node's own max speed (capped at S400 for legacy map)
        if (i < topology.physical.nodes.size()) {
            uint32_t mbps = topology.physical.nodes[i].maxSpeedMbps;
            if (mbps >= 400) snapshot_.speedMatrix[i][i] = FireWireSpeedCode::S400;
            else if (mbps >= 200) snapshot_.speedMatrix[i][i] = FireWireSpeedCode::S200;
            else snapshot_.speedMatrix[i][i] = FireWireSpeedCode::S100;
        }
    }

    // 2. Perform BFS from each node to find path speeds
    const auto& nodes = topology.physical.nodes;
    bool allPathsFound = true;

    for (uint8_t startNode = 0; startNode < nodes.size(); ++startNode) {
        if (!nodes[startNode].linkActive) continue;

        // Simple BFS to find all reachable nodes
        std::queue<uint8_t> q;
        q.push(startNode);
        
        bool visited[64]{false};
        visited[startNode] = true;

        while (!q.empty()) {
            uint8_t u = q.front();
            q.pop();

            for (const auto& link : nodes[u].links) {
                if (!link.connected) continue;
                uint8_t v = link.remoteNodeId;
                if (v >= 64 || visited[v]) continue;

                // Edge speed is min of the two nodes (capped at S400)
                uint32_t mbpsU = nodes[u].maxSpeedMbps;
                uint32_t mbpsV = nodes[v].maxSpeedMbps;
                uint32_t minMbps = std::min(mbpsU, mbpsV);
                
                FireWireSpeedCode edgeSpeed;
                if (minMbps >= 400) edgeSpeed = FireWireSpeedCode::S400;
                else if (minMbps >= 200) edgeSpeed = FireWireSpeedCode::S200;
                else edgeSpeed = FireWireSpeedCode::S100;

                // Path speed to v is min(path speed to u, edge speed u->v)
                FireWireSpeedCode pathSpeedToU = (u == startNode) ? FireWireSpeedCode::Unknown : snapshot_.speedMatrix[startNode][u];
                
                FireWireSpeedCode finalSpeed;
                if (pathSpeedToU == FireWireSpeedCode::Unknown) {
                    finalSpeed = edgeSpeed;
                } else {
                    finalSpeed = static_cast<uint8_t>(pathSpeedToU) < static_cast<uint8_t>(edgeSpeed) ? pathSpeedToU : edgeSpeed;
                }

                snapshot_.speedMatrix[startNode][v] = finalSpeed;
                snapshot_.speedMatrix[v][startNode] = finalSpeed;
                
                visited[v] = true;
                q.push(v);
            }
        }
        
        // Verify all link-active nodes were reached
        for (uint8_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i].linkActive && !visited[i]) {
                allPathsFound = false;
            }
        }
    }

    // 3. Fallback for unknown entries
    for (int i = 0; i < 64; ++i) {
        for (int j = 0; j < 64; ++j) {
            if (snapshot_.speedMatrix[i][j] == FireWireSpeedCode::Unknown) {
                snapshot_.speedMatrix[i][j] = FireWireSpeedCode::S100;
            }
        }
    }

    return allPathsFound;
}

void SpeedMapService::EncodeCSRImage() noexcept {
    encoded_.fill(0);
    // q0: [length:16][generation:16]. SPEED_MAP is obsolete in IEEE 1394-2008,
    // so ASFW exposes only the legacy 0x400-byte software CSR window.
    encoded_[0] = (kSpeedMapPayloadQuadlets << 16) | (snapshot_.generation & 0xFFu);

    // q1..q255: addressable 2-bit entries within the legacy CSR window.
    for (int i = 0; i < 64; ++i) {
        for (int j = 0; j < 64; ++j) {
            uint32_t index = i * 64 + j;
            if (index >= kSpeedMapAddressableEntries) {
                continue;
            }
            uint8_t code = static_cast<uint8_t>(snapshot_.speedMatrix[i][j]);
            if (code > 2) code = 2; // Cap at S400 for legacy map

            // Legacy packed order: index % 16 entries per quadlet.
            encoded_[index / 16 + 1] |= (static_cast<uint32_t>(code) << (2 * (index % 16)));
        }
    }

    snapshot_.encodedLengthQuadlets = kSpeedMapCSRQuadlets;
}

} // namespace ASFW::Bus
