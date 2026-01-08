// FCPPacketParsingTests.cpp - Unit tests for FCP response packet parsing
//
// Tests verify correct extraction of destination offset from OHCI AR DMA packets
// and FCP response routing logic.
//
// Critical areas tested:
// 1. Destination offset extraction from little-endian OHCI DMA format
// 2. FCP response address detection (0xFFFFF0000D00)
// 3. Cross-validation with Linux FireWire driver implementation
// 4. Real packet data from hardware logs

#include <gtest/gtest.h>
#include <cstring>
#include <span>

#include "ASFWDriver/Async/PacketHelpers.hpp"
#include "ASFWDriver/Async/Rx/PacketRouter.hpp"

using namespace ASFW::Async;

// =============================================================================
// Test Fixture
// =============================================================================

class FCPPacketParsingTest : public ::testing::Test {
protected:
    // FCP Response CSR address (IEEE 1394 TA Document 1999027)
    static constexpr uint64_t kFCPResponseAddress = 0xFFFFF0000D00ULL;
    
    // Helper: Extract destination offset using Linux-style approach
    // (convert LE to CPU order first, then extract fields)
    static uint64_t ExtractDestOffsetLinuxStyle(const uint8_t* buffer) {
        // Linux approach: le32_to_cpu() first
        uint32_t q1_cpu = (static_cast<uint32_t>(buffer[7]) << 24) |
                          (static_cast<uint32_t>(buffer[6]) << 16) |
                          (static_cast<uint32_t>(buffer[5]) << 8) |
                          static_cast<uint32_t>(buffer[4]);
        
        uint32_t q2_cpu = (static_cast<uint32_t>(buffer[11]) << 24) |
                          (static_cast<uint32_t>(buffer[10]) << 16) |
                          (static_cast<uint32_t>(buffer[9]) << 8) |
                          static_cast<uint32_t>(buffer[8]);
        
        // Extract offset_high (12 bits) from Q1[11:0]
        // In CPU order, this is the low 12 bits after masking out rCode
        uint64_t offset_high_12bit = q1_cpu & 0x0FFF;
        
        // Sign-extend 12-bit to 16-bit (matching ASFW implementation)
        uint64_t offset_high = offset_high_12bit;
        if (offset_high_12bit & 0x800) {
            offset_high |= 0xF000;  // Sign extend
        }
        
        // Extract offset_low (32 bits) from Q2
        uint64_t offset_low = q2_cpu;
        
        return (offset_high << 32) | offset_low;
    }
};

// =============================================================================
// Real Hardware Packet Tests (from logs)
// =============================================================================

TEST_F(FCPPacketParsingTest, RealPacket_FCPResponse_SubunitInfo) {
    // Real FCP response packet from logs (timestamp 13:34:48.181617+0100)
    // This is a SUBUNIT_INFO response from an AV/C device
    //
    // Raw packet: 10 7D C0 FF  FF FF C2 FF  00 0D 00 F0  00 00 08 00
    //             Q0           Q1           Q2           Q3
    //
    // Expected destination offset: 0xFFFFF0000D00 (FCP Response address)
    
    const uint8_t realPacket[] = {
        0x10, 0x7D, 0xC0, 0xFF,  // Q0: tCode=0x1 (Block Write), destID=0xFFC0
        0xFF, 0xFF, 0xC2, 0xFF,  // Q1: srcID=0xFFC2, rCode=0xF, offset_high=0xFFFF
        0x00, 0x0D, 0x00, 0xF0,  // Q2: offset_low=0xF0000D00 (LE format!)
        0x00, 0x00, 0x08, 0x00,  // Q3: data_length=8, extended_tcode=0
    };
    
    std::span<const uint8_t> header(realPacket, 16);
    
    // Test ASFW implementation
    uint64_t offset_asfw = ExtractDestOffset(header);
    EXPECT_EQ(kFCPResponseAddress, offset_asfw) 
        << "ASFW should extract 0xFFFFF0000D00 from real FCP response packet";
    
    // Cross-validate with Linux-style extraction
    uint64_t offset_linux = ExtractDestOffsetLinuxStyle(realPacket);
    EXPECT_EQ(kFCPResponseAddress, offset_linux)
        << "Linux-style extraction should also produce 0xFFFFF0000D00";
    
    // Both methods should agree
    EXPECT_EQ(offset_asfw, offset_linux)
        << "ASFW and Linux implementations should produce identical results";
}

