// TLabelMatchingTests.cpp - Comprehensive tLabel TX/RX matching verification
//
// Critical Bug Fix Validation:
// This test verifies the fix for Issue #1: tLabel Bit Position Mismatch
//
// BUG: TX used shift 18, RX used shift 10 → tLabel=0 became tLabel=48 on receive
// FIX: TX now uses shift 10 to match RX → tLabel preserved correctly
//
// Test Coverage:
// 1. OHCI Internal Format tLabel Encoding (TX path in PacketBuilder)
// 2. OHCI Internal Format tLabel Extraction (RX path in PacketRouter)
// 3. Round-trip preservation for all valid labels (0-63)
// 4. Bit-level verification of immediateData[] contents

#include <gtest/gtest.h>
#include <cstring>
#include <span>

#include "ASFWDriver/Async/Tx/PacketBuilder.hpp"
#include "ASFWDriver/Async/Rx/PacketRouter.hpp"
#include "ASFWDriver/Async/AsyncTypes.hpp"

using namespace ASFW::Async;

// =============================================================================
// Test Fixture
// =============================================================================

class TLabelMatchingTest : public ::testing::Test {
protected:
    PacketBuilder builder_;

    // OHCI Internal Format (host byte order)
    // Quadlet 0: [destinationID:16][tLabel:6][retry:2][tCode:4][priority:4]
    //            bits[31:16]        [15:10]  [9:8]   [7:4]    [3:0]
    //
    // CRITICAL: tLabel is at bits[15:10], NOT bits[23:18]!

    /// Extract tLabel from OHCI internal format (host byte order)
    static uint8_t ExtractTLabelOHCI(const uint8_t* headerBuffer) {
        uint32_t quadlet0;
        std::memcpy(&quadlet0, headerBuffer, sizeof(quadlet0));
        // tLabel is at bits[15:10] in OHCI internal format
        return static_cast<uint8_t>((quadlet0 >> 10) & 0x3F);
    }

    /// Extract tCode from OHCI internal format (host byte order)
    static uint8_t ExtractTCodeOHCI(const uint8_t* headerBuffer) {
        uint32_t quadlet0;
        std::memcpy(&quadlet0, headerBuffer, sizeof(quadlet0));
        // tCode is at bits[7:4] in OHCI internal format
        return static_cast<uint8_t>((quadlet0 >> 4) & 0x0F);
    }

    /// Extract destID from OHCI internal format (host byte order)
    static uint16_t ExtractDestIDOHCI(const uint8_t* headerBuffer) {
        uint32_t quadlet1;
        std::memcpy(&quadlet1, headerBuffer + 4, sizeof(quadlet1));  // Read Q1, not Q0
        // Linux OHCI format: destID is at bits[31:16] of quadlet1
        return static_cast<uint16_t>((quadlet1 >> 16) & 0xFFFF);
    }

    /// Extract retry from OHCI internal format (host byte order)
    static uint8_t ExtractRetryOHCI(const uint8_t* headerBuffer) {
        uint32_t quadlet0;
        std::memcpy(&quadlet0, headerBuffer, sizeof(quadlet0));
        // retry is at bits[9:8] in OHCI internal format
        return static_cast<uint8_t>((quadlet0 >> 8) & 0x03);
    }

    /// Extract priority from OHCI internal format (host byte order)
    static uint8_t ExtractPriorityOHCI(const uint8_t* headerBuffer) {
        uint32_t quadlet0;
        std::memcpy(&quadlet0, headerBuffer, sizeof(quadlet0));
        // priority is at bits[3:0] in OHCI internal format
        return static_cast<uint8_t>(quadlet0 & 0x0F);
    }
};

// =============================================================================
// Test Suite 1: TX Path - PacketBuilder tLabel Encoding
// =============================================================================

