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
    // Base 48 kHz geometry restored to 12288 frames; allocated capacity is 24576.
    EXPECT_EQ(Geometry::kPcmPublicationCacheFrames, 12288U);
    EXPECT_EQ(Geometry::kAllocatedPcmPublicationCacheFrames, 24576U);
    EXPECT_EQ(Geometry::kFrameRingFrames, 12288U);
    EXPECT_EQ(Geometry::kAllocatedFrameRingFrames, 24576U);
    EXPECT_EQ(Geometry::kHalZeroTimestampPeriodFrames, 12288U);

    // Active cache matches exactly one HAL revolution at both 48k and 96k.
    EXPECT_EQ(Geometry::PcmPublicationCacheFrames(48'000), Geometry::FrameRingFrames(48'000));
    EXPECT_EQ(Geometry::PcmPublicationCacheFrames(48'000), 12288U);
    EXPECT_EQ(Geometry::PcmPublicationCacheFrames(96'000), Geometry::FrameRingFrames(96'000));
    EXPECT_EQ(Geometry::PcmPublicationCacheFrames(96'000), 24576U);
}

TEST(PcmPublicationCacheTests, BackingMemoryPoisonAt48kHz) {
    // Sized for up to 96k (24576 frames), but active at 48k (12288 frames).
    constexpr uint32_t kAllocatedFrames = 24'576;
    constexpr uint32_t kActiveFrames = 12'288;
    constexpr uint32_t kChannels = 2;
    constexpr float kPoisonValue = -9999.0f;

    std::vector<float> backingMemory(static_cast<size_t>(kAllocatedFrames) * kChannels, kPoisonValue);

    // Stage storage for 48k active capacity
    auto staged = PcmPublicationCache::AllocateStorage(kChannels, kActiveFrames);
    ASSERT_TRUE(staged.has_value());

    PcmPublicationCache cache{};
    cache.CommitStorage(std::move(*staged));
    EXPECT_EQ(cache.ChannelCount(), kChannels);
    EXPECT_EQ(cache.CacheCapacityFrames(), kActiveFrames);

    cache.BeginEpoch(1);

    // Populate active region [0, kActiveFrames)
    for (uint32_t f = 0; f < kActiveFrames; ++f) {
        backingMemory[f * kChannels] = static_cast<float>(f);
        backingMemory[f * kChannels + 1] = -static_cast<float>(f);
    }

    // Publish from the active 12288 region
    ASSERT_EQ(cache.Publish({backingMemory.data(), 1, 0, kActiveFrames, kActiveFrames, kChannels}),
              PcmPublishResult::Published);

    // Verify copy within active region succeeds
    std::array<float, 4> output{};
    EXPECT_EQ(cache.CopyExact({1, 100, 2, 0, kChannels}, output.data(), output.size()),
              PcmCopyResult::Ready);
    EXPECT_EQ(output[0], 100.0f);
    EXPECT_EQ(output[1], -100.0f);

    // Verify uncommitted capacity [kActiveFrames, kAllocatedFrames) remains completely poisoned
    for (size_t i = static_cast<size_t>(kActiveFrames) * kChannels;
         i < static_cast<size_t>(kAllocatedFrames) * kChannels; ++i) {
        ASSERT_EQ(backingMemory[i], kPoisonValue);
    }
}

TEST(PcmPublicationCacheTests, DecoupledSourceAndCacheCapacityWrap) {
    constexpr uint32_t kCacheCapacity = 12'288;
    constexpr uint32_t kSourceCapacity = 12'288;
    constexpr uint32_t kChannels = 2;

    PcmPublicationCache cache{};
    ASSERT_TRUE(cache.Configure(kChannels, kCacheCapacity));
    cache.BeginEpoch(7);

    std::vector<float> hostRing(static_cast<size_t>(kSourceCapacity) * kChannels, 0.0f);

    // Publish a span that straddles the source wrap: absolute frames 11'000 to 13'048 (2048 frames)
    constexpr uint64_t kStartFrame = 11'000;
    constexpr uint32_t kSpanFrames = 2'048;
    for (uint32_t f = 0; f < kSpanFrames; ++f) {
        const uint64_t absolute = kStartFrame + f;
        const uint64_t physical = absolute % kSourceCapacity;
        hostRing[physical * kChannels] = static_cast<float>(absolute);
        hostRing[physical * kChannels + 1] = -static_cast<float>(absolute);
    }

    ASSERT_EQ(cache.Publish({hostRing.data(), 7, kStartFrame, kSpanFrames, kSourceCapacity, kChannels}),
              PcmPublishResult::Published);

    // Read before wrap: frame 12'000
    std::array<float, 2> outBefore{};
    EXPECT_EQ(cache.CopyExact({7, 12'000, 1, 0, kChannels}, outBefore.data(), outBefore.size()),
              PcmCopyResult::Ready);
    EXPECT_EQ(outBefore[0], 12'000.0f);
    EXPECT_EQ(outBefore[1], -12'000.0f);

    // Read after wrap: frame 12'500
    std::array<float, 2> outAfter{};
    EXPECT_EQ(cache.CopyExact({7, 12'500, 1, 0, kChannels}, outAfter.data(), outAfter.size()),
              PcmCopyResult::Ready);
    EXPECT_EQ(outAfter[0], 12'500.0f);
    EXPECT_EQ(outAfter[1], -12'500.0f);
}

TEST(PcmPublicationCacheTests, DynamicRateTransition48kTo96kTo48k) {
    constexpr uint32_t kChannels = 2;
    constexpr uint32_t kCapacity48k = 12'288;
    constexpr uint32_t kCapacity96k = 24'576;

    PcmPublicationCache cache{};

    // 1. Initial 48k configuration
    auto stage48k_1 = PcmPublicationCache::AllocateStorage(kChannels, kCapacity48k);
    ASSERT_TRUE(stage48k_1.has_value());
    cache.CommitStorage(std::move(*stage48k_1));
    cache.BeginEpoch(10);
    EXPECT_EQ(cache.CacheCapacityFrames(), kCapacity48k);

    std::vector<float> host48k(static_cast<size_t>(kCapacity48k) * kChannels, 48.0f);
    ASSERT_EQ(cache.Publish({host48k.data(), 10, 0, 1'000, kCapacity48k, kChannels}),
              PcmPublishResult::Published);

    std::array<float, 2> out{};
    EXPECT_EQ(cache.CopyExact({10, 500, 1, 0, kChannels}, out.data(), out.size()),
              PcmCopyResult::Ready);
    EXPECT_EQ(out[0], 48.0f);

    // 2. Dynamic transition to 96k
    auto stage96k = PcmPublicationCache::AllocateStorage(kChannels, kCapacity96k);
    ASSERT_TRUE(stage96k.has_value());
    // Simulate transaction commit
    cache.CommitStorage(std::move(*stage96k));
    cache.BeginEpoch(11);
    EXPECT_EQ(cache.CacheCapacityFrames(), kCapacity96k);

    // Old epoch is invalid
    EXPECT_EQ(cache.CopyExact({10, 500, 1, 0, kChannels}, out.data(), out.size()),
              PcmCopyResult::WrongEpoch);

    // Publish at 96k
    std::vector<float> host96k(static_cast<size_t>(kCapacity96k) * kChannels, 96.0f);
    ASSERT_EQ(cache.Publish({host96k.data(), 11, 0, 2'000, kCapacity96k, kChannels}),
              PcmPublishResult::Published);
    EXPECT_EQ(cache.CopyExact({11, 1'500, 1, 0, kChannels}, out.data(), out.size()),
              PcmCopyResult::Ready);
    EXPECT_EQ(out[0], 96.0f);

    // 3. Dynamic transition back to 48k
    auto stage48k_2 = PcmPublicationCache::AllocateStorage(kChannels, kCapacity48k);
    ASSERT_TRUE(stage48k_2.has_value());
    cache.CommitStorage(std::move(*stage48k_2));
    cache.BeginEpoch(12);
    EXPECT_EQ(cache.CacheCapacityFrames(), kCapacity48k);

    // 96k epoch is invalid
    EXPECT_EQ(cache.CopyExact({11, 1'500, 1, 0, kChannels}, out.data(), out.size()),
              PcmCopyResult::WrongEpoch);

    // Publish again at 48k
    ASSERT_EQ(cache.Publish({host48k.data(), 12, 10'000, 1'000, kCapacity48k, kChannels}),
              PcmPublishResult::Published);
    EXPECT_EQ(cache.CopyExact({12, 10'500, 1, 0, kChannels}, out.data(), out.size()),
              PcmCopyResult::Ready);
    EXPECT_EQ(out[0], 48.0f);
}

} // namespace