TEST_F(FCPPacketParsingTest, RealPacket_FCPResponse_Retry1) {
    // Second FCP response from logs (timestamp 13:34:48.266683+0100)
    // Same SUBUNIT_INFO response, different tLabel
    
    const uint8_t realPacket[] = {
        0x10, 0x79, 0xC0, 0xFF,  // Q0: tLabel different, but same structure
        0xFF, 0xFF, 0xC2, 0xFF,  // Q1: offset_high=0xFFFF
        0x00, 0x0D, 0x00, 0xF0,  // Q2: offset_low=0xF0000D00
        0x00, 0x00, 0x08, 0x00,  // Q3: data_length=8
    };
    
    std::span<const uint8_t> header(realPacket, 16);
    uint64_t offset = ExtractDestOffset(header);
    
    EXPECT_EQ(kFCPResponseAddress, offset)
        << "Second FCP response should also extract correct address";
}

TEST_F(FCPPacketParsingTest, RealPacket_FCPResponse_Retry2) {
    // Third FCP response from logs (timestamp 13:40:41.087730+0100)
    
    const uint8_t realPacket[] = {
        0x10, 0x05, 0xC0, 0xFF,  // Q0: different tLabel
        0xFF, 0xFF, 0xC2, 0xFF,  // Q1: offset_high=0xFFFF
        0x00, 0x0D, 0x00, 0xF0,  // Q2: offset_low=0xF0000D00
        0x00, 0x00, 0x08, 0x00,  // Q3: data_length=8
    };
    
    std::span<const uint8_t> header(realPacket, 16);
    uint64_t offset = ExtractDestOffset(header);
    
    EXPECT_EQ(kFCPResponseAddress, offset)
        << "Third FCP response should also extract correct address";
}

// =============================================================================
// Boundary Tests: Offset Extraction Edge Cases
// =============================================================================

TEST_F(FCPPacketParsingTest, OffsetExtraction_AllZeros) {
    // Test packet with offset = 0x0000_00000000
    const uint8_t packet[] = {
        0x10, 0x00, 0xC0, 0xFF,  // Q0
        0x00, 0x00, 0xC2, 0xFF,  // Q1: offset_high=0x0000
        0x00, 0x00, 0x00, 0x00,  // Q2: offset_low=0x00000000
        0x00, 0x00, 0x08, 0x00,  // Q3
    };
    
    std::span<const uint8_t> header(packet, 16);
    uint64_t offset = ExtractDestOffset(header);
    
    EXPECT_EQ(0x0000000000000000ULL, offset)
        << "Should correctly extract all-zero offset";
}

TEST_F(FCPPacketParsingTest, OffsetExtraction_AllOnes) {
    // Test packet with offset = 0xFFFF_FFFFFFFF
    const uint8_t packet[] = {
        0x10, 0x00, 0xC0, 0xFF,  // Q0
        0xFF, 0xFF, 0xC2, 0xFF,  // Q1: offset_high=0xFFFF
        0xFF, 0xFF, 0xFF, 0xFF,  // Q2: offset_low=0xFFFFFFFF (LE)
        0x00, 0x00, 0x08, 0x00,  // Q3
    };
    
    std::span<const uint8_t> header(packet, 16);
    uint64_t offset = ExtractDestOffset(header);
    
    EXPECT_EQ(0xFFFFFFFFFFFFULL, offset)
        << "Should correctly extract all-ones offset (48-bit max)";
}

TEST_F(FCPPacketParsingTest, OffsetExtraction_CSRRegisterSpace) {
    // Test CSR register space base address: 0xFFFF_F0000000
    const uint8_t packet[] = {
        0x10, 0x00, 0xC0, 0xFF,  // Q0
        0xFF, 0xFF, 0xC2, 0xFF,  // Q1: offset_high=0xFFFF
        0x00, 0x00, 0x00, 0xF0,  // Q2: offset_low=0xF0000000 (LE)
        0x00, 0x00, 0x08, 0x00,  // Q3
    };
    
    std::span<const uint8_t> header(packet, 16);
    uint64_t offset = ExtractDestOffset(header);
    
    EXPECT_EQ(0xFFFFF0000000ULL, offset)
        << "Should correctly extract CSR register space base";
}

TEST_F(FCPPacketParsingTest, OffsetExtraction_ConfigROMBase) {
    // Test Config ROM base address: 0xFFFF_F0000400
    const uint8_t packet[] = {
        0x10, 0x00, 0xC0, 0xFF,  // Q0
        0xFF, 0xFF, 0xC2, 0xFF,  // Q1: offset_high=0xFFFF
        0x00, 0x04, 0x00, 0xF0,  // Q2: offset_low=0xF0000400 (LE)
        0x00, 0x00, 0x08, 0x00,  // Q3
    };
    
    std::span<const uint8_t> header(packet, 16);
    uint64_t offset = ExtractDestOffset(header);
    
    EXPECT_EQ(0xFFFFF0000400ULL, offset)
        << "Should correctly extract Config ROM base address";
}

