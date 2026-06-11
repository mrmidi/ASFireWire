// BlockingCadenceTests.cpp
// ASFW - Phase 1.5 Encoding Tests
//
// Tests for 48 kHz blocking cadence pattern.
//

#include <gtest/gtest.h>
#include "Audio/Wire/AMDTP/AmdtpCadence.hpp"

using namespace ASFW::Protocols::Audio::AMDTP;

//==============================================================================
// Initial State Tests
//==============================================================================

TEST(BlockingCadenceTests, StartsAtCycleZero) {
    Blocking48kCadence cadence;
    EXPECT_EQ(cadence.TotalCycles(), 0);
}

TEST(BlockingCadenceTests, FirstCycleIsNoData) {
    Blocking48kCadence cadence;
    EXPECT_FALSE(cadence.CurrentCycleIsData());
    EXPECT_EQ(cadence.CurrentCycleDataFrames(), 0);
}

//==============================================================================
// Pattern Tests - N-D-D-D Repeating
//==============================================================================

TEST(BlockingCadenceTests, FullPatternOver8Cycles) {
    Blocking48kCadence cadence;
    
    // Expected pattern: N-D-D-D-N-D-D-D
    bool expected[] = {false, true, true, true, false, true, true, true};
    
    for (int i = 0; i < 8; i++) {
        SCOPED_TRACE("Cycle " + std::to_string(i));
        EXPECT_EQ(cadence.CurrentCycleIsData(), expected[i]);
        cadence.AdvanceCycle();
    }
}

TEST(BlockingCadenceTests, PatternRepeatsAfter8Cycles) {
    Blocking48kCadence cadence;
    
    // Get first 8 cycles
    bool first8[8];
    for (int i = 0; i < 8; i++) {
        first8[i] = cadence.CurrentCycleIsData();
        cadence.AdvanceCycle();
    }
    
    // Next 8 should match
    for (int i = 0; i < 8; i++) {
        SCOPED_TRACE("Cycle " + std::to_string(i + 8));
        EXPECT_EQ(cadence.CurrentCycleIsData(), first8[i]);
        cadence.AdvanceCycle();
    }
}

TEST(BlockingCadenceTests, SamplesMatchPattern) {
    Blocking48kCadence cadence;
    
    // Expected: 0, 8, 8, 8, 0, 8, 8, 8
    uint32_t expected[] = {0, 8, 8, 8, 0, 8, 8, 8};
    
    for (int i = 0; i < 8; i++) {
        SCOPED_TRACE("Cycle " + std::to_string(i));
        EXPECT_EQ(cadence.CurrentCycleDataFrames(), expected[i]);
        cadence.AdvanceCycle();
    }
}

//==============================================================================
// Sample Count Verification
//==============================================================================

TEST(BlockingCadenceTests, Total48SamplesPer8Cycles) {
    Blocking48kCadence cadence;
    uint32_t totalSamples = 0;
    
    for (int i = 0; i < 8; i++) {
        totalSamples += cadence.CurrentCycleDataFrames();
        cadence.AdvanceCycle();
    }
    
    // 6 DATA × 8 samples = 48 samples
    EXPECT_EQ(totalSamples, 48);
}

TEST(BlockingCadenceTests, Correct48kSamplesPerSecond) {
    Blocking48kCadence cadence;
    uint32_t totalSamples = 0;
    
    // 8000 cycles = 1 second at FireWire rate
    for (int i = 0; i < 8000; i++) {
        totalSamples += cadence.CurrentCycleDataFrames();
        cadence.AdvanceCycle();
    }
    
    // Should be exactly 48000 samples
    EXPECT_EQ(totalSamples, 48000);
}

//==============================================================================
// Advance and Reset Tests
//==============================================================================

TEST(BlockingCadenceTests, AdvanceIncrementsCycle) {
    Blocking48kCadence cadence;
    
    EXPECT_EQ(cadence.TotalCycles(), 0);
    cadence.AdvanceCycle();
    EXPECT_EQ(cadence.TotalCycles(), 1);
    cadence.AdvanceCycle();
    EXPECT_EQ(cadence.TotalCycles(), 2);
}

TEST(BlockingCadenceTests, ResetClearsState) {
    Blocking48kCadence cadence;
    
    for (int i = 0; i < 100; ++i) {
        cadence.AdvanceCycle();
    }
    EXPECT_GT(cadence.TotalCycles(), 0);
    
    cadence.Reset();
    EXPECT_EQ(cadence.TotalCycles(), 0);
    EXPECT_FALSE(cadence.CurrentCycleIsData());  // First cycle is NO-DATA
}

//==============================================================================
// FireBug Capture Pattern Validation
// Reference: 000-48kORIG.txt cycles 977-984
//==============================================================================

TEST(BlockingCadenceTests, MatchesFireBugPattern) {
    Blocking48kCadence cadence;
    
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
    
    EXPECT_FALSE(cadence.CurrentCycleIsData()); cadence.AdvanceCycle();  // N
    EXPECT_TRUE(cadence.CurrentCycleIsData());  cadence.AdvanceCycle();  // D
    EXPECT_TRUE(cadence.CurrentCycleIsData());  cadence.AdvanceCycle();  // D
    EXPECT_TRUE(cadence.CurrentCycleIsData());  cadence.AdvanceCycle();  // D
    EXPECT_FALSE(cadence.CurrentCycleIsData()); cadence.AdvanceCycle();  // N
    EXPECT_TRUE(cadence.CurrentCycleIsData());  cadence.AdvanceCycle();  // D
    EXPECT_TRUE(cadence.CurrentCycleIsData());  cadence.AdvanceCycle();  // D
    EXPECT_TRUE(cadence.CurrentCycleIsData());  cadence.AdvanceCycle();  // D
}
