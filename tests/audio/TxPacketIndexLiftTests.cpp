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



} // namespace