TEST_F(FCPPacketParsingTest, OffsetExtraction_FCPCommandAddress) {
    // Test FCP Command address: 0xFFFF_F0000B00
    const uint8_t packet[] = {
        0x10, 0x00, 0xC0, 0xFF,  // Q0
        0xFF, 0xFF, 0xC2, 0xFF,  // Q1: offset_high=0xFFFF
        0x00, 0x0B, 0x00, 0xF0,  // Q2: offset_low=0xF0000B00 (LE)
        0x00, 0x00, 0x08, 0x00,  // Q3
    };
    
    std::span<const uint8_t> header(packet, 16);
    uint64_t offset = ExtractDestOffset(header);
    
    EXPECT_EQ(0xFFFFF0000B00ULL, offset)
        << "Should correctly extract FCP Command address";
}

// =============================================================================
// Sign Extension Tests (12-bit offset_high)
// =============================================================================

TEST_F(FCPPacketParsingTest, SignExtension_Bit11Set_ExtendsToFFFF) {
    // Test sign extension when bit 11 of offset_high is set
    // offset_high = 0x0FFF (12 bits) should extend to 0xFFFF (16 bits)
    //
    // Q1 bytes [4-5] in LE format:
    //   byte[4] = 0xFF (offset_high[7:0])
    //   byte[5] = 0x0F (rCode=0, offset_high[11:8]=0xF)
    
    const uint8_t packet[] = {
        0x10, 0x00, 0xC0, 0xFF,  // Q0
        0xFF, 0x0F, 0xC2, 0xFF,  // Q1: offset_high=0x0FFF (should extend to 0xFFFF)
        0x00, 0x00, 0x00, 0x00,  // Q2: offset_low=0x00000000
        0x00, 0x00, 0x08, 0x00,  // Q3
    };
    
    std::span<const uint8_t> header(packet, 16);
    uint64_t offset = ExtractDestOffset(header);
    
    EXPECT_EQ(0xFFFF00000000ULL, offset)
        << "12-bit value 0x0FFF with bit 11 set should sign-extend to 0xFFFF";
}

TEST_F(FCPPacketParsingTest, SignExtension_Bit11Clear_NoExtension) {
    // Test no sign extension when bit 11 is clear
    // offset_high = 0x07FF (12 bits) should remain 0x07FF (no extension)
    
    const uint8_t packet[] = {
        0x10, 0x00, 0xC0, 0xFF,  // Q0
        0xFF, 0x07, 0xC2, 0xFF,  // Q1: offset_high=0x07FF (bit 11 clear)
        0x00, 0x00, 0x00, 0x00,  // Q2: offset_low=0x00000000
        0x00, 0x00, 0x08, 0x00,  // Q3
    };
    
    std::span<const uint8_t> header(packet, 16);
    uint64_t offset = ExtractDestOffset(header);
    
    EXPECT_EQ(0x07FF00000000ULL, offset)
        << "12-bit value 0x07FF with bit 11 clear should not sign-extend";
}

// =============================================================================
// Cross-Validation: ASFW vs Linux Implementation
// =============================================================================

