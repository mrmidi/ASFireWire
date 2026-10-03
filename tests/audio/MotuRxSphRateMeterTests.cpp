// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "Audio/DriverKit/Runtime/MotuRxSphRateMeter.hpp"
#include "Audio/Wire/AMDTP/MotuV3WireFormat.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using ASFW::Audio::Runtime::MotuRxAbsPhaseWindow;
using ASFW::Audio::Runtime::MotuRxPhaseWindow;
using ASFW::Audio::Runtime::MotuRxSphCumulativeMeasurement;
using ASFW::Audio::Runtime::MotuRxSphRateMeter;
using ASFW::Audio::Runtime::MotuRxSphRateWindow;

namespace MotuV3Wire = ASFW::Protocols::Audio::AMDTP::MotuV3Wire;

constexpr int64_t kTicksPerSecond = static_cast<int64_t>(MotuV3Wire::kTicksPerSecond);
constexpr uint32_t kFramesPerPacket = 8;

// Exact rational tick position of `frame` for a device running `ppb` fast.
// Kept rational so 44 100 Hz is expressed as 557.278912… ticks per frame and
// not as the integer 557 the transmit path currently uses. Parts per billion,
// because the real device sits under 1 ppm and whole ppm cannot express it.
int64_t TicksAtFrame(uint64_t frame, uint32_t sampleRateHz, int64_t ppb) {
    const int64_t base = static_cast<int64_t>(
        (static_cast<__int128>(frame) * kTicksPerSecond) / sampleRateHz);
    return base + static_cast<int64_t>(
        (static_cast<__int128>(base) * ppb) / 1000000000);
}

// Feeds `packets` data packets of a device clock running `ppb` fast, starting
// at `startTicks` in the SPH domain.
void FeedStream(MotuRxSphRateMeter& meter,
                uint32_t packets,
                uint32_t sampleRateHz,
                int64_t ppb,
                int64_t startTicks = 0,
                uint32_t framesPerPacket = kFramesPerPacket) {
    for (uint32_t packet = 0; packet <= packets; ++packet) {
        const uint64_t frame = static_cast<uint64_t>(packet) * framesPerPacket;
        const int64_t ticks = startTicks + TicksAtFrame(frame, sampleRateHz, ppb);
        meter.Observe(MotuV3Wire::EncodeSph(ticks), framesPerPacket, sampleRateHz);
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

// The measurement this meter exists to produce: the wire says the MOTU clock
// runs about +14 ppm against the nominal step the host transmits.
TEST(MotuRxSphRateMeterTests, FourteenPpmFastDeviceIsResolved) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 24000, 48000, 14000);  // 192 000 frames — one 4 s window

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    ASSERT_TRUE(window.valid);
    EXPECT_EQ(window.rejected, 0u);
    EXPECT_NEAR(static_cast<double>(window.deviationPpb), 14000.0, 60.0);
    EXPECT_NEAR(static_cast<double>(window.ticksPerFrameMicro), 512007168.0, 600.0);
}

// A one ppm difference must not disappear into integer truncation, or the
// meter cannot tell a locked clock from a drifting one.
TEST(MotuRxSphRateMeterTests, SinglePpmIsAboveTheNoiseFloor) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 24000, 48000, 1000);

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    ASSERT_TRUE(window.valid);
    EXPECT_NEAR(static_cast<double>(window.deviationPpb), 1000.0, 60.0);
}

// The value this hardware actually produced: 512.000484 ticks per frame, i.e.
// +945 ppb. Reported in whole ppm it truncated to 0 and was indistinguishable
// from a device in perfect lock, which is the wrong answer in the one place
// the measurement matters. The deviation must survive at this magnitude.
TEST(MotuRxSphRateMeterTests, SubPpmDeviationSurvivesAsMeasuredOnHardware) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 24000, 48000, 945);

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    ASSERT_TRUE(window.valid);
    EXPECT_NEAR(static_cast<double>(window.deviationPpb), 945.0, 60.0);
    EXPECT_NE(window.deviationPpb, 0);
    // Tolerance, not equality: the generator lays stamps on whole ticks, so the
    // synthesised slope carries its own sub-tick rounding. The meter is not
    // more exact than the stream it is given.
    EXPECT_NEAR(static_cast<double>(window.ticksPerFrameMicro), 512000484.0, 10.0);
}

