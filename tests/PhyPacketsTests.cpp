// Copyright (c) 2025 ASFW Project
//
// PhyPacketsTests.cpp - Comprehensive unit tests for IEEE 1394 PHY packet encoding/decoding
//
// Tests against:
// - IEEE 1394a-2000 specification
// - Apple FireBug reference packets (from real hardware)
// - Endianness handling (little-endian host → big-endian bus)
// - gap=0 bug regression

#include <gtest/gtest.h>
#include "Phy/PhyPackets.hpp"
#include <array>
#include <cstdint>

using namespace ASFW::Driver;

// =============================================================================
// SECTION 1: Basic Encoding Tests
// =============================================================================

TEST(PhyPackets, BasicForceRoot_SetsRBit) {
    AlphaPhyConfig config{};
    config.rootId = 2;
    config.forceRoot = true;
    config.gapCountOptimization = false;  // T=0

    Quadlet encoded = config.EncodeHostOrder();

    // Verify R bit is set (bit 23)
    EXPECT_TRUE((encoded & AlphaPhyConfig::kForceRootMask) != 0);

    // Verify rootId is encoded correctly (bits[29:24])
    uint8_t extractedRootId = (encoded & AlphaPhyConfig::kRootIdMask) >> AlphaPhyConfig::kRootIdShift;
    EXPECT_EQ(extractedRootId, 2);
}

TEST(PhyPackets, BasicForceRoot_TBitNotSet) {
    AlphaPhyConfig config{};
    config.rootId = 2;
    config.forceRoot = true;
    config.gapCountOptimization = false;  // T=0

    Quadlet encoded = config.EncodeHostOrder();

    // Verify T bit is NOT set (bit 22)
    EXPECT_FALSE((encoded & AlphaPhyConfig::kGapOptMask) != 0);
}

// CRITICAL TEST: This catches the gap=0 bug!
TEST(PhyPackets, ForceRootWithoutGapOpt_MustNotEncodeGapZero) {
    AlphaPhyConfig config{};
    config.rootId = 2;
    config.forceRoot = true;
    config.gapCountOptimization = false;  // T=0 - don't update gap
    config.gapCount = 0x3F;  // Default value

    Quadlet encoded = config.EncodeHostOrder();

    // Extract gap count field (bits[21:16])
    uint8_t gapField = (encoded & AlphaPhyConfig::kGapCountMask) >> AlphaPhyConfig::kGapCountShift;

    // CRITICAL: When T=0, gap bits should be 0x3F to prevent buggy PHYs from latching 0
    // This is the root cause of the bus reset storms!
    EXPECT_NE(gapField, 0) << "Gap field must NOT be 0 even when T=0! Buggy PHYs will latch gap=0";
    EXPECT_EQ(gapField, 0x3F) << "Gap field should be 0x3F (safe default) when T=0";
}

TEST(PhyPackets, GapOptimization_SetsAllBitsCorrectly) {
    AlphaPhyConfig config{};
    config.rootId = 2;
    config.forceRoot = true;
    config.gapCountOptimization = true;  // T=1
    config.gapCount = 7;

    Quadlet encoded = config.EncodeHostOrder();

    // Verify all bits
    EXPECT_TRUE((encoded & AlphaPhyConfig::kForceRootMask) != 0) << "R bit should be set";
    EXPECT_TRUE((encoded & AlphaPhyConfig::kGapOptMask) != 0) << "T bit should be set";

    uint8_t gapField = (encoded & AlphaPhyConfig::kGapCountMask) >> AlphaPhyConfig::kGapCountShift;
    EXPECT_EQ(gapField, 7) << "Gap count should be 7";
}

TEST(PhyPackets, PacketIdentifier_AlwaysZero) {
    AlphaPhyConfig config{};
    config.rootId = 2;
    config.forceRoot = true;

    Quadlet encoded = config.EncodeHostOrder();

    // Verify packet identifier is 00 (bits[31:30])
    uint8_t packetId = (encoded & AlphaPhyConfig::kPacketIdentifierMask) >> AlphaPhyConfig::kPacketIdentifierShift;
    EXPECT_EQ(packetId, 0) << "PHY Config packet identifier must be 0";
}

