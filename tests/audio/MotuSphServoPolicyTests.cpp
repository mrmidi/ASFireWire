// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// The production configuration of the MOTU SPH regulator at each of the six
// MOTU rates: exact nominal step, step limits that bracket it with the same
// relative width as at 48 kHz, and a decision interval of about 10.7 ms in
// whole DATA packets.

#include "Audio/Wire/AMDTP/AmdtpRateGeometry.hpp"
#include "Audio/Wire/MOTU/MotuSphServoPolicy.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using ASFW::Encoding::Motu::kSphServoMaximumTrackingRateAt48k;
using ASFW::Encoding::Motu::kSphServoMinimumTrackingRateAt48k;
using ASFW::Encoding::Motu::MotuSphClockServo;
using ASFW::Encoding::Motu::MotuSphServoConfigForRate;
using ASFW::Encoding::Motu::SphServoDecisionFrames;
using ASFW::Encoding::Motu::SphServoFramesPerDataPacket;

constexpr int64_t kTicksPerSecond = 24576000;

struct RateCase final {
    uint32_t rateHz;
    uint32_t framesPerPacket;
    uint64_t decisionFrames;
};

// 512 frames at 48 kHz is 10.667 ms. The 44.1 kHz family lands between whole
// packets (58.8 of them), so it rounds to 59: 10.70 ms.
constexpr std::array<RateCase, 6> kCases{{
    {44100, 8, 472},
    {48000, 8, 512},
    {88200, 16, 944},
    {96000, 16, 1024},
    {176400, 32, 1888},
    {192000, 32, 2048},
}};

constexpr int64_t StepQ32AtRate(uint32_t rateHz) {
    return static_cast<int64_t>((static_cast<__int128>(kTicksPerSecond) << 32) / rateHz);
}

TEST(MotuSphServoPolicyTests, FramesPerDataPacketFollowTheBlockingCadence) {
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.rateHz);
        EXPECT_EQ(SphServoFramesPerDataPacket(c.rateHz), c.framesPerPacket);
        // The same geometry the packetizer configures its blocking cadence from.
        const auto geometry = ASFW::Encoding::AmdtpRateGeometryForSampleRate(c.rateHz);
        ASSERT_TRUE(geometry.has_value());
        EXPECT_EQ(SphServoFramesPerDataPacket(c.rateHz), geometry->sytIntervalFrames);
    }
}

TEST(MotuSphServoPolicyTests, ConfiguresTheServoAtEveryMotuRate) {
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.rateHz);
        const auto config = MotuSphServoConfigForRate(c.rateHz);
        ASSERT_TRUE(config.has_value());
        EXPECT_EQ(config->sampleRateHz, c.rateHz);
        EXPECT_EQ(config->phaseGainShift, 2u);
        EXPECT_EQ(config->acquisitionPhaseGainShift, 0u);
        // The setpoint is a per-model value; the rate policy leaves it alone.
        EXPECT_EQ(config->absolutePhaseSetpointTicks, 0);

        MotuSphClockServo servo{};
        EXPECT_TRUE(servo.Configure(*config));
        EXPECT_EQ(servo.NominalStep(), StepQ32AtRate(c.rateHz));
    }
}

TEST(MotuSphServoPolicyTests, NominalStepIsExactAtEveryRate) {
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.rateHz);
        const __int128 step = MotuSphClockServo::NominalStepQ32(c.rateHz);
        const __int128 secondQ32 = static_cast<__int128>(kTicksPerSecond) << 32;
        EXPECT_LE(step * c.rateHz, secondQ32);
        EXPECT_GT((step + 1) * c.rateHz, secondQ32);
    }
    // The 48 kHz family is integral in ticks.
    EXPECT_EQ(MotuSphClockServo::NominalStepQ32(48000), int64_t{512} << 32);
    EXPECT_EQ(MotuSphClockServo::NominalStepQ32(96000), int64_t{256} << 32);
    EXPECT_EQ(MotuSphClockServo::NominalStepQ32(192000), int64_t{128} << 32);
}

TEST(MotuSphServoPolicyTests, StepLimitsAreOrderedAndBracketTheNominalStep) {
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.rateHz);
        const auto config = MotuSphServoConfigForRate(c.rateHz);
        ASSERT_TRUE(config.has_value());
        const int64_t nominal = MotuSphClockServo::NominalStepQ32(c.rateHz);
        EXPECT_GT(config->minimumStepQ32, 0);
        EXPECT_LT(config->minimumStepQ32, nominal);
        EXPECT_GT(config->maximumStepQ32, nominal);
    }
}

TEST(MotuSphServoPolicyTests, StepLimitsAtFortyEightKilohertzAreTheTrackingRatesThemselves) {
    const auto config = MotuSphServoConfigForRate(48000);
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->minimumStepQ32, StepQ32AtRate(kSphServoMaximumTrackingRateAt48k));
    EXPECT_EQ(config->maximumStepQ32, StepQ32AtRate(kSphServoMinimumTrackingRateAt48k));
    EXPECT_EQ(kSphServoMaximumTrackingRateAt48k, 50160u);
    EXPECT_EQ(kSphServoMinimumTrackingRateAt48k, 41895u);
}

TEST(MotuSphServoPolicyTests, StepLimitsKeepTheSameRelativeWidthAtEveryRate) {
    // nominal / limit is the tracking rate over 48 kHz at every rate.
    const double upper = static_cast<double>(kSphServoMaximumTrackingRateAt48k) / 48000.0;
    const double lower = static_cast<double>(kSphServoMinimumTrackingRateAt48k) / 48000.0;
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.rateHz);
        const auto config = MotuSphServoConfigForRate(c.rateHz);
        ASSERT_TRUE(config.has_value());
        const double nominal = static_cast<double>(MotuSphClockServo::NominalStepQ32(c.rateHz));
        EXPECT_NEAR(nominal / static_cast<double>(config->minimumStepQ32), upper, 1e-9);
        EXPECT_NEAR(nominal / static_cast<double>(config->maximumStepQ32), lower, 1e-9);
    }
}

TEST(MotuSphServoPolicyTests, DecisionIntervalIsAboutElevenMillisecondsInWholePackets) {
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.rateHz);
        const auto config = MotuSphServoConfigForRate(c.rateHz);
        ASSERT_TRUE(config.has_value());
        EXPECT_EQ(config->phaseCorrectionHorizonFrames, c.decisionFrames);
        EXPECT_EQ(SphServoDecisionFrames(c.rateHz), c.decisionFrames);
        EXPECT_EQ(c.decisionFrames % c.framesPerPacket, 0u);

        const double milliseconds =
            1000.0 * static_cast<double>(c.decisionFrames) / static_cast<double>(c.rateHz);
        EXPECT_GT(milliseconds, 10.6);
        EXPECT_LT(milliseconds, 10.8);
    }
}

TEST(MotuSphServoPolicyTests, RefusesRatesMotuDoesNotCarry) {
    for (const uint32_t rate : {0u, 22050u, 32000u, 384000u}) {
        SCOPED_TRACE(rate);
        EXPECT_FALSE(MotuSphServoConfigForRate(rate).has_value());
        EXPECT_EQ(SphServoFramesPerDataPacket(rate), 0u);
        EXPECT_EQ(SphServoDecisionFrames(rate), 0u);
    }
}

} // namespace
