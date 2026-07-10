// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// TopologyMapBuilder.hpp — pure helper to construct IEEE 1394 topology map (FW-20).

#pragma once

#include <span>
#include <cstdint>

namespace ASFW::Driver {
struct TopologySnapshot;
}

namespace ASFW::Bus {

/**
 * @brief Pure helper to build the IEEE 1394 topology map block (256 quadlets).
 *
 * Format follows the Linux firewire core format (core-topology.c):
 * - out[0] = (selfIdCount + 2) << 16 | CRC16 (filled last)
 * - out[1] = topologyMapGeneration
 * - out[2] = (nodeCount << 16) | selfIdCount
 * - out[3..] = raw self-id quadlets verbatim (starting at index 1 of the raw snapshot quadlets)
 *
 * Returns the total number of quadlets filled in out (always selfIdCount + 3).
 */
uint32_t BuildTopologyMap(const ASFW::Driver::TopologySnapshot& snapshot,
                          uint32_t topologyMapGeneration,
                          std::span<uint32_t, 256> out) noexcept;

} // namespace ASFW::Bus