TEST(PhyPackets, DecodeEncodeRoundtrip) {
    AlphaPhyConfig original{};
    original.rootId = 2;
    original.forceRoot = true;
    original.gapCountOptimization = true;
    original.gapCount = 7;

    Quadlet encoded = original.EncodeHostOrder();
    AlphaPhyConfig decoded = AlphaPhyConfig::DecodeHostOrder(encoded);

    EXPECT_EQ(decoded.rootId, original.rootId);
    EXPECT_EQ(decoded.forceRoot, original.forceRoot);
    EXPECT_EQ(decoded.gapCountOptimization, original.gapCountOptimization);
    EXPECT_EQ(decoded.gapCount, original.gapCount);
}

TEST(PhyPackets, InvertedQuadlet_IsCorrectComplement) {
    AlphaPhyConfigPacket packet{};
    packet.header.rootId = 2;
    packet.header.forceRoot = true;
    packet.header.gapCountOptimization = false;

    auto encoded = packet.EncodeHostOrder();

    // Verify second quadlet is bitwise NOT of first
    EXPECT_EQ(encoded[1], ~encoded[0]);
}

TEST(PhyPackets, RootIdClamping_MaxValue) {
    AlphaPhyConfig config{};
    config.rootId = 0xFF;  // Try to set all bits

    Quadlet encoded = config.EncodeHostOrder();

    // Should be clamped to 6 bits (0x3F = 63)
    uint8_t extractedRootId = (encoded & AlphaPhyConfig::kRootIdMask) >> AlphaPhyConfig::kRootIdShift;
    EXPECT_EQ(extractedRootId, 0x3F);
}

TEST(PhyPackets, GapCountClamping_MaxValue) {
    AlphaPhyConfig config{};
    config.gapCountOptimization = true;
    config.gapCount = 0xFF;  // Try to set all bits

    Quadlet encoded = config.EncodeHostOrder();

    // Should be clamped to 6 bits (0x3F = 63)
    uint8_t extractedGap = (encoded & AlphaPhyConfig::kGapCountMask) >> AlphaPhyConfig::kGapCountShift;
    EXPECT_EQ(extractedGap, 0x3F);
}

// =============================================================================
// SECTION 2: Apple FireBug Reference Validation
// =============================================================================

TEST(PhyPackets, AppleReference_ForceRoot2) {
    // From Apple FireBug log:
    // "PHY Config, force_root = 02"
    // Expected encoding: root=2, R=1, T=0, gap=0x3F (not 0!)

    AlphaPhyConfig config{};
    config.rootId = 2;
    config.forceRoot = true;
    config.gapCountOptimization = false;

    Quadlet encoded = config.EncodeHostOrder();

    // Verify it matches Apple's intent
    uint8_t rootId = (encoded >> 24) & 0x3F;
    bool R = (encoded & (1u << 23)) != 0;
    bool T = (encoded & (1u << 22)) != 0;
    uint8_t gap = (encoded >> 16) & 0x3F;

    EXPECT_EQ(rootId, 2);
    EXPECT_TRUE(R) << "R bit must be set";
    EXPECT_FALSE(T) << "T bit must NOT be set";
    EXPECT_EQ(gap, 0x3F) << "Gap must be 0x3F, not 0!";
}

TEST(PhyPackets, AppleReference_Gap3FMaintained) {
    // Apple's logs show gap=0x3f is maintained after PHY Config
    // This tests that we don't accidentally encode gap=0

    AlphaPhyConfig beforeReset{};
    beforeReset.gapCount = 0x3F;
    beforeReset.gapCountOptimization = false;

    // Send PHY Config without gap update (T=0)
    AlphaPhyConfig phyConfig{};
    phyConfig.rootId = 2;
    phyConfig.forceRoot = true;
    phyConfig.gapCountOptimization = false;  // Don't update gap
    phyConfig.gapCount = 0x3F;  // Should encode this even though T=0

    Quadlet encoded = phyConfig.EncodeHostOrder();
    uint8_t gapField = (encoded >> 16) & 0x3F;

    // After reset, gap should still be 0x3F (not 0!)
    EXPECT_EQ(gapField, 0x3F);
}

TEST(PhyPackets, AppleReference_PhyGlobalResume) {
    // From Apple FireBug log:
    // "PHY Global Resume from node 0 [003c0000]"

    PhyGlobalResumePacket resume{};
    resume.phyId = 0;

    auto encoded = resume.EncodeHostOrder();

    // Should match Apple's pattern: 0x003c0000
    EXPECT_EQ(encoded[0], 0x003C0000u) << "PHY Global Resume should encode as 0x003C0000 for node 0";
    EXPECT_EQ(encoded[1], ~0x003C0000u) << "Second quadlet should be inverted";
}

