// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FW-182/FW-184: the timing/HAL geometry resolver and the pure maths it is
// built from. These test the architecture as well as the arithmetic: the
// resolver must agree with the compile-time HAL geometry the driver allocates,
// must reproduce main's shipped 48 kHz values, and must not hide a 48 kHz
// assumption at other rates.

#include "Audio/DriverKit/Config/TimingLadder.hpp"
#include "Audio/Runtime/ResolvedTimingGeometry.hpp"
#include "Audio/Wire/AMDTP/AmdtpCadence.hpp"
#include "Audio/Wire/AMDTP/AmdtpRateGeometry.hpp"
#include "Audio/Wire/AMDTP/AmdtpTransferDelay.hpp"
#include "Shared/Isoch/AudioTimingGeometry.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>

namespace {

using namespace ASFW::Audio::Runtime;
using ASFW::Encoding::AmdtpRateGeometryForSampleRate;
using ASFW::Encoding::StreamMode;
using Geometry = ASFW::IsochTransport::AudioTimingGeometry;
namespace Ladder = ASFW::Isoch::Audio::TimingLadder;

constexpr std::array<uint32_t, 7> kAmdtpRates = {32000, 44100, 48000, 88200,
                                                 96000, 176400, 192000};

// A profile shaped like the Saffire ladder at 48 kHz.
constexpr DeviceTimingPolicy kSaffire48{29, 29, 48, 128};

TEST(TimingGeometryTests, Shipped48kGeometryIsReproducedExactly) {
    const auto resolved = ResolveTimingGeometry(48000, StreamMode::kBlocking, kSaffire48);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->sampleRateHz, 48000U);
    EXPECT_EQ(resolved->fdf, 2U);
    EXPECT_EQ(resolved->sytIntervalFrames, 8U);
    EXPECT_EQ(resolved->frameRingFrames, 1536U);
    EXPECT_EQ(resolved->zeroTimestampPeriodFrames, 1536U);
    EXPECT_EQ(resolved->clientIoBudgetFrames, 512U);
    EXPECT_EQ(resolved->outputLatencyFrames, 29U);
    EXPECT_EQ(resolved->inputLatencyFrames, 29U);
    EXPECT_EQ(resolved->outputSafetyOffsetFrames, 48U);
    EXPECT_EQ(resolved->inputSafetyOffsetFrames, 128U);
    EXPECT_EQ(resolved->inputSafetyFloorFrames, CompletionBatchFrames(*AmdtpRateGeometryForSampleRate(48000)));
    EXPECT_EQ(resolved->rxTransferDelayTicks, 12800U);
    EXPECT_EQ(resolved->txTransferDelayTicks, 12800U);
}

// COMPAT agreement (ownership G-05..G-07): consumers that still read the
// compile-time constants must see the same numbers the resolver publishes. When
// HalBufferProfileForRate becomes rate-dependent this test fails on purpose --
// those consumers must then read the resolved value.
TEST(TimingGeometryTests, ResolverAgreesWithCompileTimeHalGeometryAtEveryRate) {
    for (const uint32_t rate : kAmdtpRates) {
        SCOPED_TRACE(rate);
        const auto resolved = ResolveTimingGeometry(rate, StreamMode::kBlocking, kSaffire48);
        ASSERT_TRUE(resolved.has_value());
        EXPECT_EQ(resolved->frameRingFrames, Geometry::kFrameRingFrames);
        EXPECT_EQ(resolved->zeroTimestampPeriodFrames, Geometry::kHalZeroTimestampPeriodFrames);
        EXPECT_EQ(resolved->clientIoBudgetFrames, Geometry::kHalIoPeriodFrames);
    }
}

TEST(TimingGeometryTests, WireFactsAreCopiedNotRederived) {
    for (const uint32_t rate : kAmdtpRates) {
        SCOPED_TRACE(rate);
        const auto wire = AmdtpRateGeometryForSampleRate(rate);
        const auto resolved = ResolveTimingGeometry(rate, StreamMode::kBlocking, kSaffire48);
        ASSERT_TRUE(wire && resolved);
        EXPECT_EQ(resolved->fdf, wire->fdf);
        EXPECT_EQ(resolved->sytIntervalFrames, wire->sytIntervalFrames);
    }
}