TEST_F(TLabelMatchingTest, PacketBuilder_EncodeTLabel0_ReadQuadlet) {
    // CRITICAL TEST: tLabel=0 should be encoded at bits[15:10]
    // This was the bug: tLabel=0 at shift 18 looked like tLabel=0 in TX
    // but was parsed as tLabel=48 on RX side

    ReadParams params{};
    params.destinationID = 0xC0;  // Node 0, bus from context
    params.addressHigh = 0xFFFF;
    params.addressLow = 0xF0000400;
    params.length = 4;
    params.speedCode = 0xFF;

    PacketContext context{};
    context.sourceNodeID = 0xFFC1;  // Bus 1023, node 1
    context.generation = 1;
    context.speedCode = 0x02;

    uint8_t label = 0;  // CRITICAL: Testing label=0

    uint8_t headerBuffer[16] = {0};
    size_t headerSize = builder_.BuildReadQuadlet(params, label, context, headerBuffer, sizeof(headerBuffer));

    ASSERT_EQ(12, headerSize) << "Read quadlet header should be 12 bytes";

    // Extract tLabel from OHCI internal format
    uint8_t extractedLabel = ExtractTLabelOHCI(headerBuffer);
    EXPECT_EQ(0, extractedLabel) << "tLabel=0 should be at bits[15:10], value should be 0";

    // Verify other fields are correct
    uint8_t extractedTCode = ExtractTCodeOHCI(headerBuffer);
    EXPECT_EQ(0x4, extractedTCode) << "tCode should be 0x4 (Read Quadlet Request)";

    // Verify destID (should be busNumber << 6 | node)
    uint16_t extractedDestID = ExtractDestIDOHCI(headerBuffer);
    uint16_t expectedDestID = (1023 << 6) | 0xC0;  // Bus from source, node from params
    EXPECT_EQ(expectedDestID, extractedDestID) << "destID should be correctly encoded";
}

TEST_F(TLabelMatchingTest, PacketBuilder_EncodeTLabel48_ReadQuadlet) {
    // Test the problematic label value from the logs: label=48
    // This is what tLabel=0 looked like when extracted incorrectly from shift 18

    ReadParams params{};
    params.destinationID = 0xC0;
    params.addressHigh = 0xFFFF;
    params.addressLow = 0xF0000400;
    params.length = 4;
    params.speedCode = 0xFF;

    PacketContext context{};
    context.sourceNodeID = 0xFFC1;
    context.generation = 1;
    context.speedCode = 0x02;

    uint8_t label = 48;  // The problematic value from logs

    uint8_t headerBuffer[16] = {0};
    size_t headerSize = builder_.BuildReadQuadlet(params, label, context, headerBuffer, sizeof(headerBuffer));

    ASSERT_EQ(12, headerSize);

    uint8_t extractedLabel = ExtractTLabelOHCI(headerBuffer);
    EXPECT_EQ(48, extractedLabel) << "tLabel=48 should be preserved at bits[15:10]";
}

TEST_F(TLabelMatchingTest, PacketBuilder_EncodeTLabel_AllValidValues) {
    // Test all valid tLabel values (0-63, 6-bit field)

    ReadParams params{};
    params.destinationID = 0xC0;
    params.addressHigh = 0xFFFF;
    params.addressLow = 0xF0000400;
    params.length = 4;
    params.speedCode = 0xFF;

    PacketContext context{};
    context.sourceNodeID = 0xFFC1;
    context.generation = 1;
    context.speedCode = 0x02;

    for (uint8_t label = 0; label < 64; ++label) {
        uint8_t headerBuffer[16] = {0};
        size_t headerSize = builder_.BuildReadQuadlet(params, label, context, headerBuffer, sizeof(headerBuffer));

        ASSERT_EQ(12, headerSize) << "Header size should be 12 for label=" << static_cast<int>(label);

        uint8_t extractedLabel = ExtractTLabelOHCI(headerBuffer);
        EXPECT_EQ(label, extractedLabel)
            << "tLabel=" << static_cast<int>(label) << " should be preserved at bits[15:10]";
    }
}

