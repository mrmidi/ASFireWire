#include "Audio/Runtime/AudioLedgerIntervals.hpp"

#include <gtest/gtest.h>

namespace {
using namespace ASFW::Audio::Runtime;

TEST(LedgerIntervalBucketTests, LadderStartsAtOneIsochCycleAndEndsInOverflow) {
    // The boundaries are inclusive upper bounds, so a value sitting exactly on
    // one belongs to the bucket it names, not the next.
    EXPECT_EQ(LedgerIntervalBucket(0), 0U);
    EXPECT_EQ(LedgerIntervalBucket(125), 0U);
    EXPECT_EQ(LedgerIntervalBucket(126), 1U);
    EXPECT_EQ(LedgerIntervalBucket(1000), 3U);
    EXPECT_EQ(LedgerIntervalBucket(1001), 4U);
    EXPECT_EQ(LedgerIntervalBucket(8000), 6U);
    EXPECT_EQ(LedgerIntervalBucket(8001), kLedgerIntervalBuckets - 1);
    EXPECT_EQ(LedgerIntervalBucket(~uint64_t{0}), kLedgerIntervalBuckets - 1);
}

TEST(LedgerIntervalStatsTests, SummarisesWhatItWasGiven) {
    LedgerIntervalStats stats{};
    for (const uint64_t micros : {100U, 900U, 300U, 2500U}) stats.Record(micros);

    EXPECT_EQ(stats.samples.load(), 4U);
    EXPECT_EQ(stats.minMicros.load(), 100U);
    EXPECT_EQ(stats.maxMicros.load(), 2500U);
    EXPECT_EQ(stats.MeanMicros(), (100U + 900U + 300U + 2500U) / 4U);
    EXPECT_EQ(stats.histogram[0].load(), 1U);  // 100
    EXPECT_EQ(stats.histogram[2].load(), 1U);  // 300
    EXPECT_EQ(stats.histogram[3].load(), 1U);  // 900
    EXPECT_EQ(stats.histogram[5].load(), 1U);  // 2500
    EXPECT_FALSE(stats.SawOverflow());
}

TEST(LedgerIntervalStatsTests, OverflowIsWhatTheHeartbeatWakesOn) {
    LedgerIntervalStats stats{};
    stats.Record(8000);
    EXPECT_FALSE(stats.SawOverflow());
    stats.Record(8001);
    EXPECT_TRUE(stats.SawOverflow());
}

TEST(LedgerIntervalStatsTests, AnEmptyDistributionReportsNothingRatherThanZero) {
    LedgerIntervalStats stats{};
    EXPECT_EQ(stats.MeanMicros(), 0U);
    // A sentinel, so an untouched interval cannot be read as "always 0 us".
    EXPECT_EQ(stats.minMicros.load(), ~uint64_t{0});
}

TEST(LedgerIntervalStatsTests, UnresolvedSamplesAreCountedNotDropped) {
    LedgerIntervalStats stats{};
    stats.Record(400);
    stats.CountUnresolved();
    stats.CountUnresolved();
    // Two of three observations could not be resolved, and the histogram says
    // so instead of presenting one sample as the whole story.
    EXPECT_EQ(stats.samples.load(), 1U);
    EXPECT_EQ(stats.unresolved.load(), 2U);
}

TEST(LedgerStampRingTests, ReturnsTheRecordThatFirstCoveredTheValue) {
    LedgerStampRing ring{};
    ring.Record(100, 1000);
    ring.Record(200, 2000);
    ring.Record(300, 3000);

    uint64_t ticks = 0;
    EXPECT_TRUE(ring.CoveredAt(50, ticks));
    EXPECT_EQ(ticks, 1000U);
    EXPECT_TRUE(ring.CoveredAt(99, ticks));
    EXPECT_EQ(ticks, 1000U);
    // 100 is end-exclusive: the record covering frame 100 is the next one.
    EXPECT_TRUE(ring.CoveredAt(100, ticks));
    EXPECT_EQ(ticks, 2000U);
    EXPECT_TRUE(ring.CoveredAt(250, ticks));
    EXPECT_EQ(ticks, 3000U);
}

TEST(LedgerStampRingTests, AValueNotYetCoveredIsUnresolvedNotTheNewestRecord) {
    LedgerStampRing ring{};
    ring.Record(100, 1000);
    uint64_t ticks = 12345;
    EXPECT_FALSE(ring.CoveredAt(500, ticks));
    EXPECT_EQ(ticks, 12345U) << "output must be left alone when unresolved";
}

TEST(LedgerStampRingTests, AnEmptyRingResolvesNothing) {
    LedgerStampRing ring{};
    uint64_t ticks = 0;
    EXPECT_FALSE(ring.CoveredAt(0, ticks));
}

TEST(LedgerStampRingTests, AgedOutValuesAreRefusedRatherThanUnderstated) {
    LedgerStampRing ring{};
    // Two full laps, so everything below the last kLedgerStampSlots records is
    // gone. Answering those from the oldest surviving entry would understate
    // precisely the long intervals worth measuring.
    for (uint64_t i = 1; i <= kLedgerStampSlots * 2; ++i) {
        ring.Record(i * 10, i * 100);
    }
    uint64_t ticks = 0;
    EXPECT_FALSE(ring.CoveredAt(5, ticks));

    // The oldest surviving record is itself not a usable answer: a discarded
    // earlier record may have covered the value first, and there is no way to
    // tell from what remains.
    const uint64_t oldestRetainedCursor = (kLedgerStampSlots + 1) * 10;
    EXPECT_FALSE(ring.CoveredAt(oldestRetainedCursor - 1, ticks));

    // One past it is unambiguous: its coverer is the second surviving record,
    // and everything that decides the answer is still retained.
    EXPECT_TRUE(ring.CoveredAt(oldestRetainedCursor, ticks));
    EXPECT_EQ(ticks, (kLedgerStampSlots + 2) * 100);
}

TEST(LedgerStampRingTests, ResetForgetsEverything) {
    LedgerStampRing ring{};
    ring.Record(100, 1000);
    ring.Reset();
    uint64_t ticks = 0;
    EXPECT_FALSE(ring.CoveredAt(50, ticks));
}

} // namespace
