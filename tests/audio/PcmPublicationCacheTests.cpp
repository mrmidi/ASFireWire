#include "Audio/Runtime/PcmPublicationCache.hpp"
#include "Audio/Config/AudioConstants.hpp"
#include "Audio/Shared/AudioTimingGeometry.hpp"

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <vector>

namespace {
using ASFW::Audio::Ports::PcmCopyResult;
using ASFW::Audio::Runtime::PcmPublicationCache;
using ASFW::Audio::Runtime::PcmPublishResult;

TEST(PcmPublicationCacheTests, CopiesWrapAware4096FramePublication) {
    PcmPublicationCache cache{};
    ASSERT_TRUE(cache.Configure(2, 8192));
    cache.BeginEpoch(11);
    std::vector<float> host(8192 * 2);
    for (uint32_t frame = 0; frame < 4096; ++frame) {
        const uint64_t absolute = 7'000 + frame;
        const uint64_t physical = absolute % 8192;
        host[physical * 2] = static_cast<float>(absolute);
        host[physical * 2 + 1] = -static_cast<float>(absolute);
    }
    ASSERT_EQ(cache.Publish({host.data(), 11, 7'000, 4'096, 8'192, 2}),
              PcmPublishResult::Published);
    std::array<float, 16> output{};
    ASSERT_EQ(cache.CopyExact({11, 10'000, 8, 0, 2}, output.data(),
                              output.size()), PcmCopyResult::Ready);
    EXPECT_EQ(output[0], 10'000.0f);
    EXPECT_EQ(output[1], -10'000.0f);
}

TEST(PcmPublicationCacheTests, ReportsExpiryAfterOneRevolution) {
    PcmPublicationCache cache{};
    ASSERT_TRUE(cache.Configure(1, 16));
    cache.BeginEpoch(1);
    std::array<float, 32> host{};
    ASSERT_EQ(cache.Publish({host.data(), 1, 0, 16, 32, 1}),
              PcmPublishResult::Published);
    ASSERT_EQ(cache.Publish({host.data(), 1, 16, 16, 32, 1}),
              PcmPublishResult::Published);
    std::array<float, 4> output{};
    EXPECT_EQ(cache.CopyExact({1, 0, 4, 0, 1}, output.data(), output.size()),
              PcmCopyResult::Expired);
    EXPECT_EQ(cache.CopyExact({1, 16, 4, 0, 1}, output.data(), output.size()),
              PcmCopyResult::Ready);
}

TEST(PcmPublicationCacheTests, RejectsWrongEpochWithoutRebasing) {
    PcmPublicationCache cache{};
    ASSERT_TRUE(cache.Configure(1, 8192));
    cache.BeginEpoch(8);
    std::array<float, 8> host{};
    EXPECT_EQ(cache.Publish({host.data(), 7, 0, 8, 8, 1}),
              PcmPublishResult::WrongEpoch);
    std::array<float, 8> output{};
    EXPECT_EQ(cache.CopyExact({7, 0, 8, 0, 1}, output.data(), output.size()),
              PcmCopyResult::WrongEpoch);
    EXPECT_EQ(cache.Epoch(), 8U);
}

TEST(PcmPublicationCacheTests, EpochTransitionInvalidatesPublishedIdentity) {
    PcmPublicationCache cache{};
    ASSERT_TRUE(cache.Configure(1, 32));
    cache.BeginEpoch(8);
    std::array<float, 8> host{};
    ASSERT_EQ(cache.Publish({host.data(), 8, 0, 8, 8, 1}),
              PcmPublishResult::Published);
    cache.BeginEpoch(9);
    std::array<float, 8> output{};
    EXPECT_EQ(cache.CopyExact({8, 0, 8, 0, 1}, output.data(), output.size()),
              PcmCopyResult::WrongEpoch);
    EXPECT_EQ(cache.CopyExact({9, 0, 8, 0, 1}, output.data(), output.size()),
              PcmCopyResult::NotYetPublished);
}

TEST(PcmPublicationCacheTests, DuplicateDoesNotMutatePublishedBytes) {
    PcmPublicationCache cache{};
    ASSERT_TRUE(cache.Configure(1, 32));
    cache.BeginEpoch(2);
    std::array<float, 8> host{};
    host.fill(1.0f);
    ASSERT_EQ(cache.Publish({host.data(), 2, 0, 8, 8, 1}),
              PcmPublishResult::Published);
    host.fill(2.0f);
    ASSERT_EQ(cache.Publish({host.data(), 2, 0, 8, 8, 1}),
              PcmPublishResult::Duplicate);
    std::array<float, 8> output{};
    ASSERT_EQ(cache.CopyExact({2, 0, 8, 0, 1}, output.data(), output.size()),
              PcmCopyResult::Ready);
    EXPECT_EQ(output.front(), 1.0f);
}

TEST(PcmPublicationCacheTests, DetectsAFrameBeingConcurrentlyRewritten) {
    PcmPublicationCache cache{};
    ASSERT_TRUE(cache.Configure(1, 32));
    cache.BeginEpoch(4);
    std::array<float, 8> host{};
    ASSERT_EQ(cache.Publish({host.data(), 4, 0, 8, 8, 1}),
              PcmPublishResult::Published);
    cache.ForceFrameWritingForTest(3);
    std::array<float, 8> output{};
    EXPECT_EQ(cache.CopyExact({4, 0, 8, 0, 1}, output.data(), output.size()),
              PcmCopyResult::ConcurrentRewrite);
}

TEST(PcmPublicationCacheTests,
     MaximumChannel4096FramePublicationMeetsTenPercentBudget) {
    constexpr uint32_t kChannels = ASFW::Audio::Config::kMaxPcmChannels;
    constexpr uint32_t kFrames = 4'096;
    constexpr uint32_t kCapacity = 8'192;
    PcmPublicationCache cache{};
    ASSERT_TRUE(cache.Configure(kChannels, kCapacity));
    cache.BeginEpoch(5);
    std::vector<float> host(static_cast<size_t>(kCapacity) * kChannels, 0.25f);
    std::vector<uint64_t> durations;
    durations.reserve(25);
    for (uint64_t operation = 0; operation < 25; ++operation) {
        const auto start = std::chrono::steady_clock::now();
        ASSERT_EQ(cache.Publish({host.data(), 5, operation * kFrames,
                                 kFrames, kCapacity, kChannels}),
                  PcmPublishResult::Published);
        const auto end = std::chrono::steady_clock::now();
        durations.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
                .count()));
    }
    std::sort(durations.begin(), durations.end());
    const uint64_t p99 = durations.back();
    constexpr uint64_t kTenPercentOfAudioDurationNanos =
        (static_cast<uint64_t>(kFrames) * 1'000'000'000ULL / 192'000U) / 10U;
#if !defined(NDEBUG)
    GTEST_SKIP() << "strict RT budget requires an optimized build; debug p99="
                 << p99 << " ns";
#else
    EXPECT_LT(p99, kTenPercentOfAudioDurationNanos)
        << "p99 publication was " << p99 << " ns";
#endif
}

TEST(PcmPublicationCacheTests, ProductionCacheMatchesHalRevolution) {
    using Geometry = ASFW::Audio::Shared::AudioTimingGeometry;
    // 12288 since 2026-09-08: 8192 is not a whole number of AMDTP cadence
    // blocks at any rate, so the ZTS boundary walked the completion group.
    EXPECT_EQ(Geometry::kPcmPublicationCacheFrames, 12288U);
    EXPECT_EQ(Geometry::kFrameRingFrames, 12288U);
    EXPECT_EQ(Geometry::kHalZeroTimestampPeriodFrames, 12288U);
    // The cache is exactly one HAL revolution, not an independent size.
    EXPECT_EQ(Geometry::kPcmPublicationCacheFrames, Geometry::kFrameRingFrames);
}

} // namespace