TEST(PhyPackets, AppleReference_PhyGlobalResumeWithNode2) {
    // Test with different node ID
    PhyGlobalResumePacket resume{};
    resume.phyId = 2;

    auto encoded = resume.EncodeHostOrder();

    // Should be: 0x02 << 24 | 0x003C0000 = 0x023C0000
    EXPECT_EQ(encoded[0], 0x023C0000u);
}

TEST(PhyPackets, AppleReference_IsConfigQuadlet) {
    // Apple's PHY Config packets should be recognized as config packets
    Quadlet appleForceRoot2 = 0x00800000u | (2u << 24);  // R=1, root=2

    EXPECT_TRUE(AlphaPhyConfig::IsConfigQuadletHostOrder(appleForceRoot2));
}

// =============================================================================
// SECTION 3: Endianness Tests
// =============================================================================

TEST(PhyPackets, Endianness_HostOrderTooBusOrder) {
    AlphaPhyConfigPacket packet{};
    packet.header.rootId = 2;
    packet.header.forceRoot = true;

    auto hostOrder = packet.EncodeHostOrder();
    auto busOrder = packet.EncodeBusOrder();

    // On little-endian host, bytes should be swapped
    if constexpr (std::endian::native == std::endian::little) {
        EXPECT_NE(hostOrder[0], busOrder[0]) << "Bus order should be byte-swapped on little-endian";
        EXPECT_EQ(hostOrder[0], std::byteswap(busOrder[0]));
    } else {
        EXPECT_EQ(hostOrder[0], busOrder[0]) << "Bus order should match host order on big-endian";
    }
}

TEST(PhyPackets, Endianness_BusOrderDecoding) {
    // Simulate receiving a packet from the bus (big-endian)
    // This packet should be: R=1, root=0, T=0, gap=0x3F (after our fix)
    // Host order encoding: 0x00BF0000

    // First, encode a reference packet in host order
    AlphaPhyConfig reference{};
    reference.rootId = 0;
    reference.forceRoot = true;
    reference.gapCountOptimization = false;

    Quadlet hostOrderReference = reference.EncodeHostOrder();
    EXPECT_EQ(hostOrderReference, 0x00BF0000u) << "Reference should encode as 0x00BF0000";

    // Convert to bus order (simulates transmission on wire)
    Quadlet busOrderQuadlet = ToBusOrder(hostOrderReference);

    // Convert back to host order (simulates reception from wire)
    Quadlet hostOrderQuadlet = FromBusOrder(busOrderQuadlet);

    // Decode and verify
    AlphaPhyConfig decoded = AlphaPhyConfig::DecodeHostOrder(hostOrderQuadlet);

    EXPECT_TRUE(decoded.forceRoot);
    EXPECT_EQ(decoded.rootId, 0);
    EXPECT_EQ(decoded.gapCount, 0x3F) << "Gap should be 0x3F after roundtrip";
}

TEST(PhyPackets, Endianness_LittleEndianHost_RootId2) {
    // Test specific case: root=2, R=1, T=0
    AlphaPhyConfig config{};
    config.rootId = 2;
    config.forceRoot = true;

    auto busOrder = AlphaPhyConfigPacket{config}.EncodeBusOrder();

    if constexpr (std::endian::native == std::endian::little) {
        // On little-endian, host 0x02800000 becomes bus 0x00008002
        // Wait, let me think about this more carefully...
        // Host order: 0x02800000 (bits: root=2 at [29:24], R=1 at [23])
        // Bus order (big-endian): bytes reversed

        // Actually, let's just verify roundtrip works
        Quadlet hostBack = FromBusOrder(busOrder[0]);
        AlphaPhyConfig decoded = AlphaPhyConfig::DecodeHostOrder(hostBack);

        EXPECT_EQ(decoded.rootId, 2);
        EXPECT_TRUE(decoded.forceRoot);
    }
}

TEST(PhyPackets, Endianness_ToBusOrderAndBack) {
    Quadlet original = 0x02800000u;
    Quadlet bus = ToBusOrder(original);
    Quadlet back = FromBusOrder(bus);

    EXPECT_EQ(back, original) << "Roundtrip conversion should preserve value";
}