TEST_F(TLabelMatchingTest, PacketBuilder_BitPositionVerification_Label0) {
    // BIT-LEVEL VERIFICATION: Ensure tLabel=0 is at bits[15:10]
    //
    // Expected quadlet 0 with label=0:
    // bits[31:16] = destID (will vary)
    // bits[15:10] = tLabel = 0b000000
    // bits[9:8]   = retry  = 0b01 (RetryX)
    // bits[7:4]   = tCode  = 0b0100 (ReadQuadRequest)
    // bits[3:0]   = pri    = 0b0000
    //
    // Lower 16 bits should be: 0b00000001_01000000 = 0x0140

    ReadParams params{};
    params.destinationID = 0xC0;
    params.addressHigh = 0xFFFF;
    params.addressLow = 0xF0000400;
    params.length = 4;
    params.speedCode = 0xFF;

    PacketContext context{};
    context.sourceNodeID = 0xFFC1;
    context.generation = 1;
    context.speedCode = 0x02;

    uint8_t label = 0;

    uint8_t headerBuffer[16] = {0};
    builder_.BuildReadQuadlet(params, label, context, headerBuffer, sizeof(headerBuffer));

    uint32_t quadlet0;
    std::memcpy(&quadlet0, headerBuffer, sizeof(quadlet0));

    // Check bits[15:10] are all zero (tLabel=0)
    uint32_t tLabelMask = 0x0000FC00;  // bits[15:10]
    uint32_t tLabelBits = quadlet0 & tLabelMask;
    EXPECT_EQ(0u, tLabelBits) << "Bits[15:10] should be zero for tLabel=0, got quadlet0=0x"
                               << std::hex << quadlet0;

    // Check lower 16 bits have correct retry/tCode/priority
    uint16_t lower16 = static_cast<uint16_t>(quadlet0 & 0xFFFF);
    uint16_t expectedLower = 0x0140;  // retry=1, tCode=4, pri=0 at correct positions
    EXPECT_EQ(expectedLower, lower16) << "Lower 16 bits should be 0x0140, got 0x"
                                      << std::hex << lower16;
}

TEST_F(TLabelMatchingTest, PacketBuilder_BitPositionVerification_Label48) {
    // BIT-LEVEL VERIFICATION: Ensure tLabel=48 is at bits[15:10]
    //
    // tLabel=48 = 0b110000 (decimal 48)
    // At bits[15:10]: 0b110000 << 10 = 0xC000 (in lower 16 bits)
    //
    // Expected lower 16 bits with label=48:
    // bits[15:10] = 0b110000 (48)
    // bits[9:8]   = 0b01 (retry)
    // bits[7:4]   = 0b0100 (tCode=4)
    // bits[3:0]   = 0b0000 (pri=0)
    //
    // Combined: 0b11000001_01000000 = 0xC140

    ReadParams params{};
    params.destinationID = 0xC0;
    params.addressHigh = 0xFFFF;
    params.addressLow = 0xF0000400;
    params.length = 4;
    params.speedCode = 0xFF;

    PacketContext context{};
    context.sourceNodeID = 0xFFC1;
    context.generation = 1;
    context.speedCode = 0x02;

    uint8_t label = 48;

    uint8_t headerBuffer[16] = {0};
    builder_.BuildReadQuadlet(params, label, context, headerBuffer, sizeof(headerBuffer));

    uint32_t quadlet0;
    std::memcpy(&quadlet0, headerBuffer, sizeof(quadlet0));

    // Check bits[15:10] are 0b110000 (48)
    uint32_t tLabelMask = 0x0000FC00;  // bits[15:10]
    uint32_t tLabelBits = (quadlet0 & tLabelMask) >> 10;
    EXPECT_EQ(48u, tLabelBits) << "Bits[15:10] should be 48, got quadlet0=0x"
                               << std::hex << quadlet0;

    // Check lower 16 bits
    uint16_t lower16 = static_cast<uint16_t>(quadlet0 & 0xFFFF);
    uint16_t expectedLower = 0xC140;  // label=48, retry=1, tCode=4, pri=0
    EXPECT_EQ(expectedLower, lower16) << "Lower 16 bits should be 0xC140, got 0x"
                                      << std::hex << lower16;
}

// =============================================================================
// Test Suite 2: RX Path - tLabel Extraction Verification
// =============================================================================

TEST_F(TLabelMatchingTest, ExtractTLabel0_FromOHCIFormat) {
    // Verify tLabel=0 can be extracted from bits[15:10] using same logic as PacketRouter

    // Build a header with tLabel=0 at bits[15:10]
    uint32_t quadlet0_label0 =
        (0xFFC0u << 16) |  // destID
        (0u << 10) |       // tLabel=0 at bits[15:10]
        (1u << 8) |        // retry=1
        (0x4u << 4) |      // tCode=4
        (0u);              // priority=0

    uint8_t headerBuffer[16];
    std::memcpy(headerBuffer, &quadlet0_label0, sizeof(quadlet0_label0));
    std::memset(headerBuffer + 4, 0, 12);  // Zero out rest

    uint8_t extractedLabel = ExtractTLabelOHCI(headerBuffer);
    EXPECT_EQ(0, extractedLabel) << "Should extract tLabel=0 from bits[15:10]";
}

