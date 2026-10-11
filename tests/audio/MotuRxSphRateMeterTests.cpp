// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// The MOTU device's sample period, measured from received SPH: rational
// nominal at every rate, sub-ppm resolution, the one-second SPH wrap, and the
// rejection of steps that are not a clock.

#include "Audio/DriverKit/Runtime/MotuRxSphRateMeter.hpp"
#include "Audio/Wire/MOTU/MotuSph.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using ASFW::Audio::Runtime::MotuRxPhaseWindow;
using ASFW::Audio::Runtime::MotuRxSphCumulativeMeasurement;
using ASFW::Audio::Runtime::MotuRxSphRateMeter;
using ASFW::Audio::Runtime::MotuRxSphRateWindow;

constexpr int64_t kTicksPerSecond = ASFW::Encoding::Motu::kTicksPerSecond;
constexpr int64_t kTicksPerCycle = ASFW::Encoding::Motu::kTicksPerCycle;
constexpr uint32_t kFramesPerPacket = 8;

uint32_t Sph(int64_t ticks) {
    return ASFW::Encoding::Motu::SphFromTick(ASFW::Encoding::Motu::WrapTick(ticks));
}

// Tick position of `frame` for a device running `ppb` fast, kept rational so
// 44.1 kHz is 557.278912... ticks per frame and not the integer 557.
int64_t TicksAtFrame(uint64_t frame, uint32_t sampleRateHz, int64_t ppb) {
    const int64_t base =
        static_cast<int64_t>((static_cast<__int128>(frame) * kTicksPerSecond) / sampleRateHz);
    return base + static_cast<int64_t>((static_cast<__int128>(base) * ppb) / 1000000000);
}

// `packets` data packets of a device clock running `ppb` fast, starting at
// `startTicks` on the SPH timeline.
void FeedStream(MotuRxSphRateMeter& meter, uint32_t packets, uint32_t sampleRateHz, int64_t ppb,
                int64_t startTicks = 0, uint32_t framesPerPacket = kFramesPerPacket) {
    for (uint32_t packet = 0; packet <= packets; ++packet) {
        const uint64_t frame = static_cast<uint64_t>(packet) * framesPerPacket;
        meter.Observe(Sph(startTicks + TicksAtFrame(frame, sampleRateHz, ppb)), framesPerPacket,
                      sampleRateHz);
    }
}

TEST(MotuRxSphRateMeterTests, NominalFortyEightKilohertzMeasuresExactlyFiveTwelve) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 1000, 48000, 0);

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    ASSERT_TRUE(window.valid);
    EXPECT_EQ(window.frames, 1000u * kFramesPerPacket);
    EXPECT_EQ(window.packets, 1000u);
    EXPECT_EQ(window.rejected, 0u);
    EXPECT_EQ(window.ticksPerFrameMicro, 512000000);
    EXPECT_EQ(window.deviationPpb, 0);
}

TEST(MotuRxSphRateMeterTests, FourteenPpmFastDeviceIsResolved) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 24000, 48000, 14000);  // 192 000 frames, one 4 s window

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    ASSERT_TRUE(window.valid);
    EXPECT_EQ(window.rejected, 0u);
    EXPECT_NEAR(static_cast<double>(window.deviationPpb), 14000.0, 60.0);
    EXPECT_NEAR(static_cast<double>(window.ticksPerFrameMicro), 512007168.0, 600.0);
}

TEST(MotuRxSphRateMeterTests, SinglePpmIsAboveTheNoiseFloor) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 24000, 48000, 1000);

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    ASSERT_TRUE(window.valid);
    EXPECT_NEAR(static_cast<double>(window.deviationPpb), 1000.0, 60.0);
}

// +945 ppb, as measured on an 828mk3: whole ppm would truncate it to zero,
// indistinguishable from a device in perfect lock.
TEST(MotuRxSphRateMeterTests, SubPpmDeviationSurvives) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 24000, 48000, 945);

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    ASSERT_TRUE(window.valid);
    EXPECT_NEAR(static_cast<double>(window.deviationPpb), 945.0, 60.0);
    EXPECT_NE(window.deviationPpb, 0);
    // The generator lays stamps on whole ticks, so the slope carries its own
    // sub-tick rounding.
    EXPECT_NEAR(static_cast<double>(window.ticksPerFrameMicro), 512000484.0, 10.0);
}

