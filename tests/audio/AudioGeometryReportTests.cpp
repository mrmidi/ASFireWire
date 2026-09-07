// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// The Audio Geometry panel renders these numbers verbatim, so an error here is
// an error an operator reads as fact and tunes against. The point of deriving
// the report in Audio/Shared rather than at the user-client fill site is that
// this suite can reach it.

#include "Audio/Shared/AudioGeometryReport.hpp"

// The report itself does not need the tuning type; this suite does, because
// it checks the published bounds against what ValidateTuning enforces.
#include "Audio/Shared/AudioRuntimeTuning.hpp"

#include <gtest/gtest.h>

namespace {
using namespace ASFW::Audio::Shared;

// The cadence scan is the one piece of real arithmetic in the report; the rest
// is a copy. Check it against hand-computed windows rather than against the
// same expression written twice.
TEST(AudioGeometryReport, CadenceWindowCountsEveryStartingPhase) {
    // D,D,D,N: a six-packet window covers one or two idle slots.
    const auto six = DataPacketsInWindow(6, 4, 3);
    EXPECT_EQ(six.minimum, 4U);
    EXPECT_EQ(six.maximum, 5U);

    // A window that is a whole number of blocks has no phase dependence.
    const auto eight = DataPacketsInWindow(8, 4, 3);
    EXPECT_EQ(eight.minimum, 6U);
    EXPECT_EQ(eight.maximum, 6U);

    // A cadence with no idle slot cannot swing.
    const auto dense = DataPacketsInWindow(6, 4, 4);
    EXPECT_EQ(dense.minimum, 6U);
    EXPECT_EQ(dense.maximum, 6U);

    // Degenerate inputs return zero rather than dividing by the block size.
    EXPECT_EQ(DataPacketsInWindow(6, 0, 3).maximum, 0U);
    EXPECT_EQ(DataPacketsInWindow(0, 4, 3).maximum, 0U);
    EXPECT_EQ(DataPacketsInWindow(6, 4, 5).maximum, 0U);
}

// The interrupt cadence is what the user asked the panel to show. It is a
// property of the completion group and the 125 us cycle only -- it must not
// move with the sample rate, and this is the assertion that says so.
TEST(AudioGeometryReport, InterruptCadenceIsRateIndependent) {
    for (uint32_t rate : {48'000U, 96'000U, 192'000U}) {
        const auto g = DeriveGeometryReport(rate);
        EXPECT_EQ(g.txInterruptIntervalMicroseconds, 750U) << "rate " << rate;
        EXPECT_EQ(g.rxInterruptIntervalMicroseconds, 750U) << "rate " << rate;
        EXPECT_EQ(g.txPacketsPerGroup, 6U);
        EXPECT_EQ(g.rxPacketsPerGroup, 6U);
        // 8000 / 6 = 1333.33 per second. The interval is the exact quantity;
        // an integer rate would round away 0.33 and misreport the cadence by
        // ~2.5 interrupts a second in each direction.
        EXPECT_EQ(1'000'000U / g.txInterruptIntervalMicroseconds, 1'333U);
    }
}

// Frames per interrupt DO move with the rate, and the swing between the best
// and worst cadence phase is the number that sizes a capture margin.
TEST(AudioGeometryReport, FramesPerInterruptScaleWithTheRate) {
    struct Row {
        uint32_t rate, framesPerDataPacket, nominal, minimum, maximum;
    };
    for (const Row row : {Row{48'000, 8, 36, 32, 40},
                          Row{96'000, 16, 72, 64, 80},
                          Row{192'000, 32, 144, 128, 160}}) {
        const auto g = DeriveGeometryReport(row.rate);
        EXPECT_EQ(g.framesPerDataPacket, row.framesPerDataPacket);
        EXPECT_EQ(g.framesPerCompletionGroupRx, row.nominal) << row.rate;
        EXPECT_EQ(g.framesPerCompletionGroupTx, row.nominal) << row.rate;
        EXPECT_EQ(g.minFramesPerRxInterrupt, row.minimum) << row.rate;
        EXPECT_EQ(g.maxFramesPerRxInterrupt, row.maximum) << row.rate;
        // The nominal must sit inside the observable swing, or it is not an
        // average of anything.
        EXPECT_GE(row.nominal, row.minimum);
        EXPECT_LE(row.nominal, row.maximum);
        // One group is one interrupt interval of audio.
        EXPECT_EQ(g.framesPerCompletionGroupRx,
                  row.rate * g.rxInterruptIntervalMicroseconds / 1'000'000U);
    }
}

// A rate the driver does not run must not be answered with the 48 kHz numbers.
// The panel distinguishes "not streaming" from "6 frames a packet" by these
// zeroes, and a 44.1 kHz answer here is exactly the truncation that made the
// old framesPerPacketAverage field report 5.
TEST(AudioGeometryReport, UnsupportedRatesReportNoCadenceAtAll) {
    for (uint32_t rate : {0U, 44'100U, 88'200U, 176'400U}) {
        const auto g = DeriveGeometryReport(rate);
        EXPECT_EQ(g.framesPerDataPacket, 0U) << "rate " << rate;
        EXPECT_EQ(g.framesPerCompletionGroupRx, 0U) << "rate " << rate;
        EXPECT_EQ(g.minFramesPerRxInterrupt, 0U) << "rate " << rate;
        EXPECT_EQ(g.maxFramesPerRxInterrupt, 0U) << "rate " << rate;
        EXPECT_EQ(g.cadenceBlockFrames, 0U) << "rate " << rate;
        EXPECT_EQ(g.txSafetyOffsetPolicyFrames, 0U) << "rate " << rate;
        EXPECT_EQ(g.reportedLatencyPolicyFrames, 0U) << "rate " << rate;

        // Structure that does not depend on the rate stays reportable, so the
        // panel can show the rings and the tuning bounds before IO starts.
        EXPECT_EQ(g.txDispatchSlackFloorPackets,
                  AudioTimingGeometry::kTxDispatchSlackFloorPackets);
        EXPECT_EQ(g.rxHardwareRingPackets,
                  AudioTimingGeometry::kRxHardwareRingPackets);
        EXPECT_EQ(g.txInterruptIntervalMicroseconds, 750U);
    }
}

// The floor and the default are published so the app stops hardcoding 72. If
// they ever disagree with what ValidateTuning enforces, the panel would offer
// a value the driver warns about as though it were safe.
TEST(AudioGeometryReport, PublishedTuningBoundsMatchWhatValidationEnforces) {
    const auto g = DeriveGeometryReport(48'000);
    EXPECT_EQ(g.txDispatchSlackDefaultPackets,
              AudioRuntimeTuning{}.txDispatchSlackPackets);

    AudioRuntimeTuning onFloor{};
    onFloor.txDispatchSlackPackets = g.txDispatchSlackFloorPackets;
    EXPECT_FALSE(ValidateTuning(onFloor).Has(
        TuningWarning::kDispatchSlackBelowAssertedFloor));

    AudioRuntimeTuning belowFloor{};
    belowFloor.txDispatchSlackPackets = g.txDispatchSlackFloorPackets - 1;
    EXPECT_TRUE(ValidateTuning(belowFloor).Has(
        TuningWarning::kDispatchSlackBelowAssertedFloor));
}

// The RTL lattice quantum. Measured round-trip latency lands on
// base + k * txRingLapFrames, and the committed-margin minimum moves in the
// same step, because the producer's phase relative to the live OHCI CommandPtr
// repeats once per ring traversal. The panel names it so a jump of exactly one
// lap is recognised as a lap.
TEST(AudioGeometryReport, RingLapIsTheLatticeQuantum) {
    EXPECT_EQ(DeriveGeometryReport(48'000).txRingLapFrames, 288U);
    EXPECT_EQ(DeriveGeometryReport(96'000).txRingLapFrames, 576U);
    EXPECT_EQ(DeriveGeometryReport(192'000).txRingLapFrames, 1152U);
    // It is the ring expressed in frames, so it must agree with the ring
    // expressed in completion groups at every rate.
    for (uint32_t rate : {48'000U, 96'000U, 192'000U}) {
        const auto g = DeriveGeometryReport(rate);
        EXPECT_EQ(g.txRingLapFrames,
                  g.framesPerCompletionGroupTx *
                      (g.txHardwareRingPackets / g.txPacketsPerGroup))
            << "rate " << rate;
    }
    // No rate, no lattice: the quantum is a frame count and frames need a rate.
    EXPECT_EQ(DeriveGeometryReport(0).txRingLapFrames, 0U);
    EXPECT_EQ(DeriveGeometryReport(44'100).txRingLapFrames, 0U);
}

// The panel prints packet counts as milliseconds. That conversion is only
// legitimate because a cycle is 125 us regardless of rate; pin the grid it
// relies on.
TEST(AudioGeometryReport, RingDepthsConvertToTimeThroughTheCycleGrid) {
    const auto g = DeriveGeometryReport(48'000);
    EXPECT_EQ(g.isochCyclesPerSecond * g.microsecondsPerIsochCycle, 1'000'000U);

    // 48 packets of hardware ring = 6 ms; 168 shared slots = 21 ms; the RX
    // descriptor ring of 504 packets = 63 ms.
    EXPECT_EQ(g.txHardwareRingPackets * g.microsecondsPerIsochCycle, 6'000U);
    EXPECT_EQ(g.txSharedSlotPackets * g.microsecondsPerIsochCycle, 21'000U);
    EXPECT_EQ(g.rxHardwareRingPackets * g.microsecondsPerIsochCycle, 63'000U);

    // Every ring must be a whole number of interrupt groups, or a completion
    // would straddle a wrap.
    EXPECT_EQ(g.txHardwareRingPackets % g.txPacketsPerGroup, 0U);
    EXPECT_EQ(g.txSharedSlotPackets % g.txPacketsPerGroup, 0U);
    EXPECT_EQ(g.rxHardwareRingPackets % g.rxPacketsPerGroup, 0U);
}

} // namespace
