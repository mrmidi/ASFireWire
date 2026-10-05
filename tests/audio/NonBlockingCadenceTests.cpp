// NonBlockingCadenceTests.cpp
// ASFW - Isoch Encoding Tests
//
// Tests for 48 kHz non-blocking cadence pattern.
//

#include <gtest/gtest.h>
#include "Audio/Wire/AMDTP/AmdtpCadence.hpp"

using namespace ASFW::Protocols::Audio::AMDTP;

TEST(NonBlockingCadenceTests, AlwaysDataEveryCycle) {
    NonBlocking48kCadence cadence;
    for (int i = 0; i < 16; ++i) {
        SCOPED_TRACE("Cycle " + std::to_string(i));
        EXPECT_TRUE(cadence.CurrentCycleIsData());
        EXPECT_EQ(cadence.CurrentCycleDataFrames(), 6u);
        cadence.AdvanceCycle();
    }
}

TEST(NonBlockingCadenceTests, Produces48kSamplesPerSecond) {
    NonBlocking48kCadence cadence;
    uint32_t totalSamples = 0;

    for (int i = 0; i < 8000; ++i) {
        totalSamples += cadence.CurrentCycleDataFrames();
        cadence.AdvanceCycle();
    }

    EXPECT_EQ(totalSamples, 48000u);
}

TEST(NonBlockingCadenceTests, ResetRestoresInitialState) {
    NonBlocking48kCadence cadence;
    for (int i = 0; i < 123; ++i) {
        cadence.AdvanceCycle();
    }
    EXPECT_GT(cadence.TotalCycles(), 0u);

    cadence.Reset();
    EXPECT_EQ(cadence.TotalCycles(), 0u);
    EXPECT_TRUE(cadence.CurrentCycleIsData());
}

TEST(NonBlockingCadenceTests, AllRatesHaveExactTotalsAndEarliestRoundedPackets) {
    for (const uint32_t rate : {32000U, 44100U, 48000U, 88200U, 96000U, 176400U, 192000U}) {
        SCOPED_TRACE(rate);
        NonBlockingCadence cadence;
        ASSERT_TRUE(cadence.Configure(rate));
        uint64_t total = 0;
        // Ten complete seconds exercises fractional periods and resets no state.
        for (uint64_t cycle = 0; cycle < 80000; ++cycle) {
            const uint64_t expectedEnd = ((cycle + 1) * rate + 7999) / 8000;
            const auto frames = cadence.CurrentCycleDataFrames();
            EXPECT_EQ(total + frames, expectedEnd);
            EXPECT_TRUE(cadence.CurrentCycleIsData());
            total += frames;
            cadence.AdvanceCycle();
        }
        EXPECT_EQ(total, uint64_t(rate) * 10);
        cadence.Reset();
        EXPECT_EQ(cadence.CurrentCycleDataFrames(), (rate + 7999) / 8000);
    }
}

TEST(NonBlockingCadenceTests, LinuxFractionalStartupVectors) {
    NonBlockingCadence cadence;
    ASSERT_TRUE(cadence.Configure(44100));
    for (const auto frames : {6, 6, 5, 6, 5, 6, 5}) {
        EXPECT_EQ(cadence.CurrentCycleDataFrames(), frames);
        cadence.AdvanceCycle();
    }
    ASSERT_TRUE(cadence.Configure(88200));
    EXPECT_EQ(cadence.CurrentCycleDataFrames(), 12);
    cadence.AdvanceCycle();
    for (unsigned i = 0; i < 39; ++i) {
        EXPECT_EQ(cadence.CurrentCycleDataFrames(), 11);
        cadence.AdvanceCycle();
    }
    EXPECT_EQ(cadence.CurrentCycleDataFrames(), 12);
}

TEST(NonBlockingCadenceTests, InvalidConfigurationDisablesPriorCadence) {
    NonBlockingCadence cadence;
    ASSERT_TRUE(cadence.Configure(192000));
    EXPECT_FALSE(cadence.Configure(12345));
    EXPECT_FALSE(cadence.CurrentCycleIsData());
    EXPECT_EQ(cadence.CurrentCycleDataFrames(), 0);
    cadence.AdvanceCycle();
    EXPECT_EQ(cadence.TotalCycles(), 0);
}
