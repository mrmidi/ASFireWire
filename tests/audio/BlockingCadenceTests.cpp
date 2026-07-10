// BlockingCadenceTests.cpp
// ASFW - Phase 1.5 Encoding Tests
//
// Tests for blocking cadence patterns and the detached rational engine.
//

#include <gtest/gtest.h>

#include "Audio/Wire/AMDTP/AmdtpCadence.hpp"
#include "generated/AmdtpBlockingCadence441Golden.hpp"

#include <cstdint>
#include <string>
#include <vector>

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

//==============================================================================
// Rational blocking cadence -- generated 44.1 kHz golden data
//==============================================================================

TEST(RationalBlockingCadenceTests, MatchesGenerated441CadenceAndOffsets) {
    using namespace ASFW::Test::Golden;

    RationalBlockingCadence cadence;
    ASSERT_TRUE(cadence.Configure(k441SampleRateHz, k441SytInterval));

    uint32_t dataPackets = 0;
    uint32_t dataBlocks = 0;
    std::size_t offsetIndex = 0;
    for (std::size_t cycle = 0; cycle < k441CadencePeriodCycles; ++cycle) {
        SCOPED_TRACE("Cycle " + std::to_string(cycle));
        const RationalBlockingDecision decision = cadence.CurrentDecision();
        const bool expectedData = Is441DataCycle(cycle);

        EXPECT_EQ(decision.isData, expectedData);
        if (expectedData) {
            EXPECT_EQ(decision.dataBlocks, k441SytInterval);
            if (offsetIndex < k441SytOffsets.size()) {
                EXPECT_EQ(decision.sytOffsetTicks, k441SytOffsets[offsetIndex]);
            }
            ++dataPackets;
            dataBlocks += decision.dataBlocks;
            ++offsetIndex;
        } else {
            EXPECT_EQ(decision.dataBlocks, 0);
            EXPECT_EQ(decision.sytOffsetTicks, kNoSytOffset);
        }
        cadence.AdvanceCycle();
    }

    EXPECT_EQ(dataPackets, k441DataPacketsPerPeriod);
    EXPECT_EQ(dataBlocks, 3528u);
    EXPECT_EQ(offsetIndex, k441DataPacketsPerPeriod);
    EXPECT_EQ(cadence.TotalCycles(), k441CadencePeriodCycles);
}

TEST(RationalBlockingCadenceTests, MatchesGenerated441SytDeltaSequence) {
    using namespace ASFW::Test::Golden;

    RationalBlockingCadence cadence;
    ASSERT_TRUE(cadence.Configure(k441SampleRateHz, k441SytInterval));

    std::vector<uint64_t> absoluteDeadlineTicks;
    for (std::size_t cycle = 0; cycle < k441CadencePeriodCycles; ++cycle) {
        const RationalBlockingDecision decision = cadence.CurrentDecision();
        if (decision.isData) {
            absoluteDeadlineTicks.push_back(
                cadence.TotalCycles() * 3072ULL + decision.sytOffsetTicks);
        }
        cadence.AdvanceCycle();
    }

    ASSERT_GE(absoluteDeadlineTicks.size(), k441SytDeltaPeriodEvents + 1);
    for (std::size_t index = 0; index < k441SytDeltaPeriodEvents; ++index) {
        SCOPED_TRACE("SYT delta " + std::to_string(index));
        EXPECT_EQ(absoluteDeadlineTicks[index + 1] - absoluteDeadlineTicks[index],
                  k441SytDeltaTicks[index]);
    }
}

TEST(RationalBlockingCadenceTests, Preserves441FamilyDeadlineSequence) {
    RationalBlockingCadence at441;
    RationalBlockingCadence at882;
    RationalBlockingCadence at1764;
    ASSERT_TRUE(at441.Configure(44'100, 8));
    ASSERT_TRUE(at882.Configure(88'200, 16));
    ASSERT_TRUE(at1764.Configure(176'400, 32));

    for (std::size_t cycle = 0;
         cycle < ASFW::Test::Golden::k441CadencePeriodCycles;
         ++cycle) {
        SCOPED_TRACE("Cycle " + std::to_string(cycle));
        const RationalBlockingDecision base = at441.CurrentDecision();
        const RationalBlockingDecision x2 = at882.CurrentDecision();
        const RationalBlockingDecision x4 = at1764.CurrentDecision();

        EXPECT_EQ(x2.isData, base.isData);
        EXPECT_EQ(x4.isData, base.isData);
        EXPECT_EQ(x2.sytOffsetTicks, base.sytOffsetTicks);
        EXPECT_EQ(x4.sytOffsetTicks, base.sytOffsetTicks);
        EXPECT_EQ(x2.dataBlocks, base.isData ? 16 : 0);
        EXPECT_EQ(x4.dataBlocks, base.isData ? 32 : 0);

        at441.AdvanceCycle();
        at882.AdvanceCycle();
        at1764.AdvanceCycle();
    }
}

TEST(RationalBlockingCadenceTests, DeadlineAtCycleEndBelongsToNextCycle) {
    RationalBlockingCadence cadence;
    constexpr uint64_t deadlineAtCycleEnd = 3072ULL * 44'100;
    ASSERT_TRUE(cadence.Configure(44'100, 8, deadlineAtCycleEnd));

    EXPECT_FALSE(cadence.CurrentDecision().isData);
    cadence.AdvanceCycle();
    const RationalBlockingDecision next = cadence.CurrentDecision();
    EXPECT_TRUE(next.isData);
    EXPECT_EQ(next.sytOffsetTicks, 0);

    cadence.Reset();
    EXPECT_FALSE(cadence.CurrentDecision().isData);
}

TEST(RationalBlockingCadenceTests, RejectsInvalidOrMultiEventConfiguration) {
    RationalBlockingCadence cadence;

    EXPECT_FALSE(cadence.Configure(0, 8));
    EXPECT_FALSE(cadence.Configure(44'100, 0));
    EXPECT_FALSE(cadence.Configure(48'000, 4));
    EXPECT_FALSE(cadence.IsConfigured());
    EXPECT_FALSE(cadence.CurrentDecision().isData);
}