// Symmetry: a slow device must report a negative deviation, not a wrapped or
// clamped one. The log line formats the sign separately, so a sign error here
// would surface as a plausible-looking positive number.
TEST(MotuRxSphRateMeterTests, SlowDeviceReportsNegativeDeviation) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 24000, 48000, -945);

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    ASSERT_TRUE(window.valid);
    EXPECT_NEAR(static_cast<double>(window.deviationPpb), -945.0, 60.0);
    EXPECT_LT(window.ticksPerFrameMicro, 512000000);
}

// The nominal step for 44 100 Hz is 24 576 000 / 44 100 = 557.278912…, so an
// integer nominal would report a device that is exactly on rate as 500 ppm
// slow. That truncation is the documented blocker for the 44.1 family; this
// pins the rational form.
TEST(MotuRxSphRateMeterTests, FortyFourOneOnRateReportsZeroNotIntegerTruncation) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 22050, 44100, 0);

    const MotuRxSphRateWindow window = meter.Snapshot(44100);
    ASSERT_TRUE(window.valid);
    EXPECT_NEAR(static_cast<double>(window.deviationPpb), 0.0, 60.0);
    EXPECT_NEAR(static_cast<double>(window.ticksPerFrameMicro), 557278912.0, 600.0);
}

// The same stream measured against the integer step the transmit path uses
// today: 557 instead of 557.278912. This is the size of the error the 44.1
// family would ship with, and it is the reason the step must stay fractional
// whether or not it is ever steered.
TEST(MotuRxSphRateMeterTests, IntegerNominalWouldMisreadAnOnRateFortyFourOneDevice) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 22050, 44100, 0);

    const MotuRxSphRateWindow window = meter.Snapshot(44100);
    ASSERT_TRUE(window.valid);

    const int64_t integerStep = static_cast<int64_t>(kTicksPerSecond / 44100);
    ASSERT_EQ(integerStep, 557);
    const int64_t integerNominalTicks =
        integerStep * static_cast<int64_t>(window.frames);
    const int64_t integerPpb =
        ((window.ticks - integerNominalTicks) * 1000000000) / integerNominalTicks;
    EXPECT_NEAR(static_cast<double>(integerPpb), 500000.0, 2000.0);
}

// The SPH field wraps every second: it carries cycle and offset only, and the
// device leaves the cycle timer's seconds field at zero. A window straddling
// that wrap must measure the clock, not the wrap.
TEST(MotuRxSphRateMeterTests, WindowStraddlingTheTickDomainWrapStaysCorrect) {
    const int64_t domain = static_cast<int64_t>(MotuV3Wire::kTickDomain);
    MotuRxSphRateMeter meter{};
    // Four seconds starting one cycle short of a wrap, so the boundary is hit
    // in the first packets and four more times before the window closes.
    FeedStream(meter, 24000, 48000, 14000,
               domain - static_cast<int64_t>(MotuV3Wire::kTicksPerCycle));

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    ASSERT_TRUE(window.valid);
    EXPECT_EQ(window.rejected, 0u);
    EXPECT_NEAR(static_cast<double>(window.deviationPpb), 14000.0, 60.0);
}

// The `rejected` artefact, pinned. While this meter differenced modulo eight
// seconds against an SPH that wraps every one, a device stamp rolling 7999 -> 0
// produced a step of about -24.6 M ticks and IsPlausibleStep discarded it --
// once per second of playback, for the whole run (a 6:59:48 soak reported
// rejected == 25 188, exactly its length in seconds). The device clock does
// nothing at that instant, so nothing may be discarded and the accumulated
// phase must tile the stream unbroken.
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

// A restart or a dropped span produces a step nothing like a sample period.
// It must be counted and discarded, not averaged into the rate.
TEST(MotuRxSphRateMeterTests, DiscontinuityIsRejectedNotAveraged) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 1000, 48000, 0);

    // Jump half a second forward, then resume a clean nominal stream.
    const int64_t resumeTicks = TicksAtFrame(1000 * kFramesPerPacket, 48000, 0) +
        kTicksPerSecond / 2;
    meter.Observe(MotuV3Wire::EncodeSph(resumeTicks), kFramesPerPacket, 48000);
    for (uint32_t packet = 1; packet <= 1000; ++packet) {
        const int64_t ticks = resumeTicks +
            TicksAtFrame(static_cast<uint64_t>(packet) * kFramesPerPacket, 48000, 0);
        meter.Observe(MotuV3Wire::EncodeSph(ticks), kFramesPerPacket, 48000);
    }

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    ASSERT_TRUE(window.valid);
    EXPECT_EQ(window.rejected, 1u);
    EXPECT_EQ(window.packets, 2000u);
    EXPECT_EQ(window.deviationPpb, 0);
}