TEST(MotuRxSphRateMeterTests, SlowDeviceReportsNegativeDeviation) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 24000, 48000, -945);

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    ASSERT_TRUE(window.valid);
    EXPECT_NEAR(static_cast<double>(window.deviationPpb), -945.0, 60.0);
    EXPECT_LT(window.ticksPerFrameMicro, 512000000);
}

// 24 576 000 / 44 100 = 557.278912...: an integer nominal would report a
// device exactly on rate as 500 ppm slow.
TEST(MotuRxSphRateMeterTests, FortyFourOneOnRateReportsZeroNotIntegerTruncation) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 22050, 44100, 0);

    const MotuRxSphRateWindow window = meter.Snapshot(44100);
    ASSERT_TRUE(window.valid);
    EXPECT_NEAR(static_cast<double>(window.deviationPpb), 0.0, 60.0);
    EXPECT_NEAR(static_cast<double>(window.ticksPerFrameMicro), 557278912.0, 600.0);

    // The same stream against the integer step, for scale.
    const int64_t integerStep = kTicksPerSecond / 44100;
    ASSERT_EQ(integerStep, 557);
    const int64_t integerNominalTicks = integerStep * static_cast<int64_t>(window.frames);
    const int64_t integerPpb = ((window.ticks - integerNominalTicks) * 1000000000) / integerNominalTicks;
    EXPECT_NEAR(static_cast<double>(integerPpb), 500000.0, 2000.0);
}

// Higher rates carry more frames per DATA packet; the measurement does not
// care how they are grouped.
TEST(MotuRxSphRateMeterTests, MeasuresEveryRateFamilyAtItsOwnPacketSize) {
    struct Case { uint32_t rate; uint32_t framesPerPacket; };
    for (const Case c : {Case{88200, 16}, Case{96000, 16}, Case{176400, 32}, Case{192000, 32}}) {
        SCOPED_TRACE(c.rate);
        MotuRxSphRateMeter meter{};
        FeedStream(meter, c.rate / c.framesPerPacket, c.rate, 2000, 0, c.framesPerPacket);
        const MotuRxSphRateWindow window = meter.Snapshot(c.rate);
        ASSERT_TRUE(window.valid);
        EXPECT_EQ(window.rejected, 0u);
        EXPECT_NEAR(static_cast<double>(window.deviationPpb), 2000.0, 60.0);
    }
}

// The SPH wraps every second. A window straddling the wrap measures the
// clock, not the wrap.
TEST(MotuRxSphRateMeterTests, WindowStraddlingTheOneSecondWrapStaysCorrect) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 24000, 48000, 14000, kTicksPerSecond - kTicksPerCycle);

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    ASSERT_TRUE(window.valid);
    EXPECT_EQ(window.rejected, 0u);
    EXPECT_NEAR(static_cast<double>(window.deviationPpb), 14000.0, 60.0);
}

// A rollover 7999 -> 0 is not a discontinuity: nothing is discarded and the
// cumulative total tiles the stream unbroken. Differenced on any wider domain
// it reads as a step of about -24.6 M ticks once per second.
TEST(MotuRxSphRateMeterTests, SecondBoundaryRolloverIsNotADiscontinuity) {
    constexpr uint32_t kPacketsPerSecond = 48000 / kFramesPerPacket;
    constexpr uint32_t kPackets = 3 * kPacketsPerSecond;
    MotuRxSphRateMeter meter{};
    FeedStream(meter, kPackets, 48000, 0, kTicksPerSecond - 4096);

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    ASSERT_TRUE(window.valid);
    EXPECT_EQ(window.rejected, 0u);
    EXPECT_EQ(window.packets, kPackets);
    EXPECT_EQ(window.deviationPpb, 0);

    const MotuRxSphCumulativeMeasurement cumulative = meter.CumulativeSnapshot();
    ASSERT_TRUE(cumulative.valid);
    EXPECT_EQ(cumulative.frames, kPackets * kFramesPerPacket);
    EXPECT_EQ(cumulative.ticks, static_cast<int64_t>(cumulative.frames) * 512);
}