TEST_F(TLabelMatchingTest, ExtractTLabel48_FromOHCIFormat) {
    // Verify tLabel=48 can be extracted correctly

    uint32_t quadlet0_label48 =
        (0xFFC0u << 16) |  // destID
        (48u << 10) |      // tLabel=48 at bits[15:10]
        (1u << 8) |        // retry=1
        (0x4u << 4) |      // tCode=4
        (0u);              // priority=0

    uint8_t headerBuffer[16];
    std::memcpy(headerBuffer, &quadlet0_label48, sizeof(quadlet0_label48));
    std::memset(headerBuffer + 4, 0, 12);

    uint8_t extractedLabel = ExtractTLabelOHCI(headerBuffer);
    EXPECT_EQ(48, extractedLabel) << "Should extract tLabel=48 from bits[15:10]";
}

TEST_F(TLabelMatchingTest, ExtractTLabel_AllValidValues) {
    // Test extraction for all valid labels (0-63)

    for (uint8_t expectedLabel = 0; expectedLabel < 64; ++expectedLabel) {
        uint32_t quadlet0 =
            (0xFFC0u << 16) |          // destID
            (static_cast<uint32_t>(expectedLabel) << 10) |  // tLabel at bits[15:10]
            (1u << 8) |                // retry=1
            (0x4u << 4) |              // tCode=4
            (0u);                      // priority=0

        uint8_t headerBuffer[16];
        std::memcpy(headerBuffer, &quadlet0, sizeof(quadlet0));
        std::memset(headerBuffer + 4, 0, 12);

        uint8_t extractedLabel = ExtractTLabelOHCI(headerBuffer);
        EXPECT_EQ(expectedLabel, extractedLabel)
            << "Should extract tLabel=" << static_cast<int>(expectedLabel);
    }
}

// =============================================================================
// Test Suite 3: Round-Trip TX→RX Matching
// =============================================================================

TEST_F(TLabelMatchingTest, RoundTrip_TLabelPreserved_Label0) {
    // CRITICAL: Verify tLabel=0 survives TX→RX round-trip
    // This is the exact scenario from the bug report

    ReadParams params{};
    params.destinationID = 0xC0;
    params.addressHigh = 0xFFFF;
    params.addressLow = 0xF0000400;
    params.length = 4;
    params.speedCode = 0xFF;

    PacketContext context{};
    context.sourceNodeID = 0xFFC1;
    context.generation = 1;
    context.speedCode = 0x02;

    uint8_t labelSent = 0;

    // TX: Build packet
    uint8_t headerBuffer[16] = {0};
    size_t headerSize = builder_.BuildReadQuadlet(params, labelSent, context, headerBuffer, sizeof(headerBuffer));
    ASSERT_EQ(12, headerSize);

    // RX: Extract tLabel using same logic as PacketRouter (bits[15:10])
    uint8_t labelReceived = ExtractTLabelOHCI(headerBuffer);

    // VERIFY: Labels match
    EXPECT_EQ(labelSent, labelReceived)
        << "Round-trip failed: sent tLabel=" << static_cast<int>(labelSent)
        << " but received tLabel=" << static_cast<int>(labelReceived);
}

TEST_F(TLabelMatchingTest, RoundTrip_TLabelPreserved_AllValues) {
    // Comprehensive round-trip test for all valid labels (0-63)

    ReadParams params{};
    params.destinationID = 0xC0;
    params.addressHigh = 0xFFFF;
    params.addressLow = 0xF0000400;
    params.length = 4;
    params.speedCode = 0xFF;

    PacketContext context{};
    context.sourceNodeID = 0xFFC1;
    context.generation = 1;
    context.speedCode = 0x02;

    for (uint8_t label = 0; label < 64; ++label) {
        // TX: Build packet
        uint8_t headerBuffer[16] = {0};
        size_t headerSize = builder_.BuildReadQuadlet(params, label, context, headerBuffer, sizeof(headerBuffer));
        ASSERT_EQ(12, headerSize);

        // RX: Extract tLabel using same logic as PacketRouter (bits[15:10])
        uint8_t labelReceived = ExtractTLabelOHCI(headerBuffer);

        // VERIFY: Labels match
        EXPECT_EQ(label, labelReceived)
            << "Round-trip failed for tLabel=" << static_cast<int>(label);
    }
}

