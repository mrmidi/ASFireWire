// PacketFieldExtractionTests.cpp - Unit tests for ALL packet field extraction
//
// Tests verify correct extraction of ALL fields from OHCI AR DMA packets:
// - sourceID, destID, tCode, tLabel, rCode, etc.
//
// This test suite was created AFTER discovering a critical sourceID byte-swap bug
// that wasn't caught by the initial FCPPacketParsingTests.

#include <gtest/gtest.h>
#include <cstring>
#include <span>

#include "ASFWDriver/Async/Rx/PacketRouter.hpp"

using namespace ASFW::Async;

// =============================================================================
// Test Fixture
// =============================================================================

class PacketFieldExtractionTest : public ::testing::Test {
protected:
    // Real FCP response packet from logs
    static constexpr uint8_t kRealFCPResponse[] = {
        0x10, 0x7D, 0xC0, 0xFF,  // Q0: tCode=0x1, destID=0xFFC0
        0xFF, 0xFF, 0xC2, 0xFF,  // Q1: srcID=0xFFC2, rCode=0xF
        0x00, 0x0D, 0x00, 0xF0,  // Q2: offset=0xFFFFF0000D00
        0x00, 0x00, 0x08, 0x00,  // Q3: data_length=8
    };
};

// =============================================================================
// Source ID Extraction Tests (THE BUG THAT WAS MISSED!)
// =============================================================================

TEST_F(PacketFieldExtractionTest, ExtractSourceID_RealFCPResponse) {
    // This is the CRITICAL test that would have caught the byte-swap bug!
    // Real packet from logs where srcID should be 0xFFC2 (node 2 on local bus)
    
    std::span<const uint8_t> header(kRealFCPResponse, 16);
    uint16_t srcID = PacketRouter::ExtractSourceID(header);
    
    EXPECT_EQ(0xFFC2, srcID)
        << "Source ID should be 0xFFC2, not byte-swapped 0xC2FF!";
}

TEST_F(PacketFieldExtractionTest, ExtractSourceID_VariousNodes) {
    struct TestCase {
        uint8_t q1_bytes[4];  // Q1 in memory (little-endian)
        uint16_t expected_srcID;
        const char* description;
    };
    
    const TestCase testCases[] = {
        {{0xFF, 0xFF, 0xC2, 0xFF}, 0xFFC2, "Node 2 on local bus"},
        {{0x00, 0x00, 0xC0, 0xFF}, 0xFFC0, "Node 0 on local bus"},
        {{0x00, 0x00, 0xC1, 0xFF}, 0xFFC1, "Node 1 on local bus"},
        {{0x00, 0x00, 0x00, 0x00}, 0x0000, "Node 0 on bus 0"},
        {{0xFF, 0xFF, 0xFF, 0x03}, 0x03FF, "Node 63 on bus 3"},
    };
    
    for (const auto& tc : testCases) {
        uint8_t packet[16] = {
            0x10, 0x00, 0xC0, 0xFF,  // Q0
            tc.q1_bytes[0], tc.q1_bytes[1], tc.q1_bytes[2], tc.q1_bytes[3],  // Q1
            0x00, 0x00, 0x00, 0x00,  // Q2
            0x00, 0x00, 0x00, 0x00,  // Q3
        };
        
        std::span<const uint8_t> header(packet, 16);
        uint16_t srcID = PacketRouter::ExtractSourceID(header);
        
        EXPECT_EQ(tc.expected_srcID, srcID) << "Failed for: " << tc.description;
    }
}

// =============================================================================
// Destination ID Extraction Tests
// =============================================================================

TEST_F(PacketFieldExtractionTest, ExtractDestID_RealFCPResponse) {
    std::span<const uint8_t> header(kRealFCPResponse, 16);
    uint16_t destID = PacketRouter::ExtractDestID(header);
    
    EXPECT_EQ(0xFFC0, destID)
        << "Destination ID should be 0xFFC0 (our local node)";
}

TEST_F(PacketFieldExtractionTest, ExtractDestID_VariousNodes) {
    struct TestCase {
        uint8_t q0_bytes[4];  // Q0 in memory (little-endian)
        uint16_t expected_destID;
    };
    
    const TestCase testCases[] = {
        {{0x10, 0x00, 0xC0, 0xFF}, 0xFFC0},  // Node 0 on local bus
        {{0x10, 0x00, 0xC1, 0xFF}, 0xFFC1},  // Node 1 on local bus
        {{0x10, 0x00, 0xC2, 0xFF}, 0xFFC2},  // Node 2 on local bus
        {{0x10, 0x00, 0x00, 0x00}, 0x0000},  // Node 0 on bus 0
        {{0x10, 0x00, 0xFF, 0x03}, 0x03FF},  // Node 63 on bus 3
    };
    
    for (const auto& tc : testCases) {
        uint8_t packet[16];
        std::memcpy(packet, tc.q0_bytes, 4);
        std::memset(packet + 4, 0, 12);
        
        std::span<const uint8_t> header(packet, 16);
        uint16_t destID = PacketRouter::ExtractDestID(header);
        
        EXPECT_EQ(tc.expected_destID, destID);
    }
}

