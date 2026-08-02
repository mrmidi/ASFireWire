// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// TopologyMapBuilder.cpp — see TopologyMapBuilder.hpp

#include "TopologyMapBuilder.hpp"
#include "../../Controller/ControllerTypes.hpp"
#include "../../Common/CSRSpace.hpp"

namespace ASFW::Bus {

uint32_t BuildTopologyMap(const ASFW::Driver::TopologySnapshot& snapshot,
                          uint32_t topologyMapGeneration,
                          std::span<uint32_t, 256> out) noexcept {
    // Clear out initially
    for (auto& val : out) {
        val = 0;
    }

    const auto& rawQuads = snapshot.rawSelfIdQuadlets;
    // rawQuads[0] is the generation/header from OHCI SelfIDCount; the actual
    // Self-ID quadlets on the bus start at index 1.
    uint32_t selfIdCount = (rawQuads.size() > 1) ? static_cast<uint32_t>(rawQuads.size() - 1) : 0;
    if (selfIdCount > 253) {
        selfIdCount = 253;
    }
    const uint32_t nodeCount = snapshot.nodeCount;

    // Build map header
    out[0] = (selfIdCount + 2) << 16; // CRC is low 16 bits, calculated later
    out[1] = topologyMapGeneration;
    out[2] = (nodeCount << 16) | selfIdCount;

    // Copy Self-ID quadlets verbatim into the map
    for (uint32_t i = 0; i < selfIdCount; ++i) {
        out[i + 3] = rawQuads[i + 1];
    }

    // Compute CRC16 over quadlets 1..2+selfIdCount
    const uint32_t crcCoverageCount = 2 + selfIdCount;
    const uint16_t crc = ASFW::FW::ComputeBlockCRC16(out.subspan(1, crcCoverageCount));
    out[0] |= crc;

    return 3 + selfIdCount;
}

} // namespace ASFW::Bus
