#include "Isoch/Transmit/TxPacketIndexLift.hpp"

#include <gtest/gtest.h>

namespace {
using namespace ASFW::Isoch::Tx;

constexpr uint32_t kRing = 48;

[[nodiscard]] constexpr uint32_t Timer(uint32_t seconds, uint32_t cycle) {
    return (seconds << 25) | (cycle << 12);
}

TEST(TxPacketIndexLiftTests, CycleTimerDecodesSecondsAndCycle) {
    EXPECT_EQ(CycleTimerToCycles(Timer(0, 0)), 0U);
    EXPECT_EQ(CycleTimerToCycles(Timer(0, 7999)), 7999U);
    EXPECT_EQ(CycleTimerToCycles(Timer(1, 0)), 8000U);
    EXPECT_EQ(CycleTimerToCycles(Timer(7, 7999)), 63999U);
    // The offset field must not leak into the cycle count.
    EXPECT_EQ(CycleTimerToCycles(Timer(0, 5) | 0xFFFU), 5U);
}

TEST(TxPacketIndexLiftTests, ElapsedCyclesSurviveTheSecondsWrap) {
    EXPECT_EQ(CyclesBetween(Timer(0, 0), Timer(0, 48)), 48U);
    EXPECT_EQ(CyclesBetween(Timer(0, 7990), Timer(1, 10)), 20U);
    // 7 -> 0 is the three-bit seconds field wrapping, not time going backwards.
    EXPECT_EQ(CyclesBetween(Timer(7, 7990), Timer(0, 10)), 20U);
}

TEST(TxPacketIndexLiftTests, ResolvesTheLapTheRingSlotCannotCarry) {
    // Slot 5 with the controller believed to be near packet 5: lap 0.
    EXPECT_EQ(LiftRingSlotToAbsolute(5, 5, kRing), 5U);
    // The same slot two laps on resolves to two laps on, which is the whole
    // point: the slot is identical and only the expectation separates them.
    EXPECT_EQ(LiftRingSlotToAbsolute(5, 101, kRing), 101U);
    EXPECT_EQ(LiftRingSlotToAbsolute(5, 96 + 5, kRing), 101U);
}

TEST(TxPacketIndexLiftTests, AnExpectationOffByUnderHalfALapStillResolves) {
    // True index 101 (lap 2, slot 5). Anywhere strictly inside half a ring
    // below, and up to half a ring above, must land on 101.
    for (uint64_t expected = 101 - (kRing / 2 - 1); expected <= 101 + kRing / 2;
         ++expected) {
        EXPECT_EQ(LiftRingSlotToAbsolute(5, expected, kRing), 101U)
            << "expected=" << expected;
    }
}

TEST(TxPacketIndexLiftTests, ExactlyHalfALapIsAmbiguousAndResolvesDownward) {
    // 77 is equidistant from 53 and 101, so no rule can prefer one on the
    // evidence. The tie is broken downward; what matters is that the caller's
    // expectation must be better than half a lap, not merely close.
    EXPECT_EQ(LiftRingSlotToAbsolute(5, 77, kRing), 53U);
    EXPECT_EQ(LiftRingSlotToAbsolute(5, 78, kRing), 101U);
}

TEST(TxPacketIndexLiftTests, DoesNotUnderflowBelowTheFirstLap) {
    // Early in the stream an over-large slot must not resolve to a negative
    // lap; there is no packet before zero.
    EXPECT_EQ(LiftRingSlotToAbsolute(47, 0, kRing), 47U);
    EXPECT_EQ(LiftRingSlotToAbsolute(47, 1, kRing), 47U);
}

TEST(TxPacketIndexLiftTests, ADegenerateRingIsLeftAlone) {
    EXPECT_EQ(LiftRingSlotToAbsolute(5, 77, 0), 77U);
}

// The defect this exists to prevent, stated as a test: a first observation
// taken more than a lap after the context started used to be attributed to lap
// zero, and the resulting offset never corrected itself.
TEST(TxPacketIndexLiftTests, LateFirstObservationIsNotAttributedToLapZero) {
    const uint32_t start = Timer(0, 100);
    // 42 ms of startup delay is 336 cycles: seven laps of a 48-packet ring.
    const uint32_t now = Timer(0, 100 + 336);
    const uint32_t elapsed = CyclesBetween(start, now);
    ASSERT_EQ(elapsed, 336U);

    const uint32_t ringSlot = elapsed % kRing;   // what the CommandPtr shows
    EXPECT_EQ(ringSlot, 0U);
    // Naive accumulation from zero would call this packet 0.
    EXPECT_EQ(LiftRingSlotToAbsolute(ringSlot, elapsed, kRing), 336U);
}


// --- RecoverConsumedDelta -------------------------------------------------
//
// The regression these guard is the one that produced the k*288-frame latency
// lattice: a slot difference cannot express a lap, so a completion cursor
// advanced by accumulating slot differences drops 48 packets per missed lap and
// never recovers them.

TEST(TxPacketIndexLiftTests, ShortAdvancesAreUnchangedByRecovery) {
    // The naive difference is already exact here, and recovery must not move it.
    EXPECT_EQ(RecoverConsumedDelta(10, 12, 2, kRing), 2U);
    EXPECT_EQ(RecoverConsumedDelta(0, 47, 47, kRing), 47U);
    // Wrapping the ring without lapping it.
    EXPECT_EQ(RecoverConsumedDelta(40, 5, 13, kRing), 13U);
    // No advance at all.
    EXPECT_EQ(RecoverConsumedDelta(7, 7, 0, kRing), 0U);
}

TEST(TxPacketIndexLiftTests, AWholeLapBetweenObservationsIsRecovered) {
    // Slot moved 10 -> 12, but 50 cycles elapsed: the controller lapped once and
    // the slot difference alone reports 2.
    EXPECT_EQ(RecoverConsumedDelta(10, 12, 50, kRing), 50U);
    // Exactly one lap, same slot: the difference is 0 and the truth is 48.
    EXPECT_EQ(RecoverConsumedDelta(7, 7, 48, kRing), 48U);
    // Several laps accumulate rather than folding.
    EXPECT_EQ(RecoverConsumedDelta(10, 12, 2 + 3 * kRing, kRing), 2U + 3U * kRing);
}

TEST(TxPacketIndexLiftTests, RecoveryNeverRemovesPacketsTheSlotsProve) {
    // Elapsed cycles is an upper bound on descriptor progress, never a lower
    // one: a reading that suggests less advance than the slots show must not
    // shrink the delta, or the cursor would run ahead of the hardware.
    EXPECT_EQ(RecoverConsumedDelta(10, 20, 0, kRing), 10U);
    EXPECT_EQ(RecoverConsumedDelta(10, 20, 3, kRing), 10U);
    EXPECT_EQ(RecoverConsumedDelta(40, 5, 1, kRing), 13U);
}

TEST(TxPacketIndexLiftTests, SkippedCyclesUnderHalfARingDoNotInventALap) {
    // Self-linked skip addresses mean a cycle can pass without the context
    // advancing, so elapsed cycles overshoots. Below half a ring of overshoot
    // the nearest-congruent rule must absorb it and report the true advance.
    for (uint32_t skipped = 0; skipped < kRing / 2; ++skipped) {
        EXPECT_EQ(RecoverConsumedDelta(10, 12, 2 + skipped, kRing), 2U)
            << "skippedCycles=" << skipped;
        EXPECT_EQ(RecoverConsumedDelta(10, 12, 50 + skipped, kRing), 50U)
            << "skippedCycles=" << skipped << " (one lap lost)";
    }
}

TEST(TxPacketIndexLiftTests, RecoveryIsAlwaysCongruentWithTheObservedSlot) {
    // Whatever the cycle evidence says, the answer must land the cursor on the
    // slot the controller actually reported; anything else is a new bug class.
    for (uint32_t prev = 0; prev < kRing; prev += 7) {
        for (uint32_t now = 0; now < kRing; now += 5) {
            for (uint32_t cycles : {0U, 1U, 47U, 48U, 49U, 200U, 1000U}) {
                const uint64_t delta =
                    RecoverConsumedDelta(prev, now, cycles, kRing);
                EXPECT_EQ((prev + delta) % kRing, now)
                    << "prev=" << prev << " now=" << now << " cycles=" << cycles;
            }
        }
    }
}

TEST(TxPacketIndexLiftTests, RecoveryToleratesADegenerateRing) {
    EXPECT_EQ(RecoverConsumedDelta(0, 0, 100, 0), 0U);
}


// --- SplitCompletionWalk --------------------------------------------------
//
// Guards the regression that killed a healthy stream on 2026-09-06: a
// lap-recovered delta of 58 against a 48-packet ring made the completion walk
// inspect more than one lap, and the slots older than the last lap had been
// recycled, so their seals no longer matched. The walk reported a producer fault
// and the context fatal-stopped.

TEST(TxPacketIndexLiftTests, ShortDeltasAreWalkedWhole) {
    for (uint32_t d : {0U, 1U, 10U, 47U, kRing}) {
        const auto s = SplitCompletionWalk(d, kRing);
        EXPECT_EQ(s.abandoned, 0U) << "delta=" << d;
        EXPECT_EQ(s.walked, d) << "delta=" << d;
    }
}

TEST(TxPacketIndexLiftTests, ALappedDeltaWalksOnlyTheMostRecentLap) {
    // The exact hardware case: 58 consumed against a 48-packet ring.
    const auto s = SplitCompletionWalk(58, kRing);
    EXPECT_EQ(s.walked, kRing);
    EXPECT_EQ(s.abandoned, 10U);

    const auto two = SplitCompletionWalk(2 * kRing + 5, kRing);
    EXPECT_EQ(two.walked, kRing);
    EXPECT_EQ(two.abandoned, kRing + 5);
}

TEST(TxPacketIndexLiftTests, TheSplitNeverLosesOrInventsPackets) {
    // The cursor advances by the full delta, so the two halves must account for
    // all of it: anything else silently moves the completion coordinate.
    for (uint32_t d = 0; d < 4 * kRing; ++d) {
        const auto s = SplitCompletionWalk(d, kRing);
        EXPECT_EQ(s.abandoned + s.walked, d) << "delta=" << d;
        EXPECT_LE(s.walked, kRing) << "delta=" << d;
    }
}

TEST(TxPacketIndexLiftTests, ADegenerateRingWalksEverything) {
    const auto s = SplitCompletionWalk(58, 0);
    EXPECT_EQ(s.abandoned, 0U);
    EXPECT_EQ(s.walked, 58U);
}

} // namespace