TEST(MotuRxSphRateMeterTests, DiscontinuityIsRejectedNotAveraged) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 1000, 48000, 0);

    // Half a second forward, then a clean nominal stream again.
    const int64_t resumeTicks = TicksAtFrame(1000 * kFramesPerPacket, 48000, 0) + kTicksPerSecond / 2;
    meter.Observe(Sph(resumeTicks), kFramesPerPacket, 48000);
    for (uint32_t packet = 1; packet <= 1000; ++packet) {
        const int64_t ticks =
            resumeTicks + TicksAtFrame(static_cast<uint64_t>(packet) * kFramesPerPacket, 48000, 0);
        meter.Observe(Sph(ticks), kFramesPerPacket, 48000);
    }

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    ASSERT_TRUE(window.valid);
    EXPECT_EQ(window.rejected, 1u);
    EXPECT_EQ(window.packets, 2000u);
    EXPECT_EQ(window.deviationPpb, 0);
}

TEST(MotuRxSphRateMeterTests, ForwardDiscontinuityIsAttributedToRateNotRegression) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 1000, 48000, 0);

    const int64_t resumeTicks = TicksAtFrame(1000 * kFramesPerPacket, 48000, 0) + kTicksPerSecond / 2;
    meter.Observe(Sph(resumeTicks), kFramesPerPacket, 48000);

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    ASSERT_TRUE(window.valid);
    EXPECT_EQ(window.rejected, 1u);
    EXPECT_EQ(window.rejectedImplausibleRate, 1u);
    EXPECT_EQ(window.rejectedNonMonotonic, 0u);
    // Exactly half a second: the stream ended on this very frame, so no
    // nominal step is due on top of the jump.
    EXPECT_EQ(window.worstRejectedStepTicks, kTicksPerSecond / 2);
    EXPECT_EQ(window.worstRejectedFrames, kFramesPerPacket);
}

TEST(MotuRxSphRateMeterTests, BackwardStepIsAttributedToRegression) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 100, 48000, 0);

    const int64_t here = TicksAtFrame(100 * kFramesPerPacket, 48000, 0);
    meter.Observe(Sph(here - 10000), kFramesPerPacket, 48000);

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    ASSERT_TRUE(window.valid);
    EXPECT_EQ(window.rejected, 1u);
    EXPECT_EQ(window.rejectedNonMonotonic, 1u);
    EXPECT_EQ(window.rejectedImplausibleRate, 0u);
    EXPECT_LT(window.worstRejectedStepTicks, 0);
}

TEST(MotuRxSphRateMeterTests, WorstRejectedStepKeepsTheLargestNotTheLatest) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 100, 48000, 0);

    int64_t cursor = TicksAtFrame(100 * kFramesPerPacket, 48000, 0);
    cursor += kTicksPerSecond / 2;  // a large rejected step
    meter.Observe(Sph(cursor), kFramesPerPacket, 48000);
    const int64_t large = meter.Snapshot(48000).worstRejectedStepTicks;
    ASSERT_GT(large, 0);

    cursor += static_cast<int64_t>(kFramesPerPacket) * 512 * 2;  // a small rejected step
    meter.Observe(Sph(cursor), kFramesPerPacket, 48000);

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    EXPECT_EQ(window.rejected, 2u);
    EXPECT_EQ(window.rejectedImplausibleRate, 2u);
    EXPECT_EQ(window.worstRejectedStepTicks, large);
}