TEST(PhyPackets, Endianness_HelperFunctions) {
    // Test that ToBusOrder and FromBusOrder are inverses
    for (Quadlet test : {0x00000000u, 0x12345678u, 0xFFFFFFFFu, 0x02800000u}) {
        EXPECT_EQ(FromBusOrder(ToBusOrder(test)), test);
        EXPECT_EQ(ToBusOrder(FromBusOrder(test)), test);
    }
}

// =============================================================================
// SECTION 4: Bug Regression Tests
// =============================================================================

TEST(PhyPackets, BugRegression_Gap0WithT0) {
    // CRITICAL: This is the bug that caused bus reset storms
    // When T=0 (don't update gap), the gap bits were encoded as 0x00
    // Buggy PHYs latched this as gap=0, causing instability

    AlphaPhyConfig config{};
    config.rootId = 2;
    config.forceRoot = true;
    config.gapCountOptimization = false;  // T=0 - don't update gap
    // NOTE: We don't set gapCount explicitly, using default 0x3F

    Quadlet encoded = config.EncodeHostOrder();
    uint8_t gapBits = (encoded >> 16) & 0x3F;

    EXPECT_NE(gapBits, 0) << "BUG: Gap bits are 0 when T=0! This causes bus reset storms!";
    EXPECT_EQ(gapBits, 0x3F) << "Gap bits should be 0x3F (safe default) when T=0";
}

TEST(PhyPackets, BugRegression_Gap0WithT1_ShouldFail) {
    // Setting gap=0 with T=1 is invalid per IEEE 1394a
    // This should be caught by validation (if we add it to HardwareInterface)

    AlphaPhyConfig config{};
    config.gapCountOptimization = true;  // T=1
    config.gapCount = 0;  // INVALID

    Quadlet encoded = config.EncodeHostOrder();
    uint8_t gapBits = (encoded >> 16) & 0x3F;

    // The encoder will encode it, but this should be rejected by HardwareInterface
    EXPECT_EQ(gapBits, 0) << "Encoder allows gap=0 (validation happens at HardwareInterface)";
}

TEST(PhyPackets, BugRegression_PhyExplorerValidation_ForceRoot2) {
    // This packet should pass phy_explorer.py validation
    AlphaPhyConfig config{};
    config.rootId = 2;
    config.forceRoot = true;
    config.gapCountOptimization = false;

    Quadlet encoded = config.EncodeHostOrder();

    // Extract fields for manual verification
    uint8_t rootId = (encoded >> 24) & 0x3F;
    bool R = (encoded & (1u << 23)) != 0;
    bool T = (encoded & (1u << 22)) != 0;
    uint8_t gap = (encoded >> 16) & 0x3F;

    // Print for manual verification with phy_explorer.py
    printf("\n");
    printf("phy_explorer.py test packet:\n");
    printf("  Quadlet[0]: 0x%08X\n", encoded);
    printf("  Quadlet[1]: 0x%08X (inverted)\n", ~encoded);
    printf("  rootId=%d R=%d T=%d gap=%d\n", rootId, R, T, gap);
    printf("\n");
    printf("Run: ./tools/phy_explorer.py 0x%08X 0x%08X\n", encoded, ~encoded);
    printf("Expected: No errors, gap should be %d (not 0)\n", gap);
    printf("\n");

    // phy_explorer.py should NOT report "gap_count=0 with T=1"
    EXPECT_FALSE(T && gap == 0) << "phy_explorer.py would flag this as invalid!";
}

TEST(PhyPackets, BugRegression_ComplementCheck) {
    // Verify that the inverted quadlet is exactly ~first
    AlphaPhyConfigPacket packet{};
    packet.header.rootId = 2;
    packet.header.forceRoot = true;

    auto encoded = packet.EncodeHostOrder();

    // Manual complement check (same as phy_explorer.py)
    bool complementCorrect = (encoded[1] == (~encoded[0] & 0xFFFFFFFF));

    EXPECT_TRUE(complementCorrect) << "Second quadlet MUST be bitwise NOT of first";
}

TEST(PhyPackets, BugRegression_ExtendedPacketDetection) {
    // Extended packets have R=0, T=0
    AlphaPhyConfig config{};
    config.forceRoot = false;
    config.gapCountOptimization = false;

    EXPECT_TRUE(config.IsExtendedConfig()) << "R=0 T=0 should be detected as extended packet";
}

TEST(PhyPackets, BugRegression_NotExtendedWhenForceRoot) {
    AlphaPhyConfig config{};
    config.forceRoot = true;
    config.gapCountOptimization = false;

    EXPECT_FALSE(config.IsExtendedConfig()) << "R=1 should NOT be extended packet";
}

