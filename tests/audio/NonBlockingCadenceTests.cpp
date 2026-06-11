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
