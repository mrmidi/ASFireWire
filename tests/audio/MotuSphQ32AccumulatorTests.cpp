// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// The fractional transmit SPH clock: exact rational period at every MOTU rate,
// a one-second modular phase, and an applied-correction total that the SPH
// regulator reads back as its actuator position.

#include "Audio/Wire/MOTU/MotuSphQ32Accumulator.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using ASFW::Encoding::Motu::MotuSphQ32Accumulator;
using ASFW::Encoding::Motu::SphFromTick;

constexpr int64_t kOneQ32 = MotuSphQ32Accumulator::kOneQ32;
constexpr int64_t kSecond = ASFW::Encoding::Motu::kTicksPerSecond;
constexpr std::array<uint32_t, 6> kRates{44100, 48000, 88200, 96000, 176400, 192000};

// The phase is modular, so "how far behind" is measured the short way round.
int64_t Lag(int64_t expected, int64_t actual, int64_t domain) {
    return ((expected - actual) % domain + domain) % domain;
}

// A four-second run crosses the one-second wrap four times. What has to hold
// across the crossings is the position on the circle, not a monotonic total.
TEST(MotuSphQ32AccumulatorTests, KeepsFractionalTimeAtAllSixRates) {
    constexpr int64_t kSeconds = 4;
    for (const uint32_t rate : kRates) {
        SCOPED_TRACE(rate);
        MotuSphQ32Accumulator clock{};
        ASSERT_TRUE(clock.Configure(rate));
        ASSERT_TRUE(clock.SeedOnce(0));
        EXPECT_EQ(clock.StepQ32(), MotuSphQ32Accumulator::NominalStepQ32ForRate(rate));

        const uint32_t frames = rate * kSeconds;
        for (uint32_t frame = 0; frame < frames; ++frame) {
            clock.Advance();
        }

        // The step is floor(Q32.32), so the truncation stays under one Q32
        // unit per frame: within one integer tick after four seconds even for
        // the 44.1 kHz family.
        EXPECT_LT(Lag(0, clock.PhaseQ32(), MotuSphQ32Accumulator::kTickDomainQ32),
                  static_cast<int64_t>(frames));
        EXPECT_LE(Lag(0, clock.CurrentTicks(), kSecond), 1);

        // Wrapping is not steering: a free-running clock reports no applied
        // correction however often it crossed a second boundary.
        EXPECT_EQ(clock.AppliedCorrectionQ32(), 0);
    }
}

TEST(MotuSphQ32AccumulatorTests, NominalStepIsTheFloorOfTheExactPeriod) {
    for (const uint32_t rate : kRates) {
        SCOPED_TRACE(rate);
        const __int128 step = MotuSphQ32Accumulator::NominalStepQ32ForRate(rate);
        const __int128 secondQ32 = static_cast<__int128>(kSecond) << 32;
        EXPECT_LE(step * rate, secondQ32);
        EXPECT_GT((step + 1) * rate, secondQ32);
    }
    EXPECT_EQ(MotuSphQ32Accumulator::NominalStepQ32ForRate(48000), 512 * kOneQ32);
}

TEST(MotuSphQ32AccumulatorTests, RejectsUnsupportedRateAndInvalidSteps) {
    MotuSphQ32Accumulator clock{};
    EXPECT_FALSE(clock.Configure(32000));
    EXPECT_FALSE(clock.Configure(0));
    EXPECT_FALSE(clock.SetStepQ32(1));
    EXPECT_FALSE(clock.SeedOnce(0));
    EXPECT_FALSE(clock.ApplyPhaseCorrectionQ32(kOneQ32));

    ASSERT_TRUE(clock.Configure(48000));
    EXPECT_FALSE(clock.SetStepQ32(0));
    EXPECT_FALSE(clock.SetStepQ32(-1));
    EXPECT_FALSE(clock.SetStepQ32(MotuSphQ32Accumulator::kTickDomainQ32));
    EXPECT_EQ(clock.StepQ32(), 512 * kOneQ32);
}

TEST(MotuSphQ32AccumulatorTests, SeedsOncePerResetOnTheOneSecondTimeline) {
    MotuSphQ32Accumulator clock{};
    ASSERT_TRUE(clock.Configure(48000));
    EXPECT_EQ(clock.CurrentSph(), 0u);

    // A transmit time on a longer timeline wraps onto the one-second one.
    ASSERT_TRUE(clock.SeedOnce(3 * kSecond + 9216));
    EXPECT_EQ(clock.CurrentTicks(), 9216u);
    EXPECT_EQ(clock.CurrentSph(), SphFromTick(9216));
    EXPECT_FALSE(clock.SeedOnce(0));
    EXPECT_EQ(clock.CurrentTicks(), 9216u);

    clock.Reset();
    ASSERT_TRUE(clock.SeedOnce(-1));
    EXPECT_EQ(clock.CurrentTicks(), static_cast<uint32_t>(kSecond - 1));
}

TEST(MotuSphQ32AccumulatorTests, AppliesPreSeedRepairAndAccountsForIt) {
    MotuSphQ32Accumulator clock{};
    ASSERT_TRUE(clock.Configure(48000));

    constexpr int64_t correctionQ32 = -2 * kOneQ32;
    ASSERT_TRUE(clock.ApplyPhaseCorrectionQ32(correctionQ32));
    EXPECT_EQ(clock.AppliedCorrectionQ32(), correctionQ32);
    ASSERT_TRUE(clock.SeedOnce(1));
    EXPECT_EQ(clock.CurrentTicks(), static_cast<uint32_t>(kSecond - 1));

    clock.Reset();
    EXPECT_EQ(clock.AppliedCorrectionQ32(), 0);
    ASSERT_TRUE(clock.SeedOnce(1));
    EXPECT_EQ(clock.CurrentTicks(), 1u);
}

TEST(MotuSphQ32AccumulatorTests, RepairAfterTheSeedMovesThePhaseAndIsCounted) {
    MotuSphQ32Accumulator clock{};
    ASSERT_TRUE(clock.Configure(48000));
    ASSERT_TRUE(clock.SeedOnce(1000));

    ASSERT_TRUE(clock.ApplyPhaseCorrectionQ32(-1500 * kOneQ32));
    EXPECT_EQ(clock.CurrentTicks(), static_cast<uint32_t>(kSecond - 500));
    EXPECT_EQ(clock.AppliedCorrectionQ32(), -1500 * kOneQ32);
}

// Only stamped frames are executed correction: before the seed nothing is
// stamped, and afterwards each block adds exactly (step - nominal).
TEST(MotuSphQ32AccumulatorTests, AppliedCorrectionCountsOnlyStampedFramesAndResetClearsIt) {
    MotuSphQ32Accumulator clock{};
    ASSERT_TRUE(clock.Configure(48000));
    const int64_t step = 512 * kOneQ32 + 12345;
    ASSERT_TRUE(clock.SetStepQ32(step));

    clock.Advance();  // not seeded yet
    EXPECT_EQ(clock.AppliedCorrectionQ32(), 0);

    ASSERT_TRUE(clock.SeedOnce(0));
    for (int frame = 0; frame < 8; ++frame) {
        clock.Advance();
    }
    EXPECT_EQ(clock.AppliedCorrectionQ32(), 8 * (step - 512 * kOneQ32));

    clock.Reset();
    EXPECT_EQ(clock.StepQ32(), 512 * kOneQ32);
    EXPECT_EQ(clock.AppliedCorrectionQ32(), 0);
    EXPECT_FALSE(clock.IsSeeded());
}

} // namespace
