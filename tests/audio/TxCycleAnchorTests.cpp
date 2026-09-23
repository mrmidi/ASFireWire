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

TEST(TxCycleAnchorTests, StampJustPastTheWindowBoundaryIsNotThrownBackEightSeconds) {
    // The mirror of StampAheadOfCorrelationBelongsToThePreviousWindow, and the
    // case observed on hardware as `[BackendTiming] noCycleAnchor` bursts:
    // clockPair is published at the TOP of a refill pass and the pass's
    // completion stamps at the BOTTOM, so the stamp legitimately leads its
    // correlation. When that lead straddles an eight-second boundary the
    // correlation still carries the OLD window, and OR-ing the stamp's low
    // three bits into it selects the window BELOW the truth.
    const uint32_t completion = CompletionStamp(48, 1);
    const uint32_t correlation = encodeCycleTimer(47, 7999, 0);

    int64_t ticks = 0;
    ASSERT_TRUE(TransmitPacketBusTicks(completion, correlation, 0, ticks));
    EXPECT_EQ(ticks, BusTicks(48, 1, 0));
    // The symptom: an anchor exactly one eight-second window early, which
    // UnwrapBusTicks then refuses against its high-water mark.
    EXPECT_NE(ticks, BusTicks(40, 1, 0));
}

TEST(TxCycleAnchorTests, WindowChoiceIsNearestInBothDirections) {
    // Sweep a stamp across an eight-second boundary against a correlation
    // pinned just below it. Every sample must resolve to its true second.
    const uint32_t correlationSecond = 47;
    for (uint32_t offsetCycles = 0; offsetCycles < 8; ++offsetCycles) {
        const uint32_t second = 48;
        int64_t ticks = 0;
        ASSERT_TRUE(TransmitPacketBusTicks(
            CompletionStamp(second, offsetCycles),
            encodeCycleTimer(correlationSecond, 7999, 0), 0, ticks))
            << "cycle " << offsetCycles;
        EXPECT_EQ(ticks, BusTicks(second, offsetCycles, 0))
            << "cycle " << offsetCycles;
    }
}

