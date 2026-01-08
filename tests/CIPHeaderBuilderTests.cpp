// CIPHeaderBuilderTests.cpp
// ASFW - Phase 1.5 Encoding Tests
//
// Tests for CIP header builder using real FireBug capture data.
// Reference: 000-48kORIG.txt
//

#include <gtest/gtest.h>
#include "Isoch/Encoding/CIPHeaderBuilder.hpp"

using namespace ASFW::Encoding;

//==============================================================================
// Constants Tests
//==============================================================================

TEST(CIPHeaderBuilderTests, CorrectFormatConstant) {
    EXPECT_EQ(kCIPFormatAM824, 0x10);
}

TEST(CIPHeaderBuilderTests, CorrectSYTNoDataConstant) {
    EXPECT_EQ(kSYTNoData, 0xFFFF);
}

TEST(CIPHeaderBuilderTests, CorrectSFCConstant) {
    EXPECT_EQ(kSFC_48kHz, 0x02);
}

//==============================================================================
// Construction Tests
//==============================================================================

TEST(CIPHeaderBuilderTests, DefaultConstruction) {
    CIPHeaderBuilder builder;
    EXPECT_EQ(builder.getSID(), 0);
    EXPECT_EQ(builder.getDBS(), 2);
}

TEST(CIPHeaderBuilderTests, ConstructWithSID) {
    CIPHeaderBuilder builder(0x3F);  // Max 6-bit value
    EXPECT_EQ(builder.getSID(), 0x3F);
}

TEST(CIPHeaderBuilderTests, SIDMaskedTo6Bits) {
    CIPHeaderBuilder builder(0xFF);  // Only lower 6 bits should be kept
    EXPECT_EQ(builder.getSID(), 0x3F);
}

TEST(CIPHeaderBuilderTests, SetSID) {
    CIPHeaderBuilder builder;
    builder.setSID(0x02);
    EXPECT_EQ(builder.getSID(), 0x02);
}

TEST(CIPHeaderBuilderTests, SetDBS) {
    CIPHeaderBuilder builder;
    builder.setDBS(4);
    EXPECT_EQ(builder.getDBS(), 4);
}

//==============================================================================
// FireBug Capture Validation - DATA Packets
// Reference: 000-48kORIG.txt cycle 978
//==============================================================================

// Capture shows: Q0 = 020200c0, Q1 = 900279fe
// SID=0x02, DBS=0x02, DBC=0xC0, SYT=0x79FE
TEST(CIPHeaderBuilderTests, MatchesFireBugCapture_DataPacket) {
    CIPHeaderBuilder builder(0x02);  // SID = 2
    CIPHeader header = builder.build(0xC0, 0x79FE, false);
    
    // Q0 before swap: [SID=02][DBS=02][00][DBC=C0] = 0x020200C0
    // Q0 after swap:  0xC0000202
    EXPECT_EQ(header.q0, 0xC0000202);
    
    // Q1 before swap: [EOH=10][FMT=10][FDF=02][SYT=79FE] = 0x900279FE
    // Q1 after swap:  0xFE790290
    EXPECT_EQ(header.q1, 0xFE790290);
}

// Another DATA packet from capture: cycle 979
// Q0 = 020200c8, Q1 = 900291fe
TEST(CIPHeaderBuilderTests, MatchesFireBugCapture_DataPacket2) {
    CIPHeaderBuilder builder(0x02);
    CIPHeader header = builder.build(0xC8, 0x91FE, false);
    
    // After swap
    EXPECT_EQ(header.q0, 0xC8000202);
}

//==============================================================================
// FireBug Capture Validation - NO-DATA Packets
// Reference: 000-48kORIG.txt cycle 977
//==============================================================================

// Capture shows: Q0 = 020200c0, Q1 = 9002ffff
// SID=0x02, DBS=0x02, DBC=0xC0, SYT=0xFFFF (NO-DATA)
TEST(CIPHeaderBuilderTests, MatchesFireBugCapture_NoDataPacket) {
    CIPHeaderBuilder builder(0x02);
    CIPHeader header = builder.build(0xC0, 0x0000, true);  // isNoData=true
    
    // Q0 same as DATA packet with same DBC
    EXPECT_EQ(header.q0, 0xC0000202);
    
    // Q1 before swap: [EOH=10][FMT=10][FDF=02][SYT=FFFF] = 0x9002FFFF
    // Q1 after swap:  0xFFFF0290
    EXPECT_EQ(header.q1, 0xFFFF0290);
}

TEST(CIPHeaderBuilderTests, BuildNoDataConvenience) {
    CIPHeaderBuilder builder(0x02);
    CIPHeader header = builder.buildNoData(0xC0);
    
    // Should produce same result as build(0xC0, X, true)
    CIPHeader expected = builder.build(0xC0, 0x1234, true);
    EXPECT_EQ(header.q0, expected.q0);
    EXPECT_EQ(header.q1, expected.q1);
}

//==============================================================================
// DBC Wraparound Tests
//==============================================================================

TEST(CIPHeaderBuilderTests, DBCWraparound) {
    CIPHeaderBuilder builder(0x02);
    
    // DBC = 0xF8
    CIPHeader h1 = builder.build(0xF8, 0x0000, false);
    
    // DBC = 0x00 (wrapped)
    CIPHeader h2 = builder.build(0x00, 0x0000, false);
    
    // Verify the DBC byte is correct in both (after swap, DBC is MSB)
    EXPECT_EQ((h1.q0 >> 24) & 0xFF, 0xF8);
    EXPECT_EQ((h2.q0 >> 24) & 0xFF, 0x00);
}

//==============================================================================
// SYT Value Tests
//==============================================================================

TEST(CIPHeaderBuilderTests, SYTZero) {
    CIPHeaderBuilder builder(0x00);
    CIPHeader header = builder.build(0x00, 0x0000, false);
    
    // SYT=0x0000 should be in the header
    // Q1 before swap: 0x90020000
    // Q1 after swap:  0x00000290
    EXPECT_EQ(header.q1, 0x00000290);
}

TEST(CIPHeaderBuilderTests, SYTMaxValue) {
    CIPHeaderBuilder builder(0x00);
    CIPHeader header = builder.build(0x00, 0xFFFE, false);  // Not 0xFFFF
    
    // SYT=0xFFFE should be preserved (not NO-DATA)
    // Q1 before swap: 0x9002FFFE
    // Q1 after swap:  0xFEFF0290
    EXPECT_EQ(header.q1, 0xFEFF0290);
}

//==============================================================================
// Q0/Q1 Field Verification
//==============================================================================

TEST(CIPHeaderBuilderTests, Q0FieldLayout) {
    CIPHeaderBuilder builder(0x15);  // Arbitrary SID
    builder.setDBS(0x03);
    CIPHeader header = builder.build(0xAB, 0x0000, false);
    
    // Before swap: [SID=15][DBS=03][00][DBC=AB] = 0x150300AB
    // After swap:  0xAB000315
    EXPECT_EQ(header.q0, 0xAB000315);
}