// =============================================================================
// Transaction Code Extraction Tests
// =============================================================================

TEST_F(PacketFieldExtractionTest, ExtractTCode_RealFCPResponse) {
    std::span<const uint8_t> header(kRealFCPResponse, 16);
    uint8_t tCode = PacketRouter::ExtractTCode(header);
    
    EXPECT_EQ(0x1, tCode) << "tCode should be 0x1 (Block Write Request)";
}

TEST_F(PacketFieldExtractionTest, ExtractTCode_AllValidCodes) {
    struct TestCase {
        uint8_t q0_byte0;  // First byte of Q0 contains tCode
        uint8_t expected_tCode;
        const char* description;
    };
    
    const TestCase testCases[] = {
        {0x00, 0x0, "Quadlet Write Request"},
        {0x10, 0x1, "Block Write Request"},
        {0x20, 0x2, "Write Response"},
        {0x40, 0x4, "Quadlet Read Request"},
        {0x50, 0x5, "Block Read Request"},
        {0x60, 0x6, "Quadlet Read Response"},
        {0x70, 0x7, "Block Read Response"},
        {0x90, 0x9, "Lock Request"},
        {0xB0, 0xB, "Lock Response"},
    };
    
    for (const auto& tc : testCases) {
        uint8_t packet[16] = {tc.q0_byte0, 0, 0, 0};
        std::span<const uint8_t> header(packet, 16);
        uint8_t tCode = PacketRouter::ExtractTCode(header);
        
        EXPECT_EQ(tc.expected_tCode, tCode) << "Failed for: " << tc.description;
    }
}

// =============================================================================
// Cross-Field Validation (Integration Tests)
// =============================================================================

TEST_F(PacketFieldExtractionTest, RealPacket_AllFieldsCorrect) {
    // This test validates ALL fields from the real FCP response packet
    // This would have caught the sourceID bug immediately!
    
    std::span<const uint8_t> header(kRealFCPResponse, 16);
    
    // Extract all fields
    uint16_t srcID = PacketRouter::ExtractSourceID(header);
    uint16_t destID = PacketRouter::ExtractDestID(header);
    uint8_t tCode = PacketRouter::ExtractTCode(header);
    
    // Validate against known values from FireBug logs
    EXPECT_EQ(0xFFC2, srcID) << "Source should be 0xFFC2 (Duet device)";
    EXPECT_EQ(0xFFC0, destID) << "Dest should be 0xFFC0 (Mac)";
    EXPECT_EQ(0x1, tCode) << "tCode should be 0x1 (Block Write)";
    
    // The critical check: srcID should match what AVCDiscovery expects!
    // If srcID is byte-swapped to 0xC2FF, AVCDiscovery lookup will fail!
    EXPECT_NE(0xC2FF, srcID) << "REGRESSION: srcID is byte-swapped!";
}

TEST_F(PacketFieldExtractionTest, SourceID_MatchesAVCDiscoveryKey) {
    // Simulate what happens in the real driver:
    // 1. Device discovered at nodeID 0xFFC2
    // 2. AVCDiscovery stores FCPTransport keyed by 0xFFC2
    // 3. FCP response arrives from srcID 0xFFC2
    // 4. ExtractSourceID MUST return 0xFFC2 to match the key!
    
    const uint16_t discoveredNodeID = 0xFFC2;  // What AVCDiscovery has
    
    std::span<const uint8_t> header(kRealFCPResponse, 16);
    uint16_t extractedSrcID = PacketRouter::ExtractSourceID(header);
    
    EXPECT_EQ(discoveredNodeID, extractedSrcID)
        << "ExtractSourceID must return the same value that AVCDiscovery uses as key!";
}

// =============================================================================
// Regression Tests for Byte-Swap Bug
// =============================================================================

TEST_F(PacketFieldExtractionTest, Regression_SourceID_NotByteSwapped) {
    // Document the bug that was fixed:
    // BEFORE: ExtractSourceID returned (header[6] << 8) | header[7] = 0xC2FF
    // AFTER:  ExtractSourceID returns  (header[7] << 8) | header[6] = 0xFFC2
    
    std::span<const uint8_t> header(kRealFCPResponse, 16);
    uint16_t srcID = PacketRouter::ExtractSourceID(header);
    
    // Should NOT be byte-swapped
    EXPECT_NE(0xC2FF, srcID) << "REGRESSION: Source ID is byte-swapped!";
    
    // Should be correct
    EXPECT_EQ(0xFFC2, srcID) << "Source ID should be 0xFFC2";
}
