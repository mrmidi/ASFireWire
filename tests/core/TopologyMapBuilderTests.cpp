// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// TopologyMapBuilderTests.cpp — unit tests for BuildTopologyMap (FW-20).

#include "Bus/CSR/TopologyMapBuilder.hpp"
#include "Controller/ControllerTypes.hpp"
#include "Common/CSRSpace.hpp"

#include <gtest/gtest.h>
#include <array>

namespace {

using ASFW::Bus::BuildTopologyMap;
using ASFW::Driver::TopologySnapshot;
using ASFW::FW::ComputeBlockCRC16;

TEST(TopologyMapBuilder, EmptyOrOneNodeSnapshot) {
    TopologySnapshot snap{};
    snap.generation = 12;
    snap.nodeCount = 1;
    // Index 0 is the generation/header from OHCI SelfIDCount; no actual Self-ID quadlets.
    snap.rawSelfIdQuadlets = { 0x8000000Cu }; 

    std::array<uint32_t, 256> out{};
    const uint32_t length = BuildTopologyMap(snap, snap.generation, out);

    // Should return 3 quadlets: header, generation, counts (selfIdCount = 0)
    EXPECT_EQ(length, 3u);

    // Verify out[0]: (selfIdCount + 2) << 16 | CRC16
    // selfIdCount = 0 => length = 2. out[0] = 0x00020000 | CRC
    EXPECT_EQ(out[0] >> 16, 2u);

    // Verify out[1]: generation
    EXPECT_EQ(out[1], 12u);

    // Verify out[2]: (nodeCount << 16) | selfIdCount
    EXPECT_EQ(out[2], (1u << 16) | 0u);

    // Verify CRC covers out[1] and out[2]
    std::array<uint32_t, 2> crcBlock = { out[1], out[2] };
    const uint16_t expectedCrc = ComputeBlockCRC16(crcBlock);
    EXPECT_EQ(out[0] & 0xFFFFu, expectedCrc);
}

TEST(TopologyMapBuilder, MultipleNodesWithSelfIDs) {
    TopologySnapshot snap{};
    snap.generation = 42;
    snap.nodeCount = 3;
    // Index 0 is the header. Indices 1, 2, 3 are raw self-IDs verbatim.
    snap.rawSelfIdQuadlets = {
        0x8000000Cu, // OHCI header (stripped)
        0x50515253u, // Self-ID 0
        0x60616263u, // Self-ID 1
        0x70717273u  // Self-ID 2
    };

    std::array<uint32_t, 256> out{};
    const uint32_t length = BuildTopologyMap(snap, snap.generation, out);

    // Should return 3 + 3 = 6 quadlets
    EXPECT_EQ(length, 6u);

    // selfIdCount = 3 => (3 + 2) = 5
    EXPECT_EQ(out[0] >> 16, 5u);
    EXPECT_EQ(out[1], 42u);
    EXPECT_EQ(out[2], (3u << 16) | 3u);
    EXPECT_EQ(out[3], 0x50515253u);
    EXPECT_EQ(out[4], 0x60616263u);
    EXPECT_EQ(out[5], 0x70717273u);

    // Verify CRC coverage covers out[1..5]
    std::span<const uint32_t> crcSpan(&out[1], 5);
    const uint16_t expectedCrc = ComputeBlockCRC16(crcSpan);
    EXPECT_EQ(out[0] & 0xFFFFu, expectedCrc);
}

TEST(TopologyMapBuilder, SpansBoundSafety) {
    TopologySnapshot snap{};
    snap.generation = 1;
    snap.nodeCount = 255;
    
    // Create 300 mock quadlets (exceeding the 256-quadlet span limit)
    snap.rawSelfIdQuadlets.push_back(0x8000000Cu);
    for (size_t i = 0; i < 300; ++i) {
        snap.rawSelfIdQuadlets.push_back(static_cast<uint32_t>(i));
    }

    std::array<uint32_t, 256> out{};
    const uint32_t length = BuildTopologyMap(snap, snap.generation, out);

    // BuildTopologyMap clamps selfIdCount to 253 to remain within 256 bounds.
    // Index 0, 1, 2 are taken. So max 253 self-IDs can be copied.
    EXPECT_EQ(length, 256u);
    // But verify we only wrote up to out[255] and didn't overflow/crash.
    EXPECT_EQ(out[3], 0u);
    EXPECT_EQ(out[255], 252u);
}

} // namespace