TEST(TimingGeometryTests, UnknownRateIsAnErrorNotA48kFallback) {
    for (const uint32_t rate : {0U, 22050U, 47999U, 384000U}) {
        const auto resolved = ResolveTimingGeometry(rate, StreamMode::kBlocking, kSaffire48);
        ASSERT_FALSE(resolved.has_value()) << rate;
        EXPECT_EQ(resolved.error(), TimingGeometryError::kUnsupportedSampleRate);
    }
}

TEST(TimingGeometryTests, InvalidSafetyIsRejected) {
    DeviceTimingPolicy zeroOut = kSaffire48;
    zeroOut.outputSafetyOffsetFrames = 0;
    EXPECT_EQ(ResolveTimingGeometry(48000, StreamMode::kBlocking, zeroOut).error(),
              TimingGeometryError::kInvalidSafetyOffset);

    DeviceTimingPolicy hugeIn = kSaffire48;
    hugeIn.inputSafetyOffsetFrames = Geometry::kFrameRingFrames;
    EXPECT_EQ(ResolveTimingGeometry(48000, StreamMode::kBlocking, hugeIn).error(),
              TimingGeometryError::kInvalidSafetyOffset);
}

// D3: input safety is the profile's value floored at one completion batch --
// no jitter term, no grid alignment, so a calibrated value stands exactly.
TEST(TimingGeometryTests, InputSafetyIsTheProfileValueFlooredAtOneBatch) {
    for (const uint32_t rate : kAmdtpRates) {
        SCOPED_TRACE(rate);
        const auto wire = *AmdtpRateGeometryForSampleRate(rate);
        const uint32_t batch = CompletionBatchFrames(wire);
        EXPECT_EQ(ResolveInputSafetyFrames(0, wire), batch);
        EXPECT_EQ(ResolveInputSafetyFrames(batch - 1, wire), batch);
        EXPECT_EQ(ResolveInputSafetyFrames(batch + 1, wire), batch + 1);  // not aligned
    }
    // The Saffire calibration (10 packets = 80 frames at 48 kHz) is not raised.
    EXPECT_EQ(ResolveInputSafetyFrames(80, *AmdtpRateGeometryForSampleRate(48000)), 80U);
}

TEST(TimingGeometryTests, CompletionBatchFollowsTheRate) {
    // Most DATA packets one completion group can carry, times the SYT interval.
    const uint32_t group = Geometry::kTimingGroupPackets;
    for (const uint32_t rate : kAmdtpRates) {
        SCOPED_TRACE(rate);
        const auto wire = *AmdtpRateGeometryForSampleRate(rate);
        EXPECT_EQ(CompletionBatchFrames(wire),
                  ASFW::Encoding::MaxBlockingDataPacketsInCycles(group, wire) *
                      wire.sytIntervalFrames);
    }
}

// The completion-batch bound is checked against the production cadence itself:
// for every rate, slide a completion-group window over many cadence periods and count
// DATA decisions. The bound must be the true maximum (tight), not just safe.
TEST(TimingGeometryTests, MaxDataPacketsBoundMatchesTheProductionCadence) {
    for (const uint32_t rate : kAmdtpRates) {
        SCOPED_TRACE(rate);
        const auto wire = *AmdtpRateGeometryForSampleRate(rate);
        ASFW::Protocols::Audio::AMDTP::RationalBlockingCadence cadence;
        ASSERT_TRUE(cadence.Configure(rate, static_cast<uint8_t>(wire.sytIntervalFrames)));
        std::deque<bool> window;
        uint32_t inWindow = 0;
        uint32_t observedMax = 0;
        constexpr uint32_t kCycles = 6;
        for (uint32_t cycle = 0; cycle < 8000 * 2; ++cycle) {
            const bool isData = cadence.CurrentDecision().isData;
            cadence.AdvanceCycle();
            window.push_back(isData);
            inWindow += isData ? 1U : 0U;
            if (window.size() > kCycles) {
                inWindow -= window.front() ? 1U : 0U;
                window.pop_front();
            }
            if (window.size() == kCycles) {
                observedMax = std::max(observedMax, inWindow);
            }
        }
        EXPECT_EQ(ASFW::Encoding::MaxBlockingDataPacketsInCycles(kCycles, wire), observedMax);
    }
}