// The same discontinuity, attributed. A single counter could only say
// "one step was rejected", which is the same thing it says for a runt
// packet -- and "is the hard-resync valve ever reachable?" cannot be answered
// from a counter that conflates the two.
TEST(MotuRxSphRateMeterTests, ForwardDiscontinuityIsAttributedToRateNotRegression) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 1000, 48000, 0);

    const int64_t resumeTicks = TicksAtFrame(1000 * kFramesPerPacket, 48000, 0) +
        kTicksPerSecond / 2;
    meter.Observe(MotuV3Wire::EncodeSph(resumeTicks), kFramesPerPacket, 48000);

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    ASSERT_TRUE(window.valid);
    EXPECT_EQ(window.rejected, 1u);
    // Forward jump: the stamp advanced, so this is a rate verdict, not a
    // regression. The distinction is the whole point of the split.
    EXPECT_EQ(window.rejectedImplausibleRate, 1u);
    EXPECT_EQ(window.rejectedNonMonotonic, 0u);
    // Exactly half a second: `FeedStream` ends on this very frame, so no
    // nominal packet step is due on top of the jump.
    EXPECT_EQ(window.worstRejectedStepTicks, kTicksPerSecond / 2);
    EXPECT_EQ(window.worstRejectedFrames, kFramesPerPacket);
}

// A stamp that goes backwards is not an implausible clock: the one-second wrap
// is already folded out, so what is left is a regression. It has to read as one.
TEST(MotuRxSphRateMeterTests, BackwardStepIsAttributedToRegression) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 100, 48000, 0);

    // Well inside half the tick domain, so `MotuShortestTickDifference` reads
    // it as negative rather than as a forward wrap.
    const int64_t here = TicksAtFrame(100 * kFramesPerPacket, 48000, 0);
    meter.Observe(MotuV3Wire::EncodeSph(here - 10000), kFramesPerPacket, 48000);

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    ASSERT_TRUE(window.valid);
    EXPECT_EQ(window.rejected, 1u);
    EXPECT_EQ(window.rejectedNonMonotonic, 1u);
    EXPECT_EQ(window.rejectedImplausibleRate, 0u);
    EXPECT_LT(window.worstRejectedStepTicks, 0);
}

// The tail is kept by magnitude, not by recency: a later, smaller rejection
// must not overwrite the one large enough to have been a real discontinuity.
TEST(MotuRxSphRateMeterTests, WorstRejectedStepKeepsTheLargestNotTheLatest) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 100, 48000, 0);

    int64_t cursor = TicksAtFrame(100 * kFramesPerPacket, 48000, 0);
    cursor += kTicksPerSecond / 2;                    // duzy odrzut
    meter.Observe(MotuV3Wire::EncodeSph(cursor), kFramesPerPacket, 48000);
    const int64_t large = meter.Snapshot(48000).worstRejectedStepTicks;
    ASSERT_GT(large, 0);

    cursor += static_cast<int64_t>(kFramesPerPacket) * 512 * 2;  // maly odrzut
    meter.Observe(MotuV3Wire::EncodeSph(cursor), kFramesPerPacket, 48000);

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    EXPECT_EQ(window.rejected, 2u);
    EXPECT_EQ(window.rejectedImplausibleRate, 2u);
    EXPECT_EQ(window.worstRejectedStepTicks, large);
}

