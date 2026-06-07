#include "Audio/Runtime/PlaybackRingRange.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace {

using ASFW::Audio::Runtime::UpdatePlaybackRingRange;

TEST(PlaybackRingRangeTests, FirstWriteEstablishesActualValidStart) {
    const auto update = UpdatePlaybackRingRange(0, 0, 1000, 1192, 0, 2048);
    EXPECT_EQ(update.oldestValidFrame, 1000U);
    EXPECT_EQ(update.writtenEndFrame, 1192U);
    EXPECT_FALSE(update.discontinuity);
    EXPECT_FALSE(update.overrun);
}

TEST(PlaybackRingRangeTests, ContiguousWritesRetainOldestUntilCapacityWrap) {
    auto update = UpdatePlaybackRingRange(1192, 1000, 1192, 1320, 1100, 256);
    EXPECT_EQ(update.oldestValidFrame, 1064U);
    EXPECT_FALSE(update.discontinuity);

    update = UpdatePlaybackRingRange(1320, update.oldestValidFrame,
                                     1320, 1448, 1200, 256);
    EXPECT_EQ(update.oldestValidFrame, 1192U);
    EXPECT_FALSE(update.discontinuity);
}

TEST(PlaybackRingRangeTests, DiscontinuousWriteResetsRangeStart) {
    const auto update = UpdatePlaybackRingRange(1320, 1064, 2000, 2128, 1300, 512);
    EXPECT_EQ(update.oldestValidFrame, 2000U);
    EXPECT_EQ(update.writtenEndFrame, 2128U);
    EXPECT_TRUE(update.discontinuity);
    EXPECT_TRUE(update.overrun);
}

TEST(PlaybackRingRangeTests, NonAdvancingAndOverflowedWritesAreIgnored) {
    const auto stale = UpdatePlaybackRingRange(100, 20, 40, 80, 50, 64);
    EXPECT_EQ(stale.oldestValidFrame, 20U);
    EXPECT_EQ(stale.writtenEndFrame, 100U);

    constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
    const auto invalid = UpdatePlaybackRingRange(kMax - 4, kMax - 20,
                                                 kMax - 2, 1, kMax - 10, 64);
    EXPECT_EQ(invalid.writtenEndFrame, kMax - 4);
}

} // namespace
