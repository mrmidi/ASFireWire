// SPDX-License-Identifier: MIT
// Copyright (c) 2025 ASFW Project

#include <gtest/gtest.h>
#include "ASFWDriver/Bus/GapCountOptimizer.hpp"

using namespace ASFW::Driver;

// ============================================================================
// Hop Count Calculation Tests
// ============================================================================

TEST(GapCountOptimizer, CalculateFromHops_SingleNode) {
    // Single node (no hops)
    EXPECT_EQ(GapCountOptimizer::CalculateFromHops(0), 63);
}

TEST(GapCountOptimizer, CalculateFromHops_TwoNodes) {
    // 2 nodes = 1 hop
    EXPECT_EQ(GapCountOptimizer::CalculateFromHops(1), 5);
}

TEST(GapCountOptimizer, CalculateFromHops_ThreeNodes_RealWorld) {
    // Real-world scenario from FireBug logs:
    // 3 nodes (Mac + FireBug + another device)
    // Root node ID = 2 → max hops = 2
    EXPECT_EQ(GapCountOptimizer::CalculateFromHops(2), 7);
}

TEST(GapCountOptimizer, CalculateFromHops_FourNodes) {
    // 4 nodes = 3 hops
    EXPECT_EQ(GapCountOptimizer::CalculateFromHops(3), 8);
}

TEST(GapCountOptimizer, CalculateFromHops_FiveNodes) {
    // 5 nodes = 4 hops
    EXPECT_EQ(GapCountOptimizer::CalculateFromHops(4), 10);
}

TEST(GapCountOptimizer, CalculateFromHops_MaxTableSize) {
    // Edge of table (25 hops)
    EXPECT_EQ(GapCountOptimizer::CalculateFromHops(25), 63);
}

TEST(GapCountOptimizer, CalculateFromHops_BeyondTable) {
    // Beyond table size should clamp to 63
    EXPECT_EQ(GapCountOptimizer::CalculateFromHops(30), 63);
    EXPECT_EQ(GapCountOptimizer::CalculateFromHops(100), 63);
    EXPECT_EQ(GapCountOptimizer::CalculateFromHops(255), 63);
}

// ============================================================================
// Ping Time Calculation Tests (Apple's Formula)
// ============================================================================

TEST(GapCountOptimizer, CalculateFromPing_VeryShort) {
    // Ping < 29ns → gap=5 (minimum)
    EXPECT_EQ(GapCountOptimizer::CalculateFromPing(20), 5);
    EXPECT_EQ(GapCountOptimizer::CalculateFromPing(28), 5);
}

TEST(GapCountOptimizer, CalculateFromPing_Boundary) {
    // Ping = 29ns → first table entry
    // (29 - 20) / 9 = 1 → GAP_TABLE[1] = 5
    EXPECT_EQ(GapCountOptimizer::CalculateFromPing(29), 5);
}

TEST(GapCountOptimizer, CalculateFromPing_TwoHopRange) {
    // Ping 29-37ns should give gap for 2 hops
    // (37 - 20) / 9 = 1.88 → index 1 → gap=5
    // (38 - 20) / 9 = 2 → index 2 → gap=7
    EXPECT_EQ(GapCountOptimizer::CalculateFromPing(37), 5);
    EXPECT_EQ(GapCountOptimizer::CalculateFromPing(38), 7);
}

TEST(GapCountOptimizer, CalculateFromPing_ThreeHopRange) {
    // Ping 38-46ns should give gap for 3 hops
    // (46 - 20) / 9 = 2.88 → index 2 → gap=7
    // (47 - 20) / 9 = 3 → index 3 → gap=8
    EXPECT_EQ(GapCountOptimizer::CalculateFromPing(46), 7);
    EXPECT_EQ(GapCountOptimizer::CalculateFromPing(47), 8);
}