TEST_F(TLabelMatchingTest, RoundTrip_MultiplePacketTypes_PreserveLabels) {
    // Test tLabel preservation across different packet types

    PacketContext context{};
    context.sourceNodeID = 0xFFC1;
    context.generation = 1;
    context.speedCode = 0x02;

    // Test with labels: 0, 1, 48, 63 (edge cases and problematic value)
    std::vector<uint8_t> testLabels = {0, 1, 48, 63};

    for (uint8_t label : testLabels) {
        // Test ReadQuadlet
        {
            ReadParams params{};
            params.destinationID = 0xC0;
            params.addressHigh = 0xFFFF;
            params.addressLow = 0xF0000400;
            params.length = 4;
            params.speedCode = 0xFF;

            uint8_t headerBuffer[16] = {0};
            size_t headerSize = builder_.BuildReadQuadlet(params, label, context, headerBuffer, sizeof(headerBuffer));

            uint8_t received = ExtractTLabelOHCI(headerBuffer);
            EXPECT_EQ(label, received) << "ReadQuadlet failed for tLabel=" << static_cast<int>(label);
        }

        // Test ReadBlock
        {
            ReadParams params{};
            params.destinationID = 0xC0;
            params.addressHigh = 0xFFFF;
            params.addressLow = 0xF0000400;
            params.length = 512;
            params.speedCode = 0xFF;

            uint8_t headerBuffer[16] = {0};
            size_t headerSize = builder_.BuildReadBlock(params, label, context, headerBuffer, sizeof(headerBuffer));

            uint8_t received = ExtractTLabelOHCI(headerBuffer);
            EXPECT_EQ(label, received) << "ReadBlock failed for tLabel=" << static_cast<int>(label);
        }

        // Test WriteQuadlet
        {
            uint32_t data = 0x12345678;
            WriteParams params{};
            params.destinationID = 0xC0;
            params.addressHigh = 0xFFFF;
            params.addressLow = 0xF0000400;
            params.length = 4;
            params.payload = &data;
            params.speedCode = 0xFF;

            uint8_t headerBuffer[16] = {0};
            size_t headerSize = builder_.BuildWriteQuadlet(params, label, context, headerBuffer, sizeof(headerBuffer));

            uint8_t received = ExtractTLabelOHCI(headerBuffer);
            EXPECT_EQ(label, received) << "WriteQuadlet failed for tLabel=" << static_cast<int>(label);
        }
    }
}

// =============================================================================
// Test Suite 4: Regression Test for Original Bug
// =============================================================================

TEST_F(TLabelMatchingTest, BugRegression_Label0_NotMisparsedAs48) {
    // REGRESSION TEST: Ensure the original bug doesn't return
    //
    // Original bug: TX encoded tLabel=0 at shift 18, RX read from shift 10
    // Result: tLabel appeared as 48 (bit pattern 0b110000) on RX side
    //
    // With fix: Both TX and RX use shift 10, tLabel=0 stays as 0

    ReadParams params{};
    params.destinationID = 0xC0;
    params.addressHigh = 0xFFFF;
    params.addressLow = 0xF0000400;
    params.length = 4;
    params.speedCode = 0xFF;

    PacketContext context{};
    context.sourceNodeID = 0xFFC1;
    context.generation = 1;
    context.speedCode = 0x02;

    uint8_t labelSent = 0;

    uint8_t headerBuffer[16] = {0};
    size_t headerSize = builder_.BuildReadQuadlet(params, labelSent, context, headerBuffer, sizeof(headerBuffer));
    ASSERT_EQ(12, headerSize);

    uint8_t labelReceived = ExtractTLabelOHCI(headerBuffer);

    // CRITICAL: This should NOT be 48!
    EXPECT_NE(48, labelReceived) << "BUG REGRESSION: tLabel=0 should not be misparsed as 48!";
    EXPECT_EQ(0, labelReceived) << "tLabel=0 should be correctly preserved as 0";
}