TEST_F(FCPPacketParsingTest, CrossValidation_RandomOffsets) {
    // Test various random offsets to ensure ASFW and Linux methods agree
    // NOTE: offset_high is 12 bits (0x000-0xFFF), sign-extended to 16 bits
    struct TestCase {
        uint16_t offset_high;  // 12-bit value (will be sign-extended)
        uint32_t offset_low;
        uint64_t expected;
    };
    
    const TestCase testCases[] = {
        // FCP addresses (offset_high=0xFFF sign-extends to 0xFFFF)
        {0x0FFF, 0xF0000D00, 0xFFFFF0000D00ULL},  // FCP Response
        {0x0FFF, 0xF0000B00, 0xFFFFF0000B00ULL},  // FCP Command
        {0x0FFF, 0xF0000400, 0xFFFFF0000400ULL},  // Config ROM
        // Zero
        {0x0000, 0x00000000, 0x0000000000000000ULL},
        // Random values with offset_high < 0x800 (no sign extension)
        {0x0234, 0x56789ABC, 0x023456789ABCULL},
        {0x07CD, 0xEF012345, 0x07CDEF012345ULL},
        // Random values with offset_high >= 0x800 (sign extends)
        {0x0BCD, 0x12345678, 0xFBCD12345678ULL},  // 0x0BCD sign-extends to 0xFBCD
    };
    
    for (const auto& tc : testCases) {
        // Build packet with specified offset
        // Q1 bytes [4-5]: offset_high is 12 bits, stored in LE format
        //   byte[4] = offset_high[7:0]
        //   byte[5] = (rCode << 4) | offset_high[11:8]
        // For testing, use rCode=0
        uint8_t packet[16] = {
            0x10, 0x00, 0xC0, 0xFF,  // Q0
            static_cast<uint8_t>(tc.offset_high & 0xFF),         // Q1[4]: offset_high[7:0]
            static_cast<uint8_t>((tc.offset_high >> 8) & 0x0F),  // Q1[5]: offset_high[11:8] (rCode=0)
            0xC2, 0xFF,  // Q1[6-7]: srcID
            static_cast<uint8_t>(tc.offset_low & 0xFF),           // Q2[8]: offset_low[7:0]
            static_cast<uint8_t>((tc.offset_low >> 8) & 0xFF),    // Q2[9]: offset_low[15:8]
            static_cast<uint8_t>((tc.offset_low >> 16) & 0xFF),   // Q2[10]: offset_low[23:16]
            static_cast<uint8_t>((tc.offset_low >> 24) & 0xFF),   // Q2[11]: offset_low[31:24]
            0x00, 0x00, 0x08, 0x00,  // Q3
        };
        
        std::span<const uint8_t> header(packet, 16);
        
        uint64_t offset_asfw = ExtractDestOffset(header);
        uint64_t offset_linux = ExtractDestOffsetLinuxStyle(packet);
        
        EXPECT_EQ(tc.expected, offset_asfw)
            << "ASFW extraction failed for offset_high=0x" << std::hex << tc.offset_high
            << " offset_low=0x" << tc.offset_low;
        
        EXPECT_EQ(tc.expected, offset_linux)
            << "Linux extraction failed for offset_high=0x" << std::hex << tc.offset_high
            << " offset_low=0x" << tc.offset_low;
        
        EXPECT_EQ(offset_asfw, offset_linux)
            << "ASFW and Linux disagree for offset_high=0x" << std::hex << tc.offset_high
            << " offset_low=0x" << tc.offset_low;
    }
}

// =============================================================================
// FCP Address Detection Tests
// =============================================================================

TEST_F(FCPPacketParsingTest, FCPAddressDetection_ResponseAddress) {
    const uint8_t packet[] = {
        0x10, 0x7D, 0xC0, 0xFF,
        0xFF, 0xFF, 0xC2, 0xFF,
        0x00, 0x0D, 0x00, 0xF0,  // FCP Response: 0xFFFFF0000D00
        0x00, 0x00, 0x08, 0x00,
    };
    
    std::span<const uint8_t> header(packet, 16);
    uint64_t offset = ExtractDestOffset(header);
    
    EXPECT_EQ(kFCPResponseAddress, offset);
    EXPECT_TRUE(offset == kFCPResponseAddress) << "Should detect FCP Response address";
}

TEST_F(FCPPacketParsingTest, FCPAddressDetection_NotFCPResponse) {
    // Config ROM read - should NOT match FCP Response address
    const uint8_t packet[] = {
        0x10, 0x00, 0xC0, 0xFF,
        0xFF, 0xFF, 0xC2, 0xFF,
        0x00, 0x04, 0x00, 0xF0,  // Config ROM: 0xFFFFF0000400
        0x00, 0x00, 0x08, 0x00,
    };
    
    std::span<const uint8_t> header(packet, 16);
    uint64_t offset = ExtractDestOffset(header);
    
    EXPECT_NE(kFCPResponseAddress, offset);
    EXPECT_EQ(0xFFFFF0000400ULL, offset);
}

// =============================================================================
// Regression Tests: Previous Bugs
// =============================================================================

TEST_F(FCPPacketParsingTest, Regression_OffsetMismatch_Bug) {
    // This test documents the bug that was fixed:
    // Previous implementation extracted offset incorrectly, producing 0x000D00F00000
    // instead of the correct 0xFFFFF0000D00
    
    const uint8_t packet[] = {
        0x10, 0x7D, 0xC0, 0xFF,
        0xFF, 0xFF, 0xC2, 0xFF,
        0x00, 0x0D, 0x00, 0xF0,
        0x00, 0x00, 0x08, 0x00,
    };
    
    std::span<const uint8_t> header(packet, 16);
    uint64_t offset = ExtractDestOffset(header);
    
    // Should NOT produce the buggy value
    EXPECT_NE(0x000D00F00000ULL, offset) << "Should not produce buggy offset";
    
    // Should produce the correct value
    EXPECT_EQ(0xFFFFF0000D00ULL, offset) << "Should produce correct FCP Response offset";
}