TEST(GapCountOptimizer, CalculateFromPing_MaxPing) {
    // Ping > 245ns should clamp to 63
    EXPECT_EQ(GapCountOptimizer::CalculateFromPing(245), 63);
    EXPECT_EQ(GapCountOptimizer::CalculateFromPing(300), 63);
    EXPECT_EQ(GapCountOptimizer::CalculateFromPing(1000), 63);
}

// ============================================================================
// Combined Calculation Tests (Hop + Ping, use maximum)
// ============================================================================

TEST(GapCountOptimizer, Calculate_HopOnlyMode) {
    // No ping time available → use hop count
    EXPECT_EQ(GapCountOptimizer::Calculate(2, std::nullopt), 7);
    EXPECT_EQ(GapCountOptimizer::Calculate(3, std::nullopt), 8);
}

TEST(GapCountOptimizer, Calculate_BothModesAgree) {
    // Hops suggest gap=7, ping suggests gap=7 → use 7
    uint8_t hops = 2;  // gap=7
    uint32_t ping = 38;  // gap=7
    EXPECT_EQ(GapCountOptimizer::Calculate(hops, ping), 7);
}

TEST(GapCountOptimizer, Calculate_PingMoreConservative) {
    // Hops suggest gap=5 (1 hop), but ping suggests gap=7 (longer propagation)
    // Should use the LARGER (safer) value
    uint8_t hops = 1;  // gap=5
    uint32_t ping = 38;  // gap=7
    EXPECT_EQ(GapCountOptimizer::Calculate(hops, ping), 7);  // Use larger
}

TEST(GapCountOptimizer, Calculate_HopMoreConservative) {
    // Hops suggest gap=8 (3 hops), but ping suggests gap=5 (short cables)
    // Should use the LARGER (safer) value
    uint8_t hops = 3;  // gap=8
    uint32_t ping = 28;  // gap=5
    EXPECT_EQ(GapCountOptimizer::Calculate(hops, ping), 8);  // Use larger
}

TEST(GapCountOptimizer, Calculate_NeverReturnsZero) {
    // Verify we NEVER return gap=0 under any circumstances
    for (uint8_t hops = 0; hops < 30; ++hops) {
        uint8_t gap = GapCountOptimizer::Calculate(hops, std::nullopt);
        EXPECT_GE(gap, 5) << "Gap count should never be < 5 for hops=" << (int)hops;
    }

    for (uint32_t ping = 0; ping < 300; ping += 10) {
        uint8_t gap = GapCountOptimizer::Calculate(10, ping);
        EXPECT_GE(gap, 5) << "Gap count should never be < 5 for ping=" << ping;
    }
}

// ============================================================================
// Gap Consistency Tests
// ============================================================================

TEST(GapCountOptimizer, AreGapsConsistent_Empty) {
    std::vector<uint8_t> gaps = {};
    EXPECT_TRUE(GapCountOptimizer::AreGapsConsistent(gaps));
}

TEST(GapCountOptimizer, AreGapsConsistent_SingleNode) {
    std::vector<uint8_t> gaps = {7};
    EXPECT_TRUE(GapCountOptimizer::AreGapsConsistent(gaps));
}

TEST(GapCountOptimizer, AreGapsConsistent_AllSame) {
    std::vector<uint8_t> gaps = {7, 7, 7};
    EXPECT_TRUE(GapCountOptimizer::AreGapsConsistent(gaps));
}

TEST(GapCountOptimizer, AreGapsConsistent_Default63_RealWorld) {
    // From FireBug logs: all nodes initially have gap=0x3f (63)
    std::vector<uint8_t> gaps = {63, 63, 63};
    EXPECT_TRUE(GapCountOptimizer::AreGapsConsistent(gaps));
}

TEST(GapCountOptimizer, AreGapsConsistent_Mismatch) {
    // Inconsistent gaps (from Apple code comment)
    std::vector<uint8_t> gaps = {7, 63, 7};
    EXPECT_FALSE(GapCountOptimizer::AreGapsConsistent(gaps));
}