// The split has to be additive, not a behaviour change: a clean stream stays
// clean, and the sum stays the sum.
TEST(MotuRxSphRateMeterTests, SplitCountersSumToTheLegacyRejectedTotal) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 500, 48000, 0);

    int64_t cursor = TicksAtFrame(500 * kFramesPerPacket, 48000, 0);
    cursor += kTicksPerSecond / 2;
    meter.Observe(MotuV3Wire::EncodeSph(cursor), kFramesPerPacket, 48000);
    meter.Observe(MotuV3Wire::EncodeSph(cursor - 20000), kFramesPerPacket, 48000);

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    ASSERT_TRUE(window.valid);
    EXPECT_EQ(window.rejected,
              window.rejectedNonMonotonic + window.rejectedImplausibleRate);
    EXPECT_EQ(window.rejectedImplausibleRate, 1u);
    EXPECT_EQ(window.rejectedNonMonotonic, 1u);
}

// `Reset()` owns the rejection lifetime, exactly as it owns `rejected`.
TEST(MotuRxSphRateMeterTests, ResetClearsTheSplitRejectionCounters) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 100, 48000, 0);
    const int64_t here = TicksAtFrame(100 * kFramesPerPacket, 48000, 0);
    meter.Observe(MotuV3Wire::EncodeSph(here + kTicksPerSecond / 2),
                  kFramesPerPacket, 48000);
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

// NO-DATA packets advance neither the stamp nor the frame count, so a step
// measured across them is still one packet's worth of frames.
TEST(MotuRxSphRateMeterTests, NoDataPacketsDoNotDisturbTheStep) {
    MotuRxSphRateMeter meter{};
    for (uint32_t packet = 0; packet <= 1000; ++packet) {
        const uint64_t frame = static_cast<uint64_t>(packet) * kFramesPerPacket;
        const int64_t ticks = TicksAtFrame(frame, 48000, 0);
        meter.Observe(MotuV3Wire::EncodeSph(ticks), kFramesPerPacket, 48000);
        meter.Observe(MotuV3Wire::EncodeSph(ticks), 0, 48000);  // NO-DATA
    }

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    ASSERT_TRUE(window.valid);
    EXPECT_EQ(window.packets, 1000u);
    EXPECT_EQ(window.rejected, 0u);
    EXPECT_EQ(window.deviationPpb, 0);
}

// Windows tile the stream: closing one must not drop the step that crosses it.
TEST(MotuRxSphRateMeterTests, BeginWindowKeepsTheChainAndClearsTheAccumulation) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 1000, 48000, 0);
    ASSERT_EQ(meter.Snapshot(48000).packets, 1000u);

    meter.BeginWindow();
    const MotuRxSphRateWindow cleared = meter.Snapshot(48000);
    EXPECT_FALSE(cleared.valid);
    EXPECT_EQ(cleared.frames, 0u);

    // One further packet, continuing the same clock: the step across the window
    // boundary is measured, so a single observation already yields a window.
    const int64_t next = TicksAtFrame(1001 * kFramesPerPacket, 48000, 0);
    meter.Observe(MotuV3Wire::EncodeSph(next), kFramesPerPacket, 48000);

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
    const MotuRxSphCumulativeMeasurement beforeWindowReset =
        meter.CumulativeSnapshot();
    ASSERT_TRUE(beforeWindowReset.valid);
    EXPECT_EQ(beforeWindowReset.frames, 1000u * kFramesPerPacket);
    EXPECT_EQ(beforeWindowReset.ticks,
              static_cast<int64_t>(beforeWindowReset.frames) * 512);

    meter.BeginWindow();
    const MotuRxSphCumulativeMeasurement afterWindowReset =
        meter.CumulativeSnapshot();
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

    const int64_t next = TicksAtFrame(11 * kFramesPerPacket, 48000, 0);
    meter.Observe(MotuV3Wire::EncodeSph(next), kFramesPerPacket, 48000);

    const MotuRxSphRateWindow window = meter.Snapshot(48000);
    EXPECT_FALSE(window.valid);
    EXPECT_EQ(window.packets, 0u);
    EXPECT_EQ(window.rejected, 0u);
}