TEST(MotuRxSphRateMeterTests, SplitCountersSumToTheRejectedTotal) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 500, 48000, 0);

    int64_t cursor = TicksAtFrame(500 * kFramesPerPacket, 48000, 0);
    cursor += kTicksPerSecond / 2;
    meter.Observe(Sph(cursor), kFramesPerPacket, 48000);
    meter.Observe(Sph(cursor - 20000), kFramesPerPacket, 48000);

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    ASSERT_TRUE(window.valid);
    EXPECT_EQ(window.rejected, window.rejectedNonMonotonic + window.rejectedImplausibleRate);
    EXPECT_EQ(window.rejectedImplausibleRate, 1u);
    EXPECT_EQ(window.rejectedNonMonotonic, 1u);
}

TEST(MotuRxSphRateMeterTests, ResetClearsTheSplitRejectionCounters) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 100, 48000, 0);
    const int64_t here = TicksAtFrame(100 * kFramesPerPacket, 48000, 0);
    meter.Observe(Sph(here + kTicksPerSecond / 2), kFramesPerPacket, 48000);
    ASSERT_EQ(meter.Snapshot(48000).rejectedImplausibleRate, 1u);

    meter.Reset();
    FeedStream(meter, 100, 48000, 0);

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    ASSERT_TRUE(window.valid);
    EXPECT_EQ(window.rejected, 0u);
    EXPECT_EQ(window.rejectedNonMonotonic, 0u);
    EXPECT_EQ(window.rejectedImplausibleRate, 0u);
    EXPECT_EQ(window.worstRejectedStepTicks, 0);
    EXPECT_EQ(window.worstRejectedFrames, 0u);
}

TEST(MotuRxSphRateMeterTests, NoDataPacketsDoNotDisturbTheStep) {
    MotuRxSphRateMeter meter{};
    for (uint32_t packet = 0; packet <= 1000; ++packet) {
        const int64_t ticks = TicksAtFrame(static_cast<uint64_t>(packet) * kFramesPerPacket, 48000, 0);
        meter.Observe(Sph(ticks), kFramesPerPacket, 48000);
        meter.Observe(Sph(ticks), 0, 48000);  // NO-DATA
    }

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    ASSERT_TRUE(window.valid);
    EXPECT_EQ(window.packets, 1000u);
    EXPECT_EQ(window.rejected, 0u);
    EXPECT_EQ(window.deviationPpb, 0);
}

TEST(MotuRxSphRateMeterTests, BeginWindowKeepsTheChainAndClearsTheAccumulation) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 1000, 48000, 0);
    ASSERT_EQ(meter.Snapshot(48000).packets, 1000u);

    meter.BeginWindow();
    const MotuRxSphRateWindow cleared = meter.Snapshot(48000);
    EXPECT_FALSE(cleared.valid);
    EXPECT_EQ(cleared.frames, 0u);

    // The step across the window boundary is measured, so one further packet
    // already yields a window.
    meter.Observe(Sph(TicksAtFrame(1001 * kFramesPerPacket, 48000, 0)), kFramesPerPacket, 48000);

    const MotuRxSphRateWindow resumed = meter.Snapshot(48000);
    ASSERT_TRUE(resumed.valid);
    EXPECT_EQ(resumed.packets, 1u);
    EXPECT_EQ(resumed.frames, kFramesPerPacket);
    EXPECT_EQ(resumed.ticksPerFrameMicro, 512000000);
}

TEST(MotuRxSphRateMeterTests, CumulativeSnapshotPreservesRawMeasurementAcrossWindows) {
    MotuRxSphRateMeter meter{};
    EXPECT_FALSE(meter.CumulativeSnapshot().valid);

    FeedStream(meter, 1000, 48000, 0);
    const MotuRxSphCumulativeMeasurement beforeWindowReset = meter.CumulativeSnapshot();
    ASSERT_TRUE(beforeWindowReset.valid);
    EXPECT_EQ(beforeWindowReset.frames, 1000u * kFramesPerPacket);
    EXPECT_EQ(beforeWindowReset.ticks, static_cast<int64_t>(beforeWindowReset.frames) * 512);

    meter.BeginWindow();
    const MotuRxSphCumulativeMeasurement afterWindowReset = meter.CumulativeSnapshot();
    EXPECT_TRUE(afterWindowReset.valid);
    EXPECT_EQ(afterWindowReset.frames, beforeWindowReset.frames);
    EXPECT_EQ(afterWindowReset.ticks, beforeWindowReset.ticks);

    meter.Reset();
    EXPECT_FALSE(meter.CumulativeSnapshot().valid);
}