TEST(GapCountOptimizer, AreGapsConsistent_TwoNodesDisagree) {
    std::vector<uint8_t> gaps = {7, 8};
    EXPECT_FALSE(GapCountOptimizer::AreGapsConsistent(gaps));
}

// ============================================================================
// Invalid Gap Detection Tests
// ============================================================================

TEST(GapCountOptimizer, HasInvalidGap_Zero) {
    // gap=0 is INVALID per IEEE 1394a
    std::vector<uint8_t> gaps = {0, 0, 0};
    EXPECT_TRUE(GapCountOptimizer::HasInvalidGap(gaps));
}

TEST(GapCountOptimizer, HasInvalidGap_ZeroAmongValid) {
    // Even one gap=0 is invalid
    std::vector<uint8_t> gaps = {7, 0, 7};
    EXPECT_TRUE(GapCountOptimizer::HasInvalidGap(gaps));
}

TEST(GapCountOptimizer, HasInvalidGap_Inconsistent) {
    // Inconsistent gaps are invalid
    std::vector<uint8_t> gaps = {7, 63, 7};
    EXPECT_TRUE(GapCountOptimizer::HasInvalidGap(gaps));
}

TEST(GapCountOptimizer, HasInvalidGap_Valid) {
    // All consistent, non-zero gaps are valid
    std::vector<uint8_t> gaps = {7, 7, 7};
    EXPECT_FALSE(GapCountOptimizer::HasInvalidGap(gaps));
}

// ============================================================================
// ShouldUpdate Tests (Decision Logic)
// ============================================================================

TEST(GapCountOptimizer, ShouldUpdate_Empty) {
    // No nodes → no update
    std::vector<uint8_t> gaps = {};
    EXPECT_FALSE(GapCountOptimizer::ShouldUpdate(gaps, 7, 0xFF));
}

TEST(GapCountOptimizer, ShouldUpdate_AlreadyOptimal) {
    // Current gap matches new gap → no update
    std::vector<uint8_t> gaps = {7, 7, 7};
    EXPECT_FALSE(GapCountOptimizer::ShouldUpdate(gaps, 7, 0xFF));
}

TEST(GapCountOptimizer, ShouldUpdate_MatchesPrevious) {
    // Current gap matches previous gap (avoid jitter)
    std::vector<uint8_t> gaps = {8, 8, 8};
    EXPECT_FALSE(GapCountOptimizer::ShouldUpdate(gaps, 7, 8));
}

TEST(GapCountOptimizer, ShouldUpdate_NeedChange) {
    // Current gap doesn't match new or previous → update
    std::vector<uint8_t> gaps = {63, 63, 63};  // Default
    EXPECT_TRUE(GapCountOptimizer::ShouldUpdate(gaps, 7, 0xFF));
}

TEST(GapCountOptimizer, ShouldUpdate_Inconsistent_RealWorld) {
    // From Apple code: inconsistent gaps MUST be updated
    std::vector<uint8_t> gaps = {7, 63, 7};
    EXPECT_TRUE(GapCountOptimizer::ShouldUpdate(gaps, 7, 7));
}

TEST(GapCountOptimizer, ShouldUpdate_Zero_Critical) {
    // gap=0 is CRITICAL ERROR → MUST update
    std::vector<uint8_t> gaps = {0, 0, 0};
    EXPECT_TRUE(GapCountOptimizer::ShouldUpdate(gaps, 7, 7));
}

TEST(GapCountOptimizer, ShouldUpdate_ZeroAmongConsistent_Critical) {
    // Even if only one node has gap=0 → MUST update
    std::vector<uint8_t> gaps = {7, 0, 7};  // Inconsistent + zero
    EXPECT_TRUE(GapCountOptimizer::ShouldUpdate(gaps, 7, 7));
}