TEST(MotuRxSphRateMeterTests, ReanchorDropsGapButPreservesCumulativeHighWater) {
    MotuRxSphRateMeter meter{};
    meter.Observe(MotuV3Wire::EncodeSph(0), kFramesPerPacket, 48000);
    meter.Observe(MotuV3Wire::EncodeSph(4096), kFramesPerPacket, 48000);
    meter.Observe(MotuV3Wire::EncodeSph(8192), kFramesPerPacket, 48000);
    const MotuRxSphCumulativeMeasurement before = meter.CumulativeSnapshot();
    ASSERT_TRUE(before.valid);

    meter.Reanchor();

    // The first packet after recovery may be arbitrarily far away in the SPH
    // domain. It becomes the new anchor and contributes no cross-gap step.
    meter.Observe(MotuV3Wire::EncodeSph(1048576), kFramesPerPacket, 48000);
    EXPECT_FALSE(meter.Snapshot(48000).valid);
    EXPECT_EQ(meter.CumulativeSnapshot().frames, before.frames);
    EXPECT_EQ(meter.CumulativeSnapshot().ticks, before.ticks);

    meter.Observe(MotuV3Wire::EncodeSph(1052672), kFramesPerPacket, 48000);
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
}

// --- MotuRxPhaseWindow: cumulative phase error -----------------------------

// The reason this accumulator exists instead of reusing the windowed rate:
// a fixed-but-wrong step produces a `deviationPpb` that looks the same in
// every four-second window, while the *phase* it produces keeps growing.
// `BeginWindow()` must clear the rate window without touching that growth.
TEST(MotuRxSphRateMeterTests, WindowedRateResetsButCumulativePhaseDoesNot) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 24000, 48000, 945);  // one 4 s window, a measured hardware value
    const MotuRxPhaseWindow firstWindowPhase = meter.PhaseSnapshot(48000);
    ASSERT_TRUE(firstWindowPhase.valid);
    EXPECT_GT(firstWindowPhase.phaseErrorTicks, 0);

    meter.BeginWindow();
    const MotuRxSphRateWindow clearedRate = meter.Snapshot(48000);
    EXPECT_FALSE(clearedRate.valid);  // rate window is empty right after BeginWindow()

    const MotuRxPhaseWindow phaseAfterBeginWindow = meter.PhaseSnapshot(48000);
    ASSERT_TRUE(phaseAfterBeginWindow.valid);
    EXPECT_EQ(phaseAfterBeginWindow.phaseErrorTicks, firstWindowPhase.phaseErrorTicks);
    EXPECT_EQ(phaseAfterBeginWindow.frames, firstWindowPhase.frames);
}

// Feed the same +945 ppb drift across three windows and confirm the phase
// error is additive: three windows accumulate roughly three times the phase
// of one, not the same value each time (which is what a windowed-only
// measurement would show).
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

// A device exactly on rate must show a phase error indistinguishable from
// zero even after many windows -- this is the negative control for the
// accumulation test above.
TEST(MotuRxSphRateMeterTests, NominalClockProducesNegligiblePhaseError) {
    MotuRxSphRateMeter meter{};
    for (int window = 0; window < 5; ++window) {
        FeedStream(meter, 24000, 48000, 0,
                  TicksAtFrame(static_cast<uint64_t>(window) * 24000 * kFramesPerPacket,
                               48000, 0));
        meter.BeginWindow();
    }

    const MotuRxPhaseWindow phase = meter.PhaseSnapshot(48000);
    ASSERT_TRUE(phase.valid);
    EXPECT_NEAR(static_cast<double>(phase.phaseErrorTicks), 0.0, 200.0);
    EXPECT_EQ(phase.phaseErrorMilliCycles, 0);
}

// `Reset()` is the stream-restart path (DirectAudioReceiveConsumer::Reset()
// calls it directly). The accumulated phase from a previous stream must not
// survive into the next one, or a restart would look like an instantaneous
// multi-cycle phase jump instead of what it is: a fresh start.
TEST(MotuRxSphRateMeterTests, ResetClearsCumulativePhase) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 24000, 48000, 945);
    ASSERT_GT(meter.PhaseSnapshot(48000).phaseErrorTicks, 0);

    meter.Reset();
    EXPECT_FALSE(meter.PhaseSnapshot(48000).valid);

    FeedStream(meter, 24000, 48000, 0);
    EXPECT_NEAR(static_cast<double>(meter.PhaseSnapshot(48000).phaseErrorTicks), 0.0, 200.0);
}

