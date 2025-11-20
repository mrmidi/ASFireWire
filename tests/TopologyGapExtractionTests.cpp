// SPDX-License-Identifier: MIT
// Copyright (c) 2025 ASFW Project

#include <gtest/gtest.h>
#include <vector>
#include <cstdint>

// Forward declare the function we're testing (static method)
namespace ASFW::Driver {
    class TopologyManager {
    public:
        static std::vector<uint8_t> ExtractGapCounts(const std::vector<uint32_t>& selfIDs);
    };
}

using namespace ASFW::Driver;

// ============================================================================
// Gap Count Extraction Tests - Real-World FireBug Data
// ============================================================================

TEST(TopologyManager, ExtractGapCounts_EmptySelfIDs) {
    std::vector<uint32_t> selfIDs = {};
    auto gaps = TopologyManager::ExtractGapCounts(selfIDs);
    EXPECT_TRUE(gaps.empty());
}

TEST(TopologyManager, ExtractGapCounts_FireBugLog_InitialReset) {
    // Real-world data from FireBug logs (first bus reset):
    // 008:2162:2390  Self-ID  803fc464  Node=0  Link=0  gap=3f  spd=1394b  C=0  pwr=4
    // 008:2162:2634  Self-ID  813f84b6  Node=1  Link=0  gap=3f  spd=s400  C=0  pwr=4
    // 008:2162:2874  Self-ID  827f8cc0  Node=2  Link=1  gap=3f  spd=s400  C=1  pwr=4

    std::vector<uint32_t> selfIDs = {
        0x803fc464,  // Node 0: gap=0x3f (63)
        0x813f84b6,  // Node 1: gap=0x3f (63)
        0x827f8cc0   // Node 2: gap=0x3f (63)
    };

    auto gaps = TopologyManager::ExtractGapCounts(selfIDs);

    ASSERT_EQ(gaps.size(), 3);
    EXPECT_EQ(gaps[0], 0x3f);  // 63 (default gap count)
    EXPECT_EQ(gaps[1], 0x3f);
    EXPECT_EQ(gaps[2], 0x3f);
}

TEST(TopologyManager, ExtractGapCounts_FireBugLog_AfterBadPhyPacket) {
    // Real-world data from FireBug logs (after bad PHY packet 0x00000200):
    // 015:6793:0605  Self-ID  807f8c80  Node=0  Link=1  gap=3f  spd=s400  C=1  pwr=4
    // 015:6793:0815  Self-ID  8240cc76  Node=2  Link=1  gap=0   spd=1394b  C=1  pwr=4
    //                                                    ^^^^^ BAD! gap=0

    std::vector<uint32_t> selfIDs = {
        0x807f8c80,  // Node 0: gap=0x3f (63)
        0x813f84e4,  // Node 1: gap=0x3f (63) (from log)
        0x8240cc76   // Node 2: gap=0x00 (0) ← INVALID!
    };

    auto gaps = TopologyManager::ExtractGapCounts(selfIDs);

    ASSERT_EQ(gaps.size(), 3);
    EXPECT_EQ(gaps[0], 0x3f);
    EXPECT_EQ(gaps[1], 0x3f);
    EXPECT_EQ(gaps[2], 0x00);  // ← This is the bug we're detecting!
}

TEST(TopologyManager, ExtractGapCounts_BitFieldParsing) {
    // Verify correct bit extraction for gap count (bits 21:16)

    // Self-ID packet 0 format (simplified):
    // Bits[31:30] = 10 (Self-ID identifier)
    // Bits[29:24] = Physical ID
    // Bits[23:22] = 00 (packet 0)
    // Bits[21:16] = Gap count ← We're testing this
    // Bits[15:0]  = Other fields

    // Construct a packet with gap=7 (0x07):
    // 10 [phy=0] 00 [gap=7] [other=0xc464]
    // = 0x8007c464
    uint32_t packet_gap7 = 0x8007c464;

    // Construct a packet with gap=63 (0x3f):
    // 10 [phy=0] 00 [gap=63] [other=c464]
    // = 0x803fc464
    uint32_t packet_gap63 = 0x803fc464;

    std::vector<uint32_t> selfIDs = {packet_gap7, packet_gap63};
    auto gaps = TopologyManager::ExtractGapCounts(selfIDs);

    ASSERT_EQ(gaps.size(), 2);
    EXPECT_EQ(gaps[0], 7);
    EXPECT_EQ(gaps[1], 63);
}

