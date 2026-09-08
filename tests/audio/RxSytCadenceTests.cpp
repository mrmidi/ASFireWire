// The SYT cadence ring, and the phase-versus-cursor arithmetic built on it.
//
// Reference behaviour is Saffire.kext's ReadFirewireBuffers (0xd69e-0xd81e) and
// FillFirewireBuffers (0xec5d-0xed72). Where a case encodes a divergence from
// that reference, the comment says so.

#include "Audio/Wire/AMDTP/RxSytCadence.hpp"

#include <gtest/gtest.h>

namespace {

using ASFW::Driver::RxSytCadence;
using Update = RxSytCadence::Update;

constexpr uint32_t kTicksPerCycle = ASFW::Timing::kTicksPerCycle;
constexpr uint32_t kStep = 4096;   // 8 frames * 512 ticks, the 48 kHz step

uint32_t CycleTimer(uint32_t seconds, uint32_t cycle, uint32_t offset) {
    return (seconds << ASFW::Timing::kCycleTimerSecondsShift) |
           (cycle << ASFW::Timing::kCycleTimerCyclesShift) | offset;
}

// A SYT whose 16-cycle field lands `ticks` into the window.
uint16_t SytForFieldTicks(uint32_t ticks) {
    const uint32_t reduced = ticks % (16u * kTicksPerCycle);
    return static_cast<uint16_t>(((reduced / kTicksPerCycle) << 12) |
                                 (reduced % kTicksPerCycle));
}

// Drive the ring with a clean 48 kHz chain. Returns the SYT field position
// reached, so a caller can continue the sequence.
uint32_t FeedChain(RxSytCadence& cadence, uint32_t count, uint32_t startTicks) {
    uint32_t ticks = startTicks;
    for (uint32_t i = 0; i < count; ++i) {
        cadence.Observe(SytForFieldTicks(ticks), CycleTimer(0, 100 + i, 0));
        ticks += kStep;
    }
    return ticks;
}

TEST(RxSytCadenceTests, SeedWritesTheRunningAverageAndCountsIt) {
    // DIVERGENCE FIXED. The seed used to write nothing and count nothing, so a
    // reader trailing the writer by kReadDelay slipped one entry per seed.
    RxSytCadence cadence;
    cadence.Reset();

    RxSytCadence::Snapshot before{};
    ASSERT_TRUE(cadence.TrySnapshot(before));
    EXPECT_EQ(before.validUpdates, 0u);

    EXPECT_EQ(cadence.Observe(SytForFieldTicks(0), CycleTimer(0, 100, 0)),
              Update::kSeeded);

    RxSytCadence::Snapshot after{};
    ASSERT_TRUE(cadence.TrySnapshot(after));
    EXPECT_EQ(after.validUpdates, 1u) << "the seed must be counted";
    EXPECT_EQ(after.writeIndex, before.writeIndex + 1)
        << "the seed must advance the writer, or it desynchronises the reader";
    EXPECT_EQ(after.seedCount, 1u);
}

TEST(RxSytCadenceTests, SeedIsRateAdaptiveRatherThanANominalConstant) {
    // Saffire 0xd6ac writes the ring's own current running average, which
    // carries no bias at a rate whose real step differs from any constant we
    // could have picked.
    RxSytCadence cadence;
    cadence.Reset();
    const uint32_t next = FeedChain(cadence, 300, 0);

    RxSytCadence::Snapshot before{};
    ASSERT_TRUE(cadence.TrySnapshot(before));
    const uint32_t average =
        before.rollingCadenceTicks / RxSytCadence::kCadenceAverageEntries;

    // Break the chain, then seed it again.
    cadence.Observe(RxSytCadence::kNoInfo, CycleTimer(0, 500, 0));
    cadence.Reset();
    FeedChain(cadence, 300, 0);
    RxSytCadence::Snapshot reference{};
    ASSERT_TRUE(cadence.TrySnapshot(reference));
    const uint32_t expectedIncoming =
        reference.rollingCadenceTicks / RxSytCadence::kCadenceAverageEntries;

    EXPECT_EQ(average, expectedIncoming);
    EXPECT_EQ(cadence.ReadEntry(reference.writeIndex - 1), kStep)
        << "a settled chain averages to its own step";
    (void)next;
}

TEST(RxSytCadenceTests, NonAdvancingSytReseedsInsteadOfRejecting) {
    // DIVERGENCE FIXED. The old band returned "rejected" here and the caller
    // escalated it to a replay-epoch reset. The reference has no such test; its
    // only chain break is a DBC mismatch (0xd67c).
    RxSytCadence cadence;
    cadence.Reset();
    const uint32_t ticks = FeedChain(cadence, 40, 0);

    RxSytCadence::Snapshot before{};
    ASSERT_TRUE(cadence.TrySnapshot(before));

    // Repeat the previous SYT: delta is zero, not forward.
    const uint16_t repeated = SytForFieldTicks(ticks - kStep);
    EXPECT_EQ(cadence.Observe(repeated, CycleTimer(0, 200, 0)),
              Update::kSeeded);

    RxSytCadence::Snapshot after{};
    ASSERT_TRUE(cadence.TrySnapshot(after));
    EXPECT_EQ(after.validUpdates, before.validUpdates + 1);
    EXPECT_EQ(after.writeIndex, before.writeIndex + 1);
    // The chain's own first observation was already a seed, so this is the
    // second.
    EXPECT_EQ(after.seedCount, before.seedCount + 1);
    // And the ring absorbed the average, not a zero or a wrapped negative.
    EXPECT_EQ(cadence.ReadEntry(after.writeIndex - 1),
              before.rollingCadenceTicks / RxSytCadence::kCadenceAverageEntries);
}

TEST(RxSytCadenceTests, NoInfoDoesNotBreakTheChain) {
    // Blocking mode emits NO-DATA packets between data packets; they carry
    // SYT 0xFFFF and must leave the delta chain intact.
    RxSytCadence cadence;
    cadence.Reset();
    FeedChain(cadence, 20, 0);
    RxSytCadence::Snapshot before{};
    ASSERT_TRUE(cadence.TrySnapshot(before));

    EXPECT_EQ(cadence.Observe(RxSytCadence::kNoInfo, CycleTimer(0, 300, 0)),
              Update::kIgnored);

    RxSytCadence::Snapshot after{};
    ASSERT_TRUE(cadence.TrySnapshot(after));
    EXPECT_EQ(after.validUpdates, before.validUpdates);
    EXPECT_EQ(after.seedCount, before.seedCount);
    EXPECT_EQ(cadence.Observe(SytForFieldTicks(20 * kStep),
                              CycleTimer(0, 301, 0)),
              Update::kExtended)
        << "the chain must continue across a NO-DATA packet";
}

TEST(RxSytCadenceTests, EstablishesOnTheUpdateAfterTheHistoryFills) {
    RxSytCadence cadence;
    cadence.Reset();
    FeedChain(cadence, RxSytCadence::kWarmupUpdates - 1, 0);
    RxSytCadence::Snapshot snapshot{};
    ASSERT_TRUE(cadence.TrySnapshot(snapshot));
    EXPECT_FALSE(snapshot.established);

    FeedChain(cadence, 1, (RxSytCadence::kWarmupUpdates - 1) * kStep);
    ASSERT_TRUE(cadence.TrySnapshot(snapshot));
    EXPECT_TRUE(snapshot.established);
    EXPECT_EQ(snapshot.validUpdates, RxSytCadence::kWarmupUpdates);
}

TEST(RxSytCadenceTests, FramesForPhaseDeltaInvertsTheSettledCadence) {
    RxSytCadence cadence;
    cadence.Reset();
    FeedChain(cadence, RxSytCadence::kWarmupUpdates, 0);
    RxSytCadence::Snapshot snapshot{};
    ASSERT_TRUE(cadence.TrySnapshot(snapshot));
    ASSERT_TRUE(snapshot.established);

    int64_t frames = 0;
    ASSERT_TRUE(ASFW::Driver::FramesForPhaseDelta(
        kStep, snapshot.rollingCadenceTicks, 8, frames));
    EXPECT_EQ(frames, 8) << "one packet of phase is one packet of frames";

    ASSERT_TRUE(ASFW::Driver::FramesForPhaseDelta(
        10 * kStep, snapshot.rollingCadenceTicks, 8, frames));
    EXPECT_EQ(frames, 80);

    // Symmetric about zero: a leading and a lagging cursor are corrected by the
    // same magnitude, so repeated corrections cannot walk the cursor.
    ASSERT_TRUE(ASFW::Driver::FramesForPhaseDelta(
        -10 * static_cast<int64_t>(kStep), snapshot.rollingCadenceTicks, 8,
        frames));
    EXPECT_EQ(frames, -80);
}

TEST(RxSytCadenceTests, FramesForPhaseDeltaRefusesAnUnprimedRing) {
    int64_t frames = 12345;
    EXPECT_FALSE(ASFW::Driver::FramesForPhaseDelta(kStep, 0, 8, frames));
    EXPECT_FALSE(ASFW::Driver::FramesForPhaseDelta(kStep, 1024, 0, frames));
    EXPECT_EQ(frames, 12345) << "a refusal must not write a guess";
}

TEST(RxSytCadenceTests, FramesForPhaseDeltaFollowsTheMeasuredRateNotTheNominal) {
    // A device running 1/16 fast puts 4352 ticks between packets rather than
    // 4096. The same phase delta must then span fewer frames.
    RxSytCadence cadence;
    cadence.Reset();
    uint32_t ticks = 0;
    for (uint32_t i = 0; i < RxSytCadence::kWarmupUpdates; ++i) {
        cadence.Observe(SytForFieldTicks(ticks), CycleTimer(0, 100 + i, 0));
        ticks += 4352;
    }
    RxSytCadence::Snapshot snapshot{};
    ASSERT_TRUE(cadence.TrySnapshot(snapshot));
    EXPECT_EQ(snapshot.rollingCadenceTicks /
                  RxSytCadence::kCadenceAverageEntries, 4352u);

    int64_t frames = 0;
    ASSERT_TRUE(ASFW::Driver::FramesForPhaseDelta(
        10 * 4352, snapshot.rollingCadenceTicks, 8, frames));
    EXPECT_EQ(frames, 80);
}

}  // namespace