// A step this meter already rejects as implausible (DiscontinuityIsRejectedNotAveraged
// above) must not enter the cumulative phase either -- otherwise a single
// restart-sized gap would masquerade as a permanent multi-cycle phase error.
TEST(MotuRxSphRateMeterTests, RejectedStepsDoNotEnterCumulativePhase) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 1000, 48000, 0);
    const int64_t phaseBeforeJump = meter.PhaseSnapshot(48000).phaseErrorTicks;

    const int64_t resumeTicks = TicksAtFrame(1000 * kFramesPerPacket, 48000, 0) +
        kTicksPerSecond / 2;
    meter.Observe(MotuV3Wire::EncodeSph(resumeTicks), kFramesPerPacket, 48000);

    const MotuRxPhaseWindow phaseAfterJump = meter.PhaseSnapshot(48000);
    ASSERT_TRUE(phaseAfterJump.valid);
    EXPECT_EQ(phaseAfterJump.phaseErrorTicks, phaseBeforeJump);
    EXPECT_EQ(phaseAfterJump.frames, 1000u * kFramesPerPacket);
}

// Converts to the official driver's own unit: its Internal-clock hard-resync
// bound is exactly 4.0 cycles. A device drifting at the magnitude actually
// measured on hardware (923 ppb) must show a
// phase error whose milli-cycle conversion lands where hand computation says
// it should -- this is the number the eventual regulator will compare against
// that threshold, so its arithmetic is pinned here independent of the
// regulator's existence.
TEST(MotuRxSphRateMeterTests, PhaseErrorMilliCyclesMatchesHandComputationAtMeasuredDrift) {
    MotuRxSphRateMeter meter{};
    FeedStream(meter, 24000, 48000, 923);

    const MotuRxPhaseWindow phase = meter.PhaseSnapshot(48000);
    ASSERT_TRUE(phase.valid);

    const double expectedMilliCycles =
        (static_cast<double>(phase.phaseErrorTicks) * 1000.0) /
        static_cast<double>(MotuV3Wire::kTicksPerCycle);
    EXPECT_NEAR(static_cast<double>(phase.phaseErrorMilliCycles),
               expectedMilliCycles, 2.0);
}

// ---------------------------------------------------------------------------
// P2 -- absolute receive phase (observation only, no behaviour change)
// ---------------------------------------------------------------------------

namespace {

// A device SPH and the host receive cycle timer for the same instant, offset by
// `phaseTicks`. The host side carries whole seconds; the device side never does
// (P1), so a correct meter must fold both into the one-second domain before
// subtracting -- `hostSeconds` exists to prove it does.
void ObservePhaseAt(MotuRxSphRateMeter& meter,
                    int64_t phaseTicks,
                    uint32_t hostSeconds = 5,
                    int64_t hostWithinSecondTicks = 12 * MotuV3Wire::kTicksPerCycle) {
    const int64_t sphTicks = hostWithinSecondTicks + phaseTicks;
    const uint32_t cycleTimer = ASFW::Timing::encodeCycleTimer(
        hostSeconds,
        static_cast<uint32_t>(hostWithinSecondTicks / MotuV3Wire::kTicksPerCycle),
        static_cast<uint32_t>(hostWithinSecondTicks % MotuV3Wire::kTicksPerCycle));
    meter.ObserveAbsolutePhase(MotuV3Wire::EncodeSph(sphTicks), cycleTimer);
}

} // namespace

// The shape, not the value: min/median/max over a window is what tells a phase
// holding its setpoint apart from one drifting or jumping.
TEST(MotuRxSphRateMeterTests, AbsolutePhaseWindowReportsTheDistributionShape) {
    MotuRxSphRateMeter meter{};
    for (const int64_t ticks : {9216, 9100, 9300, 9216, 9250}) {
        ObservePhaseAt(meter, ticks);
    }

    const MotuRxAbsPhaseWindow window = meter.AbsolutePhaseSnapshot();
    ASSERT_TRUE(window.valid);
    EXPECT_EQ(window.samples, 5u);
    EXPECT_EQ(window.stored, 5u);
    EXPECT_EQ(window.minTicks, 9100);
    EXPECT_EQ(window.maxTicks, 9300);
    EXPECT_EQ(window.medianTicks, 9216);
}

