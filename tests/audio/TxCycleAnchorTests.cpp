// SPDX-License-Identifier: Apache-2.0
//
// Regression cover for the TX planning bus-time domain. A transmit plan's bus
// time is compared against, and subtracted from, the hardware timeline's RX
// presentation observations, which live in the cycle timer's full 128-second
// domain. Collapsing the plan into the eight-second OUTPUT_LAST domain instead
// made every DATA plan fail HardwareSampleTimeline::PreviewTxRange, so the
// stream emitted nothing but cadence NO-DATA and never consulted the PCM cache.

#include "Audio/Shared/TxCycleAnchor.hpp"

#include <gtest/gtest.h>

namespace {

using ASFW::Audio::Shared::TransmitPacketBusTicks;
using ASFW::Audio::Shared::kBusDomainTicks;
using ASFW::Timing::encodeCycleTimer;
using ASFW::Timing::kCyclesPerSecond;
using ASFW::Timing::kTicksPerCycle;
using ASFW::Timing::kTicksPerSecond;

// IsochTxDmaRing reconstructs the OUTPUT_LAST stamp at offset zero and can only
// recover cycleSeconds[2:0].
uint32_t CompletionStamp(uint32_t seconds, uint32_t cycle) {
    return encodeCycleTimer(seconds & 0x7u, cycle, 0);
}

int64_t BusTicks(uint32_t seconds, uint32_t cycle, uint32_t offset) {
    return static_cast<int64_t>(seconds) * static_cast<int64_t>(kTicksPerSecond) +
           static_cast<int64_t>(cycle) * static_cast<int64_t>(kTicksPerCycle) +
           offset;
}

TEST(TxCycleAnchorTests, LiftsThreeBitSecondsIntoTheFullBusDomain) {
    // Bus seconds 45: the completion stamp only carries 45 & 7 == 5.
    const uint32_t completion = CompletionStamp(45, 1000);
    const uint32_t correlation = encodeCycleTimer(45, 1200, 512);

    int64_t ticks = 0;
    ASSERT_TRUE(TransmitPacketBusTicks(completion, correlation, 0, ticks));
    EXPECT_EQ(ticks, BusTicks(45, 1000, 0));
}

TEST(TxCycleAnchorTests, ProjectsForwardByWholeCycles) {
    const uint32_t completion = CompletionStamp(45, 1000);
    const uint32_t correlation = encodeCycleTimer(45, 1200, 0);

    int64_t ticks = 0;
    ASSERT_TRUE(TransmitPacketBusTicks(completion, correlation, 144, ticks));
    EXPECT_EQ(ticks, BusTicks(45, 1144, 0));
}

TEST(TxCycleAnchorTests, StampAheadOfCorrelationBelongsToThePreviousWindow) {
    // Correlation has just rolled into second 48; the newest stamp is still in
    // second 47, which shares the low three bits with 55.
    const uint32_t completion = CompletionStamp(47, 7990);
    const uint32_t correlation = encodeCycleTimer(48, 5, 0);

    int64_t ticks = 0;
    ASSERT_TRUE(TransmitPacketBusTicks(completion, correlation, 0, ticks));
    EXPECT_EQ(ticks, BusTicks(47, 7990, 0));
    EXPECT_LT(ticks, BusTicks(48, 5, 0));
}

TEST(TxCycleAnchorTests, StaysMonotonicAcrossAnEightSecondWrap) {
    // The eight-second OUTPUT_LAST wrap must not appear in the result: the
    // whole run below is one continuous stretch of the 128-second domain.
    int64_t previous = -1;
    for (uint32_t second = 46; second <= 50; ++second) {
        for (uint32_t cycle = 0; cycle < kCyclesPerSecond; cycle += 1000) {
            int64_t ticks = 0;
            ASSERT_TRUE(TransmitPacketBusTicks(
                CompletionStamp(second, cycle),
                encodeCycleTimer(second, cycle + 100, 0), 0, ticks));
            EXPECT_EQ(ticks, BusTicks(second, cycle, 0));
            EXPECT_GT(ticks, previous);
            previous = ticks;
        }
    }
}

TEST(TxCycleAnchorTests, WrapsAtTheCycleTimerPeriodNotAtEightSeconds) {
    const uint32_t completion = CompletionStamp(127, 7990);
    const uint32_t correlation = encodeCycleTimer(127, 7995, 0);

    int64_t ticks = 0;
    ASSERT_TRUE(TransmitPacketBusTicks(completion, correlation, 20, ticks));
    // 7990 + 20 cycles crosses second 127's end, which is the 128-second wrap.
    EXPECT_EQ(ticks, BusTicks(0, 10, 0));
    EXPECT_GE(ticks, 0);
    EXPECT_LT(ticks, kBusDomainTicks);
}

TEST(TxCycleAnchorTests, ResultIsComparableWithReceiveObservationBusTicks) {
    // The RX consumer builds its observation from the full seven-bit seconds
    // field. A transmit plan scheduled 144 cycles ahead must land after it.
    const uint32_t seconds = 45;
    const uint32_t cycle = 3000;
    const int64_t receiveObservation = BusTicks(seconds, cycle, 0);

    int64_t ticks = 0;
    ASSERT_TRUE(TransmitPacketBusTicks(
        CompletionStamp(seconds, cycle),
        encodeCycleTimer(seconds, cycle + 2, 0), 144, ticks));
    EXPECT_GT(ticks, receiveObservation);
    EXPECT_EQ(ticks - receiveObservation,
              static_cast<int64_t>(144) * kTicksPerCycle);
}

TEST(TxCycleAnchorTests, MicrosecondCompletionLeadIsARaceNotAWindowWrap) {
    // IsochTxDmaRing publishes clockPair at the top of a refill pass and the
    // pass's completion stamps at the bottom, so a reader can legitimately see
    // a stamp that leads its correlation by the width of one pass. Treating
    // that as an eight-second wrap threw the plan 8 s into the past, and the
    // unwrapper -- which only stores on success -- then rejected every later
    // sample against a high-water mark it could no longer reach.
    const uint32_t completion = CompletionStamp(45, 1001);
    const uint32_t correlation = encodeCycleTimer(45, 1000, 0);

    int64_t ticks = 0;
    ASSERT_TRUE(TransmitPacketBusTicks(completion, correlation, 0, ticks));
    EXPECT_EQ(ticks, BusTicks(45, 1001, 0));
}

TEST(TxCycleAnchorTests, SuccessiveAnchorsStayMonotonicAcrossThePublishRace) {
    // What the unwrapper actually needs: never a backwards step, whichever
    // pass's correlation happens to be paired with a given stamp.
    int64_t previous = -1;
    for (uint32_t cycle = 1000; cycle < 1040; ++cycle) {
        // Alternate a fresh correlation with a stale one from the pass before.
        const uint32_t correlation = (cycle & 1u)
            ? encodeCycleTimer(45, cycle - 1, 0)
            : encodeCycleTimer(45, cycle + 1, 0);
        int64_t ticks = 0;
        ASSERT_TRUE(TransmitPacketBusTicks(
            CompletionStamp(45, cycle), correlation, 0, ticks));
        EXPECT_EQ(ticks, BusTicks(45, cycle, 0));
        EXPECT_GT(ticks, previous);
        previous = ticks;
    }
}

} // namespace
