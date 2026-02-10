// NonBlockingCadenceTests.cpp
// ASFW - Isoch Encoding Tests
//
// Tests for 48 kHz non-blocking cadence pattern.
//

#include <gtest/gtest.h>
#include "Isoch/Encoding/NonBlockingCadence48k.hpp"

using namespace ASFW::Encoding;

TEST(NonBlockingCadenceTests, ConstantsAreCorrect) {
    EXPECT_EQ(kNonBlockingSamplesPerPacket48k, 6u);
    EXPECT_EQ(kNonBlockingDataPacketsPer8Cycles, 8u);
    EXPECT_EQ(kNonBlockingNoDataPacketsPer8Cycles, 0u);
}

TEST(NonBlockingCadenceTests, AlwaysDataEveryCycle) {
    NonBlockingCadence48k cadence;
    for (int i = 0; i < 16; ++i) {
        SCOPED_TRACE("Cycle " + std::to_string(i));
        EXPECT_TRUE(cadence.isDataPacket());
        EXPECT_EQ(cadence.samplesThisCycle(), 6u);
        cadence.advance();
    }
}

TEST(NonBlockingCadenceTests, Produces48kSamplesPerSecond) {
    NonBlockingCadence48k cadence;
    uint32_t totalSamples = 0;

    for (int i = 0; i < 8000; ++i) {
        totalSamples += cadence.samplesThisCycle();
        cadence.advance();
    }

    EXPECT_EQ(totalSamples, 48000u);
}

TEST(NonBlockingCadenceTests, ResetRestoresInitialState) {
    NonBlockingCadence48k cadence;
    cadence.advanceBy(123);
    EXPECT_GT(cadence.getTotalCycles(), 0u);

    cadence.reset();
    EXPECT_EQ(cadence.getTotalCycles(), 0u);
    EXPECT_EQ(cadence.getCycleIndex(), 0u);
    EXPECT_TRUE(cadence.isDataPacket());
}