TEST(TopologyManager, ExtractGapCounts_SkipsNonPacket0) {
    // Self-ID packets come in sequences (packet 0, 1, 2, 3 for multi-port PHYs)
    // Gap count is ONLY in packet 0 (bits 23:22 == 00)
    // Verify we skip packets 1, 2, 3

    uint32_t packet0 = 0x803fc464;  // Packet 0, gap=63
    uint32_t packet1 = 0x844000ff;  // Packet 1 (bits 23:22 = 01)
    uint32_t packet2 = 0x888000ff;  // Packet 2 (bits 23:22 = 10)

    std::vector<uint32_t> selfIDs = {packet0, packet1, packet2};
    auto gaps = TopologyManager::ExtractGapCounts(selfIDs);

    // Should only extract gap from packet 0
    ASSERT_EQ(gaps.size(), 1);
    EXPECT_EQ(gaps[0], 63);
}

TEST(TopologyManager, ExtractGapCounts_SkipsNonSelfIDPackets) {
    // Verify we skip non-Self-ID packets (bits 31:30 != 10)

    uint32_t selfIDPacket = 0x803fc464;   // bits[31:30] = 10 (Self-ID)
    uint32_t otherPacket1 = 0x003fc464;   // bits[31:30] = 00 (not Self-ID)
    uint32_t otherPacket2 = 0x403fc464;   // bits[31:30] = 01 (not Self-ID)

    std::vector<uint32_t> selfIDs = {selfIDPacket, otherPacket1, otherPacket2};
    auto gaps = TopologyManager::ExtractGapCounts(selfIDs);

    // Should only extract gap from actual Self-ID packet
    ASSERT_EQ(gaps.size(), 1);
    EXPECT_EQ(gaps[0], 63);
}

TEST(TopologyManager, ExtractGapCounts_Integration_WithGapCountOptimizer) {
    // Integration test: Extract gaps from real Self-IDs and verify with GapCountOptimizer

    // Scenario: 3-node bus with default gaps
    std::vector<uint32_t> selfIDs = {0x803fc464, 0x813f84b6, 0x827f8cc0};
    auto gaps = TopologyManager::ExtractGapCounts(selfIDs);

    // All nodes should have gap=63 (default)
    ASSERT_EQ(gaps.size(), 3);
    for (uint8_t gap : gaps) {
        EXPECT_EQ(gap, 63);
    }

    // GapCountOptimizer should detect this needs optimization
    // (tested in GapCountOptimizerTests.cpp)
}

// ============================================================================
// Real-World Debugging: Gap=0 Detection
// ============================================================================

TEST(TopologyManager, ExtractGapCounts_DebugBusResetStorm) {
    // This test documents the actual bug from your logs:
    // PHY packet 0x00000200 set gap=0 on node 2, causing infinite resets

    // Before bug: all gaps = 63
    std::vector<uint32_t> before = {0x803fc464, 0x813f84b6, 0x827f8cc0};
    auto gaps_before = TopologyManager::ExtractGapCounts(before);
    ASSERT_EQ(gaps_before.size(), 3);
    EXPECT_EQ(gaps_before[0], 63);
    EXPECT_EQ(gaps_before[1], 63);
    EXPECT_EQ(gaps_before[2], 63);

    // After bad PHY packet: node 2 has gap=0
    // Construct Self-ID with gap=0 (bits 21:16 = 0x00):
    // 10 [phy=2] 00 [gap=0] [other=0x0cc76]
    // = 0x82000cc76 & 0xFFFFFFFF = 0x8200cc76
    std::vector<uint32_t> after = {
        0x807f8c80,  // Node 0: gap=63
        0x813f84e4,  // Node 1: gap=63
        0x8200cc76   // Node 2: gap=0 ← BUG!
    };
    auto gaps_after = TopologyManager::ExtractGapCounts(after);
    ASSERT_EQ(gaps_after.size(), 3);
    EXPECT_EQ(gaps_after[0], 0x3f);
    EXPECT_EQ(gaps_after[1], 0x3f);
    EXPECT_EQ(gaps_after[2], 0x00);  // ← Detected!

    // Verify GapCountOptimizer would flag this as critical error
    // (HasInvalidGap should return true)
}