// The host clock wraps at 128 s and the SPH at one; only after both are folded
// into the same second does this field mean anything. Before the P1 domain fix
// the same pair read as tens of millions of ticks of aliasing.
TEST(MotuRxSphRateMeterTests, AbsolutePhaseIgnoresWholeHostSeconds) {
    MotuRxSphRateMeter meter{};
    for (const uint32_t second : {0u, 7u, 93u, 127u}) {
        ObservePhaseAt(meter, 9216, second);
    }

    const MotuRxAbsPhaseWindow window = meter.AbsolutePhaseSnapshot();
    ASSERT_TRUE(window.valid);
    EXPECT_EQ(window.samples, 4u);
    EXPECT_EQ(window.minTicks, 9216);
    EXPECT_EQ(window.maxTicks, 9216);
}

// A packet carrying no device SPH must not be counted. The caller gates on
// `hasMotuRxSph`, so the meter simply never sees those -- and an unfed window
// has to report itself invalid rather than a zero-valued distribution, which
// would read as "phase measured, and it is zero".
TEST(MotuRxSphRateMeterTests, AbsolutePhaseWindowIsInvalidWithoutObservations) {
    MotuRxSphRateMeter meter{};
    const MotuRxAbsPhaseWindow window = meter.AbsolutePhaseSnapshot();
    EXPECT_FALSE(window.valid);
    EXPECT_EQ(window.samples, 0u);
    EXPECT_EQ(window.stored, 0u);
    EXPECT_EQ(window.medianTicks, 0);
}

// Window-scoped like the rate window beside it, and cleared by the same call,
// so the two lines in the ring always describe the same four seconds.
TEST(MotuRxSphRateMeterTests, AbsolutePhaseWindowClearsWithTheRateWindow) {
    MotuRxSphRateMeter meter{};
    ObservePhaseAt(meter, 9216);
    ASSERT_TRUE(meter.AbsolutePhaseSnapshot().valid);

    meter.BeginWindow();
    EXPECT_FALSE(meter.AbsolutePhaseSnapshot().valid);

    ObservePhaseAt(meter, 9216);
    ASSERT_TRUE(meter.AbsolutePhaseSnapshot().valid);
    meter.Reset();
    EXPECT_FALSE(meter.AbsolutePhaseSnapshot().valid);
}

// Overflow must be visible, not silent: `samples` keeps counting past storage
// so a window that outgrew the buffer says so instead of presenting a truncated
// distribution as if it covered everything.
TEST(MotuRxSphRateMeterTests, AbsolutePhaseWindowReportsSamplesBeyondItsStorage) {
    MotuRxSphRateMeter meter{};
    const uint32_t offered = MotuRxSphRateMeter::kAbsPhaseCapacity + 20u;
    for (uint32_t i = 0; i < offered; ++i) {
        ObservePhaseAt(meter, 9216 + static_cast<int64_t>(i));
    }

    const MotuRxAbsPhaseWindow window = meter.AbsolutePhaseSnapshot();
    ASSERT_TRUE(window.valid);
    EXPECT_EQ(window.samples, offered);
    EXPECT_EQ(window.stored, MotuRxSphRateMeter::kAbsPhaseCapacity);
    EXPECT_GT(window.samples, window.stored);
    EXPECT_EQ(window.minTicks, 9216);
    EXPECT_EQ(window.maxTicks,
              9216 + static_cast<int64_t>(MotuRxSphRateMeter::kAbsPhaseCapacity) - 1);
}

// A jump is the signal here, not noise. `Observe()` discards implausible steps
// because a discontinuity would corrupt a rate estimate; this window must keep
// them, or it could never show the failure mode it was built to detect.
TEST(MotuRxSphRateMeterTests, AbsolutePhaseWindowKeepsDiscontinuities) {
    MotuRxSphRateMeter meter{};
    ObservePhaseAt(meter, 9216);
    ObservePhaseAt(meter, 9216);
    ObservePhaseAt(meter, 9216 + 4 * MotuV3Wire::kTicksPerCycle);

    const MotuRxAbsPhaseWindow window = meter.AbsolutePhaseSnapshot();
    ASSERT_TRUE(window.valid);
    EXPECT_EQ(window.samples, 3u);
    EXPECT_EQ(window.minTicks, 9216);
    EXPECT_EQ(window.maxTicks, 9216 + 4 * MotuV3Wire::kTicksPerCycle);
    EXPECT_EQ(window.medianTicks, 9216);
}

} // namespace