TEST(TimingGeometryTests, TransferDelayIsTheBlockingFormulaAtEveryRate) {
    struct Row {
        uint32_t rate;
        uint32_t blocking;
    };
    constexpr Row kRows[] = {{32000, 14848}, {44100, 13162}, {48000, 12800}, {88200, 13162},
                             {96000, 12800}, {176400, 13162}, {192000, 12800}};
    for (const auto& row : kRows) {
        SCOPED_TRACE(row.rate);
        const auto wire = *AmdtpRateGeometryForSampleRate(row.rate);
        EXPECT_EQ(ASFW::Encoding::AmdtpTransferDelayTicks(wire, StreamMode::kBlocking),
                  row.blocking);
        EXPECT_EQ(ASFW::Encoding::AmdtpTransferDelayTicks(wire, StreamMode::kNonBlocking),
                  8704U);
        // D1: the blocking formula is applied to every stream (as midi does),
        // whatever the stream mode.
        EXPECT_EQ(AppliedTransferDelayTicks(wire), row.blocking);
        const auto resolved = ResolveTimingGeometry(row.rate, StreamMode::kNonBlocking, kSaffire48);
        ASSERT_TRUE(resolved.has_value());
        EXPECT_EQ(resolved->rxTransferDelayTicks, row.blocking);
        EXPECT_EQ(resolved->txTransferDelayTicks, row.blocking);
    }
}

// The ladder helper must reproduce the per-profile copies it replaces, at every
// AMDTP rate. The expressions below are the legacy profile bodies verbatim.
TEST(TimingGeometryTests, LadderHelperReproducesLegacyProfileLadders) {
    const auto legacySafety = [](uint32_t delayPackets, double sampleRate) {
        uint32_t framesPerPacket = 8;
        uint32_t rateAddend = 0;
        if (sampleRate > 96000.0) {
            framesPerPacket = 32;
            rateAddend = 4;
        } else if (sampleRate > 48000.0) {
            framesPerPacket = 16;
            rateAddend = 2;
        }
        return (delayPackets + rateAddend) * framesPerPacket;
    };
    const auto legacyWeissFpp = [](double sampleRate) {
        return sampleRate > 48000.0 ? (sampleRate > 96000.0 ? 32U : 16U) : 8U;
    };
    const auto legacyLatency = [](double sampleRate) {
        if (sampleRate > 96000.0) return 119U;
        if (sampleRate > 48000.0) return 59U;
        return 29U;
    };
    for (const uint32_t rate : kAmdtpRates) {
        const double r = rate;
        SCOPED_TRACE(rate);
        EXPECT_EQ(Ladder::SafetyOffsetFrames(6, r, Ladder::RateAddend::kPerTier),
                  legacySafety(6, r));
        EXPECT_EQ(Ladder::SafetyOffsetFrames(16, r, Ladder::RateAddend::kPerTier),
                  legacySafety(16, r));
        EXPECT_EQ(Ladder::SafetyOffsetFrames(6, r, Ladder::RateAddend::kNone),
                  6U * legacyWeissFpp(r));
        EXPECT_EQ(Ladder::SafetyOffsetFrames(16, r, Ladder::RateAddend::kNone),
                  16U * legacyWeissFpp(r));
        EXPECT_EQ(Ladder::ReportedLatencyFrames(r), legacyLatency(r));
    }
    // Outside the wire table the helper refuses rather than guessing 1x.
    EXPECT_EQ(Ladder::FramesPerPacket(22050.0), 0U);
    EXPECT_EQ(Ladder::ReportedLatencyFrames(22050.0), 0U);
}

TEST(TimingGeometryTests, LiveCompatibilityRequiresUnchangedRingAndPeriod) {
    const auto at48 = *ResolveTimingGeometry(48000, StreamMode::kBlocking, kSaffire48);
    const auto at96 = *ResolveTimingGeometry(96000, StreamMode::kBlocking, kSaffire48);
    EXPECT_TRUE(IsLiveCompatible(at48, at96));
    auto grown = at96;
    grown.zeroTimestampPeriodFrames *= 2;
    EXPECT_FALSE(IsLiveCompatible(at48, grown));
}

} // namespace