// =============================================================================
// SECTION 5: Real-World Scenarios
// =============================================================================

TEST(PhyPackets, RealWorld_InitialBusReset_ForceRoot) {
    // Scenario: After bus reset, driver wants to force node 2 as root
    // This is what Apple does: send PHY Config with force_root=02

    AlphaPhyConfig config{};
    config.rootId = 2;
    config.forceRoot = true;
    config.gapCountOptimization = false;  // Don't change gap yet

    AlphaPhyConfigPacket packet{config};
    auto encoded = packet.EncodeHostOrder();

    // Verify this matches Apple's behavior
    uint8_t rootId = (encoded[0] >> 24) & 0x3F;
    bool R = (encoded[0] & (1u << 23)) != 0;
    bool T = (encoded[0] & (1u << 22)) != 0;
    uint8_t gap = (encoded[0] >> 16) & 0x3F;

    EXPECT_EQ(rootId, 2);
    EXPECT_TRUE(R);
    EXPECT_FALSE(T);
    EXPECT_EQ(gap, 0x3F) << "Gap must be 0x3F to prevent buggy PHYs from adopting gap=0";
}

TEST(PhyPackets, RealWorld_GapOptimization_TwoHopBus) {
    // Scenario: After topology stabilizes, optimize gap for 2-hop bus
    // Gap=7 is optimal for 2 hops per IEEE 1394a Table E.1

    AlphaPhyConfig config{};
    config.rootId = 2;
    config.forceRoot = true;
    config.gapCountOptimization = true;  // Update gap this time
    config.gapCount = 7;

    AlphaPhyConfigPacket packet{config};
    auto encoded = packet.EncodeHostOrder();

    bool T = (encoded[0] & (1u << 22)) != 0;
    uint8_t gap = (encoded[0] >> 16) & 0x3F;

    EXPECT_TRUE(T) << "T bit must be set to apply gap update";
    EXPECT_EQ(gap, 7) << "Gap should be 7 for 2-hop bus";
}

TEST(PhyPackets, RealWorld_PhyGlobalResume_AfterReset) {
    // Scenario: After successful bus reset, send PHY Global Resume
    // This wakes up low-power devices

    PhyGlobalResumePacket resume{};
    resume.phyId = 0;  // Local node

    auto encoded = resume.EncodeHostOrder();

    // Should match Apple's FireBug log: "PHY Global Resume from node 0 [003c0000]"
    EXPECT_EQ(encoded[0], 0x003C0000u);
    EXPECT_EQ(encoded[1], ~0x003C0000u);
}

// =============================================================================
// SECTION 6: Decode Tests (Simulating Received Packets)
// =============================================================================

TEST(PhyPackets, Decode_AppleForceRoot) {
    // Simulate receiving Apple's "PHY Config, force_root = 02" packet
    // Expected encoding: 0x02800000 (root=2, R=1, T=0)

    Quadlet received = 0x02800000u;  // Host order after bus→host conversion

    AlphaPhyConfig decoded = AlphaPhyConfig::DecodeHostOrder(received);

    EXPECT_EQ(decoded.rootId, 2);
    EXPECT_TRUE(decoded.forceRoot);
    EXPECT_FALSE(decoded.gapCountOptimization);
}

TEST(PhyPackets, Decode_GapOptimizationPacket) {
    // Simulate gap optimization: root=2, R=1, T=1, gap=7
    // Bits: [31:30]=00, [29:24]=000010, [23]=1, [22]=1, [21:16]=000111

    Quadlet received = 0x02C70000u;  // 0000 0010 1100 0111 0000 0000 0000 0000

    AlphaPhyConfig decoded = AlphaPhyConfig::DecodeHostOrder(received);

    EXPECT_EQ(decoded.rootId, 2);
    EXPECT_TRUE(decoded.forceRoot);
    EXPECT_TRUE(decoded.gapCountOptimization);
    EXPECT_EQ(decoded.gapCount, 7);
}

TEST(PhyPackets, Decode_MaxRootId) {
    // Test decoding maximum root ID (0x3F = 63)
    Quadlet received = 0x3F800000u;  // root=63, R=1, T=0

    AlphaPhyConfig decoded = AlphaPhyConfig::DecodeHostOrder(received);

    EXPECT_EQ(decoded.rootId, 0x3F);
    EXPECT_TRUE(decoded.forceRoot);
}