TEST(TxCycleAnchorTests, BoundaryStraddleStaysMonotonicForTheUnwrapper) {
    // End to end: the sequence UnwrapBusTicks actually sees while a refill pass
    // spans the boundary. A single backwards step here is one noCycleAnchor
    // burst on hardware.
    int64_t previous = -1;
    for (int step = -4; step <= 4; ++step) {
        const int64_t absoluteCycle =
            static_cast<int64_t>(48) * kCyclesPerSecond + step;
        const uint32_t second =
            static_cast<uint32_t>(absoluteCycle / kCyclesPerSecond);
        const uint32_t cycle =
            static_cast<uint32_t>(absoluteCycle % kCyclesPerSecond);
        // Correlation lags the stamp by one cycle: the publish race.
        const int64_t correlationCycle = absoluteCycle - 1;
        const uint32_t correlationSecond =
            static_cast<uint32_t>(correlationCycle / kCyclesPerSecond);
        int64_t ticks = 0;
        ASSERT_TRUE(TransmitPacketBusTicks(
            CompletionStamp(second, cycle),
            encodeCycleTimer(
                correlationSecond,
                static_cast<uint32_t>(correlationCycle % kCyclesPerSecond), 0),
            0, ticks)) << "step " << step;
        EXPECT_EQ(ticks, BusTicks(second, cycle, 0)) << "step " << step;
        EXPECT_GT(ticks, previous) << "step " << step;
        previous = ticks;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// ExpandCompletionAgainstCorrelation: the state contract.
//
// This path had no coverage at all, and it wedged on an M-Audio FireWire 1814:
// bursts of 1024, 4096 and finally 131,072 consecutive conversion failures over
// 414 seconds, which starved the zero-timestamp boundaries entirely. Instruments
// showed "No Data" for a 4.7-second capture while audio was still playing.

namespace {

using ASFW::Audio::Shared::ExpandCompletionAgainstCorrelation;
using ASFW::Audio::Shared::TxCorrelationUnwrapState;

struct Expansion final {
    bool ok{false};
    uint64_t completion{0};
    uint64_t correlation{0};
};

Expansion Expand(TxCorrelationUnwrapState& state,
                 uint32_t completionStamp,
                 uint32_t correlationTimer) {
    Expansion out{};
    out.ok = ExpandCompletionAgainstCorrelation(
        state, completionStamp, correlationTimer, out.completion,
        out.correlation);
    return out;
}

}  // namespace

TEST(TxCycleAnchorTests, ExpansionRecoversTheCompletionAgeFromTheCorrelation) {
    TxCorrelationUnwrapState state{};
    const auto r = Expand(state, CompletionStamp(6, 5520),
                          encodeCycleTimer(6, 5520, 1479));
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.correlation - r.completion, 1479u)
        << "the completion sits one correlation offset behind";
}

TEST(TxCycleAnchorTests, ExpansionAcceptsCompletionsWalkedNewestFirst) {
    // The M-Audio path reads the stamp queue backwards to find the freshest
    // DATA stamp, so completions descend under one correlation read. Enforcing
    // monotonicity on them here rejected almost every stamp.
    TxCorrelationUnwrapState state{};
    const uint32_t correlation = encodeCycleTimer(6, 5520, 1479);
    uint64_t previous = 0;
    for (const uint32_t cycle : {5520u, 5519u, 5517u, 5513u}) {
        const auto r = Expand(state, CompletionStamp(6, cycle), correlation);
        ASSERT_TRUE(r.ok) << "cycle " << cycle;
        if (previous != 0) EXPECT_LT(r.completion, previous);
        previous = r.completion;
    }
}

TEST(TxCycleAnchorTests, ExpansionSurvivesACompletionAheadOfItsCorrelation) {
    // THE REGRESSION. IsochTxDmaRing publishes clockPair at the top of a refill
    // pass and its stamps at the bottom, so a stamp can be newer than the
    // correlation it is paired with. The old code stored that completion as the
    // high-water mark the NEXT call's correlation was checked against, and
    // because the check returns before updating, the mark stayed in the future
    // and every later call failed until the bus clock caught up.
    TxCorrelationUnwrapState state{};
    const auto ahead = Expand(state, CompletionStamp(6, 5522),
                              encodeCycleTimer(6, 5520, 1479));
    ASSERT_TRUE(ahead.ok);
    EXPECT_GT(ahead.completion, ahead.correlation);

    // Everything that follows must still convert.
    for (uint32_t i = 1; i <= 8; ++i) {
        const auto r = Expand(state, CompletionStamp(6, 5520 + i),
                              encodeCycleTimer(6, 5520 + i, 1479));
        ASSERT_TRUE(r.ok) << "wedged " << i << " calls after the overshoot";
    }
}

TEST(TxCycleAnchorTests, ExpansionRejectsABackwardCorrelationWithoutWedging) {
    // A controller read that lands behind the mark is refused -- the caller
    // counts it -- but it must not poison the mark, or one bad read costs every
    // later conversion.
    TxCorrelationUnwrapState state{};
    ASSERT_TRUE(Expand(state, CompletionStamp(6, 5520),
                       encodeCycleTimer(6, 5520, 1479)).ok);
    EXPECT_FALSE(Expand(state, CompletionStamp(6, 5000),
                        encodeCycleTimer(6, 5000, 0)).ok);
    EXPECT_TRUE(Expand(state, CompletionStamp(6, 5521),
                       encodeCycleTimer(6, 5521, 0)).ok)
        << "the next forward read must recover on its own";
}

TEST(TxCycleAnchorTests, ExpansionStaysMonotoneAcrossTheEightSecondStamp) {
    // The stamp only carries seconds[2:0], so it repeats every eight seconds
    // while the correlation keeps counting to 128.
    TxCorrelationUnwrapState state{};
    uint64_t previous = 0;
    for (uint32_t seconds = 6; seconds <= 12; ++seconds) {
        const auto r = Expand(state, CompletionStamp(seconds, 100),
                              encodeCycleTimer(seconds, 100, 512));
        ASSERT_TRUE(r.ok) << "seconds " << seconds;
        EXPECT_GT(r.correlation, previous);
        EXPECT_EQ(r.correlation - r.completion, 512u);
        previous = r.correlation;
    }
}

TEST(TxCycleAnchorTests, ExpansionUnwrapsThe128SecondCorrelationWrap) {
    TxCorrelationUnwrapState state{};
    const auto before = Expand(state, CompletionStamp(127, 7999),
                               encodeCycleTimer(127, 7999, 0));
    ASSERT_TRUE(before.ok);
    const auto after = Expand(state, CompletionStamp(0, 0),
                              encodeCycleTimer(0, 0, 0));
    ASSERT_TRUE(after.ok) << "the wrap must lift, not be read as a backward step";
    EXPECT_GT(after.correlation, before.correlation);
    EXPECT_EQ(after.correlation - before.correlation,
              static_cast<uint64_t>(kTicksPerCycle));
}