TEST(MotuRxSphRateMeterTests, ResetDropsTheChainSoNoStepCrossesIt) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 10, 48000, 0);
    meter.Reset();

    meter.Observe(Sph(TicksAtFrame(11 * kFramesPerPacket, 48000, 0)), kFramesPerPacket, 48000);

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    EXPECT_FALSE(window.valid);
    EXPECT_EQ(window.packets, 0u);
    EXPECT_EQ(window.rejected, 0u);
}

TEST(MotuRxSphRateMeterTests, ReanchorDropsTheGapButKeepsTheCumulativeTotals) {
    MotuRxSphRateMeter meter{};
    meter.Observe(Sph(0), kFramesPerPacket, 48000);
    meter.Observe(Sph(4096), kFramesPerPacket, 48000);
    meter.Observe(Sph(8192), kFramesPerPacket, 48000);
    const MotuRxSphCumulativeMeasurement before = meter.CumulativeSnapshot();
    ASSERT_TRUE(before.valid);

    meter.Reanchor();

    // The first packet after the gap may be anywhere on the SPH timeline. It
    // becomes the new anchor and contributes no cross-gap step.
    meter.Observe(Sph(1048576), kFramesPerPacket, 48000);
    EXPECT_FALSE(meter.Snapshot(48000).valid);
    EXPECT_EQ(meter.CumulativeSnapshot().frames, before.frames);
    EXPECT_EQ(meter.CumulativeSnapshot().ticks, before.ticks);

    meter.Observe(Sph(1052672), kFramesPerPacket, 48000);
    const MotuRxSphRateWindow resumed = meter.Snapshot(48000);
    const MotuRxSphCumulativeMeasurement after = meter.CumulativeSnapshot();
    ASSERT_TRUE(resumed.valid);
    ASSERT_TRUE(after.valid);
    EXPECT_EQ(resumed.packets, 1u);
    EXPECT_EQ(after.frames, before.frames + kFramesPerPacket);
    EXPECT_EQ(after.ticks, before.ticks + 4096);
    EXPECT_EQ(resumed.rejected, 0u);
}

TEST(MotuRxSphRateMeterTests, UnknownSampleRateProducesNoMeasurement) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 100, 48000, 0);
    EXPECT_FALSE(meter.Snapshot(0).valid);
    EXPECT_FALSE(meter.PhaseSnapshot(0).valid);

    MotuRxSphRateMeter unrated{};
    unrated.Observe(Sph(0), kFramesPerPacket, 0);
    unrated.Observe(Sph(4096), kFramesPerPacket, 0);
    EXPECT_FALSE(unrated.CumulativeSnapshot().valid);
}

// --- Cumulative phase error --------------------------------------------------

// A fixed-but-wrong rate gives the same deviationPpb in every window while
// the phase it produces keeps growing; BeginWindow() must not touch it.
TEST(MotuRxSphRateMeterTests, WindowedRateResetsButCumulativePhaseDoesNot) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 24000, 48000, 945);
    const MotuRxPhaseWindow firstWindowPhase = meter.PhaseSnapshot(48000);
    ASSERT_TRUE(firstWindowPhase.valid);
    EXPECT_GT(firstWindowPhase.phaseErrorTicks, 0);

    meter.BeginWindow();
    EXPECT_FALSE(meter.Snapshot(48000).valid);

    const MotuRxPhaseWindow phaseAfterBeginWindow = meter.PhaseSnapshot(48000);
    ASSERT_TRUE(phaseAfterBeginWindow.valid);
    EXPECT_EQ(phaseAfterBeginWindow.phaseErrorTicks, firstWindowPhase.phaseErrorTicks);
    EXPECT_EQ(phaseAfterBeginWindow.frames, firstWindowPhase.frames);
}

