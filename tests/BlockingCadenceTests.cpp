// BlockingCadenceTests.cpp
// ASFW - Phase 1.5 Encoding Tests
//
// Tests for 48 kHz blocking cadence pattern.
// Reference: 000-48kORIG.txt
//

#include <gtest/gtest.h>
#include "Isoch/Encoding/BlockingCadence48k.hpp"

using namespace ASFW::Encoding;

//==============================================================================
// Constants Tests
//==============================================================================

TEST(BlockingCadenceTests, CorrectSamplesPerPacket) {
    EXPECT_EQ(kSamplesPerPacket48k, 8);
}

TEST(BlockingCadenceTests, CorrectDataPacketsPerPeriod) {
    EXPECT_EQ(kDataPacketsPer8Cycles, 6);
}

TEST(BlockingCadenceTests, CorrectNoDataPacketsPerPeriod) {
    EXPECT_EQ(kNoDataPacketsPer8Cycles, 2);
}

//==============================================================================
// Initial State Tests
//==============================================================================

TEST(BlockingCadenceTests, StartsAtCycleZero) {
    BlockingCadence48k cadence;
    EXPECT_EQ(cadence.getCycleIndex(), 0);
    EXPECT_EQ(cadence.getTotalCycles(), 0);
}

TEST(BlockingCadenceTests, FirstCycleIsNoData) {
    BlockingCadence48k cadence;
    EXPECT_FALSE(cadence.isDataPacket());
    EXPECT_EQ(cadence.samplesThisCycle(), 0);
}

//==============================================================================
// Pattern Tests - N-D-D-D Repeating
//==============================================================================

TEST(BlockingCadenceTests, FullPatternOver8Cycles) {
    BlockingCadence48k cadence;
    
    // Expected pattern: N-D-D-D-N-D-D-D
    bool expected[] = {false, true, true, true, false, true, true, true};
    
    for (int i = 0; i < 8; i++) {
        SCOPED_TRACE("Cycle " + std::to_string(i));
        EXPECT_EQ(cadence.isDataPacket(), expected[i]);
        cadence.advance();
    }
}

TEST(BlockingCadenceTests, PatternRepeatsAfter8Cycles) {
    BlockingCadence48k cadence;
    
    // Get first 8 cycles
    bool first8[8];
    for (int i = 0; i < 8; i++) {
        first8[i] = cadence.isDataPacket();
        cadence.advance();
    }
    
    // Next 8 should match
    for (int i = 0; i < 8; i++) {
        SCOPED_TRACE("Cycle " + std::to_string(i + 8));
        EXPECT_EQ(cadence.isDataPacket(), first8[i]);
        cadence.advance();
    }
}

TEST(BlockingCadenceTests, SamplesMatchPattern) {
    BlockingCadence48k cadence;
    
    // Expected: 0, 8, 8, 8, 0, 8, 8, 8
    uint32_t expected[] = {0, 8, 8, 8, 0, 8, 8, 8};
    
    for (int i = 0; i < 8; i++) {
        SCOPED_TRACE("Cycle " + std::to_string(i));
        EXPECT_EQ(cadence.samplesThisCycle(), expected[i]);
        cadence.advance();
    }
}

//==============================================================================
// Sample Count Verification
//==============================================================================

TEST(BlockingCadenceTests, Total48SamplesPer8Cycles) {
    BlockingCadence48k cadence;
    uint32_t totalSamples = 0;
    
    for (int i = 0; i < 8; i++) {
        totalSamples += cadence.samplesThisCycle();
        cadence.advance();
    }
    
    // 6 DATA × 8 samples = 48 samples
    EXPECT_EQ(totalSamples, 48);
}

TEST(BlockingCadenceTests, Correct48kSamplesPerSecond) {
    BlockingCadence48k cadence;
    uint32_t totalSamples = 0;
    
    // 8000 cycles = 1 second at FireWire rate
    for (int i = 0; i < 8000; i++) {
        totalSamples += cadence.samplesThisCycle();
        cadence.advance();
    }
    
    // Should be exactly 48000 samples
    EXPECT_EQ(totalSamples, 48000);
}

//==============================================================================
// Advance and Reset Tests
//==============================================================================

TEST(BlockingCadenceTests, AdvanceIncrementsCycle) {
    BlockingCadence48k cadence;
    
    EXPECT_EQ(cadence.getTotalCycles(), 0);
    cadence.advance();
    EXPECT_EQ(cadence.getTotalCycles(), 1);
    cadence.advance();
    EXPECT_EQ(cadence.getTotalCycles(), 2);
}

TEST(BlockingCadenceTests, AdvanceByMultiple) {
    BlockingCadence48k cadence;
    
    cadence.advanceBy(5);
    EXPECT_EQ(cadence.getTotalCycles(), 5);
    EXPECT_EQ(cadence.getCycleIndex(), 5);
}

TEST(BlockingCadenceTests, ResetClearsState) {
    BlockingCadence48k cadence;
    
    cadence.advanceBy(100);
    EXPECT_GT(cadence.getTotalCycles(), 0);
    
    cadence.reset();
    EXPECT_EQ(cadence.getTotalCycles(), 0);
    EXPECT_EQ(cadence.getCycleIndex(), 0);
    EXPECT_FALSE(cadence.isDataPacket());  // First cycle is NO-DATA
}

//==============================================================================
// FireBug Capture Pattern Validation
// Reference: 000-48kORIG.txt cycles 977-984
//==============================================================================

TEST(BlockingCadenceTests, MatchesFireBugPattern) {
    BlockingCadence48k cadence;
    
    // From capture (starting at an arbitrary point in the pattern):
    // 977: NO-DATA (8 bytes)
    // 978: DATA (72 bytes)
    // 979: DATA (72 bytes)
    // 980: DATA (72 bytes)
    // 981: NO-DATA (8 bytes)
    // 982: DATA (72 bytes)
    // 983: DATA (72 bytes)
    // 984: DATA (72 bytes)
    
    // This matches: N-D-D-D-N-D-D-D
    // Which is our pattern starting at cycle 0
    
    EXPECT_FALSE(cadence.isDataPacket()); cadence.advance();  // N
    EXPECT_TRUE(cadence.isDataPacket());  cadence.advance();  // D
    EXPECT_TRUE(cadence.isDataPacket());  cadence.advance();  // D
    EXPECT_TRUE(cadence.isDataPacket());  cadence.advance();  // D
    EXPECT_FALSE(cadence.isDataPacket()); cadence.advance();  // N
    EXPECT_TRUE(cadence.isDataPacket());  cadence.advance();  // D
    EXPECT_TRUE(cadence.isDataPacket());  cadence.advance();  // D
    EXPECT_TRUE(cadence.isDataPacket());  cadence.advance();  // D
}