TEST(GapCountOptimizer, ShouldUpdate_FromDefault63ToOptimal) {
    // Real-world scenario: nodes boot with gap=63, optimize to gap=7
    std::vector<uint8_t> gaps = {63, 63, 63};
    EXPECT_TRUE(GapCountOptimizer::ShouldUpdate(gaps, 7, 0xFF));
}

TEST(GapCountOptimizer, ShouldUpdate_StableAfterFirstUpdate) {
    // After first update: current=7, new=7, prev=63 → no update (stable)
    std::vector<uint8_t> gaps = {7, 7, 7};
    EXPECT_FALSE(GapCountOptimizer::ShouldUpdate(gaps, 7, 63));
}

TEST(GapCountOptimizer, ShouldUpdate_JitterPrevention) {
    // Ping time jitter might change gap 7→8→7
    // If current=7, new=8, prev=7 → should NOT update (matches prev)
    std::vector<uint8_t> gaps = {7, 7, 7};
    EXPECT_FALSE(GapCountOptimizer::ShouldUpdate(gaps, 8, 7));
}

// ============================================================================
// Integration Test: Complete Real-World Scenario
// ============================================================================

TEST(GapCountOptimizer, RealWorldScenario_ThreeNodeBus) {
    // Scenario from FireBug logs:
    // - 3 nodes: Mac (node 0), FireBug (node 1), Device (node 2)
    // - Root node ID = 2 → max hops = 2
    // - Initial gaps = [63, 63, 63] (default)
    // - Expected optimal gap = 7

    // Step 1: Calculate optimal gap
    uint8_t maxHops = 2;  // Root node ID
    uint8_t optimalGap = GapCountOptimizer::Calculate(maxHops, std::nullopt);
    EXPECT_EQ(optimalGap, 7);

    // Step 2: Check if update needed (first boot)
    std::vector<uint8_t> currentGaps = {63, 63, 63};
    EXPECT_TRUE(GapCountOptimizer::ShouldUpdate(currentGaps, optimalGap, 0xFF));

    // Step 3: After update, gaps should be consistent
    std::vector<uint8_t> updatedGaps = {7, 7, 7};
    EXPECT_FALSE(GapCountOptimizer::ShouldUpdate(updatedGaps, optimalGap, 63));

    // Step 4: Verify no further updates needed
    EXPECT_FALSE(GapCountOptimizer::ShouldUpdate(updatedGaps, optimalGap, optimalGap));
}

TEST(GapCountOptimizer, RealWorldScenario_GapZeroDetection) {
    // Scenario from kernel logs:
    // - PHY packet 0x00000200 was sent (gap=0, T=1, R=0)
    // - This created invalid state: Self-ID shows gap=0
    // - Must detect and force update

    // Simulate gap=0 in Self-IDs
    std::vector<uint8_t> brokenGaps = {0, 7, 0};  // Node 2 has gap=0 from bad PHY packet

    // Should detect as invalid
    EXPECT_TRUE(GapCountOptimizer::HasInvalidGap(brokenGaps));

    // Should force update
    EXPECT_TRUE(GapCountOptimizer::ShouldUpdate(brokenGaps, 7, 7));
}

TEST(GapCountOptimizer, RealWorldScenario_NoInfiniteLoop) {
    // Ensure that after max attempts, the logic would stop
    // (This test just verifies the gap calculation itself doesn't cause loops)

    std::vector<uint8_t> gaps = {7, 7, 7};
    uint8_t newGap = 7;
    uint8_t prevGap = 7;

    // Should NOT update if already optimal
    EXPECT_FALSE(GapCountOptimizer::ShouldUpdate(gaps, newGap, prevGap));

    // Even if called repeatedly, should still return false
    for (int i = 0; i < 100; ++i) {
        EXPECT_FALSE(GapCountOptimizer::ShouldUpdate(gaps, newGap, prevGap));
    }
}
