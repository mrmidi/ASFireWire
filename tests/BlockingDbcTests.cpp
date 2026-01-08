// BlockingDbcTests.cpp
// ASFW - Phase 1.5 Encoding Tests
//
// Tests for Data Block Counter (DBC) tracking per IEC 61883-1.
// Reference: 000-48kORIG.txt
//

#include <gtest/gtest.h>
#include "Isoch/Encoding/BlockingDbcGenerator.hpp"
#include "Isoch/Encoding/BlockingCadence48k.hpp"

using namespace ASFW::Encoding;

//==============================================================================
// Initial State Tests
//==============================================================================

TEST(BlockingDbcTests, DefaultStartsAtZero) {
    BlockingDbcGenerator dbc;
    EXPECT_EQ(dbc.peekNextDbc(), 0);
}

TEST(BlockingDbcTests, ConstructWithInitialValue) {
    BlockingDbcGenerator dbc(0xC0);
    EXPECT_EQ(dbc.peekNextDbc(), 0xC0);
}

//==============================================================================
// Basic DBC Behavior
//==============================================================================

TEST(BlockingDbcTests, DataPacketIncrementsByDefault8) {
    BlockingDbcGenerator dbc;
    
    EXPECT_EQ(dbc.getDbc(true), 0);    // Returns 0, then increments
    EXPECT_EQ(dbc.peekNextDbc(), 8);   // Next value should be 8
    
    EXPECT_EQ(dbc.getDbc(true), 8);    // Returns 8, then increments
    EXPECT_EQ(dbc.peekNextDbc(), 16);
}

TEST(BlockingDbcTests, NoDataDoesNotIncrement) {
    BlockingDbcGenerator dbc(0x10);
    
    EXPECT_EQ(dbc.getDbc(false), 0x10);  // Returns 0x10
    EXPECT_EQ(dbc.peekNextDbc(), 0x10);  // Still 0x10 (no increment)
    
    EXPECT_EQ(dbc.getDbc(false), 0x10);  // Still returns 0x10
}

//==============================================================================
// Blocking Mode DBC Rules
//==============================================================================

// Rule: NO-DATA packet reuses the DBC of the following DATA packet
TEST(BlockingDbcTests, NoDataReusesFollowingDataDbc) {
    BlockingDbcGenerator dbc(0xC0);
    
    // NO-DATA should return 0xC0 without incrementing
    uint8_t noDataDbc = dbc.getDbc(false);
    EXPECT_EQ(noDataDbc, 0xC0);
    
    // DATA should also return 0xC0, then increment
    uint8_t dataDbc = dbc.getDbc(true);
    EXPECT_EQ(dataDbc, 0xC0);
    
    // Next DATA should be 0xC8
    EXPECT_EQ(dbc.peekNextDbc(), 0xC8);
}

// Rule: Consecutive DATA packets increment DBC by 8
TEST(BlockingDbcTests, ConsecutiveDataIncrementsBy8) {
    BlockingDbcGenerator dbc(0xC0);
    
    EXPECT_EQ(dbc.getDbc(true), 0xC0);
    EXPECT_EQ(dbc.getDbc(true), 0xC8);
    EXPECT_EQ(dbc.getDbc(true), 0xD0);
    EXPECT_EQ(dbc.getDbc(true), 0xD8);
}

//==============================================================================
// Wraparound Tests
//==============================================================================

TEST(BlockingDbcTests, WrapsAt256) {
    BlockingDbcGenerator dbc(0xF8);
    
    EXPECT_EQ(dbc.getDbc(true), 0xF8);
    EXPECT_EQ(dbc.peekNextDbc(), 0x00);  // Wrapped
    
    EXPECT_EQ(dbc.getDbc(true), 0x00);
    EXPECT_EQ(dbc.peekNextDbc(), 0x08);
}

TEST(BlockingDbcTests, WrapsCorrectlyAtBoundary) {
    BlockingDbcGenerator dbc(0xFC);
    
    EXPECT_EQ(dbc.getDbc(true), 0xFC);
    // 0xFC + 8 = 0x104, truncated to 8 bits = 0x04
    EXPECT_EQ(dbc.peekNextDbc(), 0x04);
}

//==============================================================================
// FireBug Capture Validation
// Reference: 000-48kORIG.txt cycles 977-984
//==============================================================================

TEST(BlockingDbcTests, MatchesFireBugSequence) {
    // From capture:
    // Cycle 977 (NO-DATA): DBC = 0xC0
    // Cycle 978 (DATA):    DBC = 0xC0 (reused!)
    // Cycle 979 (DATA):    DBC = 0xC8
    // Cycle 980 (DATA):    DBC = 0xD0
    // Cycle 981 (NO-DATA): DBC = 0xD8
    // Cycle 982 (DATA):    DBC = 0xD8 (reused!)
    // Cycle 983 (DATA):    DBC = 0xE0
    // Cycle 984 (DATA):    DBC = 0xE8
    
    BlockingDbcGenerator dbc(0xC0);
    BlockingCadence48k cadence;
    
    // Track expected DBC values
    uint8_t expected[] = {0xC0, 0xC0, 0xC8, 0xD0, 0xD8, 0xD8, 0xE0, 0xE8};
    
    for (int i = 0; i < 8; i++) {
        SCOPED_TRACE("Cycle " + std::to_string(977 + i));
        
        bool isData = cadence.isDataPacket();
        uint8_t dbcValue = dbc.getDbc(isData);
        
        EXPECT_EQ(dbcValue, expected[i])
            << "isData=" << isData;
        
        cadence.advance();
    }
}

//==============================================================================
// Reset Tests
//==============================================================================

TEST(BlockingDbcTests, ResetToZero) {
    BlockingDbcGenerator dbc(0x50);
    dbc.getDbc(true);  // Increment
    dbc.getDbc(true);  // Increment more
    
    dbc.reset();
    EXPECT_EQ(dbc.peekNextDbc(), 0);
}

TEST(BlockingDbcTests, ResetToSpecificValue) {
    BlockingDbcGenerator dbc(0);
    dbc.getDbc(true);  // Increment
    
    dbc.reset(0xC0);
    EXPECT_EQ(dbc.peekNextDbc(), 0xC0);
}

//==============================================================================
// Custom Sample Count Tests
//==============================================================================

TEST(BlockingDbcTests, CustomSampleCount) {
    BlockingDbcGenerator dbc;
    
    // Increment by custom amount
    EXPECT_EQ(dbc.getDbc(true, 4), 0);
    EXPECT_EQ(dbc.peekNextDbc(), 4);
    
    EXPECT_EQ(dbc.getDbc(true, 4), 4);
    EXPECT_EQ(dbc.peekNextDbc(), 8);
}