TEST(MotuRxSphRateMeterTests, PhaseErrorAccumulatesAcrossMultipleWindows) {
    MotuRxSphRateMeter meter{};
    int64_t startTicks = 0;

    FeedStream(meter, 24000, 48000, 945, startTicks);
    const int64_t phaseAfterOneWindow = meter.PhaseSnapshot(48000).phaseErrorTicks;
    startTicks = TicksAtFrame(24000 * kFramesPerPacket, 48000, 945);
    meter.BeginWindow();

    FeedStream(meter, 24000, 48000, 945, startTicks);
    startTicks += TicksAtFrame(24000 * kFramesPerPacket, 48000, 945);
    meter.BeginWindow();

    FeedStream(meter, 24000, 48000, 945, startTicks);
    const MotuRxPhaseWindow afterThreeWindows = meter.PhaseSnapshot(48000);

    ASSERT_TRUE(afterThreeWindows.valid);
    EXPECT_NEAR(static_cast<double>(afterThreeWindows.phaseErrorTicks),
                3.0 * static_cast<double>(phaseAfterOneWindow), 200.0);
}

TEST(MotuRxSphRateMeterTests, NominalClockProducesNegligiblePhaseError) {
    MotuRxSphRateMeter meter{};
    for (int window = 0; window < 5; ++window) {
        FeedStream(meter, 24000, 48000, 0,
                   TicksAtFrame(static_cast<uint64_t>(window) * 24000 * kFramesPerPacket, 48000, 0));
        meter.BeginWindow();
    }

    const MotuRxPhaseWindow phase = meter.PhaseSnapshot(48000);
    ASSERT_TRUE(phase.valid);
    EXPECT_NEAR(static_cast<double>(phase.phaseErrorTicks), 0.0, 200.0);
    EXPECT_EQ(phase.phaseErrorMilliCycles, 0);
}

// A new stream must not inherit the previous one's phase, or a restart would
// look like an instantaneous multi-cycle jump.
TEST(MotuRxSphRateMeterTests, ResetClearsCumulativePhase) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 24000, 48000, 945);
    ASSERT_GT(meter.PhaseSnapshot(48000).phaseErrorTicks, 0);

    meter.Reset();
    EXPECT_FALSE(meter.PhaseSnapshot(48000).valid);

    FeedStream(meter, 24000, 48000, 0);
    EXPECT_NEAR(static_cast<double>(meter.PhaseSnapshot(48000).phaseErrorTicks), 0.0, 200.0);
}

TEST(MotuRxSphRateMeterTests, RejectedStepsDoNotEnterCumulativePhase) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 1000, 48000, 0);
    const int64_t phaseBeforeJump = meter.PhaseSnapshot(48000).phaseErrorTicks;

    const int64_t resumeTicks = TicksAtFrame(1000 * kFramesPerPacket, 48000, 0) + kTicksPerSecond / 2;
    meter.Observe(Sph(resumeTicks), kFramesPerPacket, 48000);

    const MotuRxPhaseWindow phaseAfterJump = meter.PhaseSnapshot(48000);
    ASSERT_TRUE(phaseAfterJump.valid);
    EXPECT_EQ(phaseAfterJump.phaseErrorTicks, phaseBeforeJump);
    EXPECT_EQ(phaseAfterJump.frames, 1000u * kFramesPerPacket);
}

TEST(MotuRxSphRateMeterTests, PhaseErrorMilliCyclesMatchesHandComputation) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 24000, 48000, 923);  // a drift measured on an 828mk3

    const MotuRxPhaseWindow phase = meter.PhaseSnapshot(48000);
    ASSERT_TRUE(phase.valid);
    const double expectedMilliCycles =
        (static_cast<double>(phase.phaseErrorTicks) * 1000.0) / static_cast<double>(kTicksPerCycle);
    EXPECT_NEAR(static_cast<double>(phase.phaseErrorMilliCycles), expectedMilliCycles, 2.0);
}

} // namespace
