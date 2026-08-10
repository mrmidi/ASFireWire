#include "Audio/Runtime/TxPcmStagingRing.hpp"
#include "Audio/Shared/AudioTimingGeometry.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using ASFW::Audio::Ports::TxPcmReadRequest;
using ASFW::Audio::Ports::TxPcmReadResult;
using ASFW::Audio::Runtime::TxPcmStageResult;
using ASFW::Audio::Runtime::TxPcmStagingRing;

TEST(TxPcmStagingRingTests, RetainsCaptured1984FrameStepBeyondHalRing) {
    TxPcmStagingRing ring{};
    ASSERT_TRUE(ring.Configure(2, 4096));
    constexpr uint32_t kHostCapacity = 1536;
    std::vector<float> host(kHostCapacity * 2);

    for (uint64_t start = 0; start < 1984; start += 512) {
        const uint32_t count =
            static_cast<uint32_t>(std::min<uint64_t>(512, 1984 - start));
        for (uint32_t frame = 0; frame < count; ++frame) {
            const uint64_t absolute = start + frame;
            const uint64_t slot = absolute % kHostCapacity;
            host[slot * 2] = static_cast<float>(absolute + 1);
            host[slot * 2 + 1] = -static_cast<float>(absolute + 1);
        }
        ASSERT_EQ(ring.Stage({host.data(), start, count,
                              kHostCapacity, 2}),
                  TxPcmStageResult::kStaged);
    }

    // Host slot zero now belongs to frame 1536, but staging still owns frame 0.
    ASSERT_EQ(host[0], 1537.0f);
    std::array<float, 16> snapshot{};
    ASSERT_EQ(ring.ReadFloat32Interleaved(
                  {0, 8, 0, 2}, snapshot.data(), snapshot.size()),
              TxPcmReadResult::kReady);
    EXPECT_EQ(snapshot[0], 1.0f);
    EXPECT_EQ(snapshot[1], -1.0f);
    EXPECT_EQ(snapshot[14], 8.0f);
    EXPECT_EQ(snapshot[15], -8.0f);
    EXPECT_EQ(ring.WrittenEndFrame(), 1984U);
}

TEST(TxPcmStagingRingTests, WrapClassifiesOldFramesAsStale) {
    TxPcmStagingRing ring{};
    ASSERT_TRUE(ring.Configure(1, 16));
    std::array<float, 32> host{};
    for (uint32_t index = 0; index < host.size(); ++index) {
        host[index] = static_cast<float>(index);
    }
    ASSERT_EQ(ring.Stage({host.data(), 0, 32, 32, 1}),
              TxPcmStageResult::kStaged);
    EXPECT_EQ(ring.OldestValidFrame(), 16U);

    std::array<float, 8> out{};
    EXPECT_EQ(ring.ReadFloat32Interleaved(
                  {8, 8, 0, 1}, out.data(), out.size()),
              TxPcmReadResult::kStaleOverwritten);
    EXPECT_EQ(ring.ReadFloat32Interleaved(
                  {16, 8, 0, 1}, out.data(), out.size()),
              TxPcmReadResult::kReady);
    EXPECT_EQ(out.front(), 16.0f);
}

TEST(TxPcmStagingRingTests, DiscontinuityDoesNotInventGapFrames) {
    TxPcmStagingRing ring{};
    ASSERT_TRUE(ring.Configure(1, 64));
    std::array<float, 8> host{};
    host.fill(1.0f);
    ASSERT_EQ(ring.Stage({host.data(), 0, 8, 8, 1}),
              TxPcmStageResult::kStaged);
    host.fill(2.0f);
    ASSERT_EQ(ring.Stage({host.data(), 100, 8, 8, 1}),
              TxPcmStageResult::kStaged);
    EXPECT_EQ(ring.OldestValidFrame(), 100U);
    std::array<float, 8> out{};
    EXPECT_EQ(ring.ReadFloat32Interleaved(
                  {8, 8, 0, 1}, out.data(), out.size()),
              TxPcmReadResult::kStaleOverwritten);
    EXPECT_EQ(ring.ReadFloat32Interleaved(
                  {100, 8, 0, 1}, out.data(), out.size()),
              TxPcmReadResult::kReady);
    EXPECT_EQ(out.front(), 2.0f);
}

TEST(TxPcmStagingRingTests, OverlapCopiesOnlyNewSuffix) {
    TxPcmStagingRing ring{};
    ASSERT_TRUE(ring.Configure(1, 64));
    std::array<float, 16> host{};
    for (uint32_t index = 0; index < host.size(); ++index) {
        host[index] = static_cast<float>(index + 1);
    }
    ASSERT_EQ(ring.Stage({host.data(), 0, 8, 16, 1}),
              TxPcmStageResult::kStaged);
    host[4] = 99.0f;
    ASSERT_EQ(ring.Stage({host.data(), 4, 8, 16, 1}),
              TxPcmStageResult::kStaged);
    std::array<float, 12> out{};
    ASSERT_EQ(ring.ReadFloat32Interleaved(
                  {0, 12, 0, 1}, out.data(), out.size()),
              TxPcmReadResult::kReady);
    EXPECT_EQ(out[4], 5.0f); // already-published prefix was immutable
    EXPECT_EQ(out[8], 9.0f);
}

TEST(TxPcmStagingRingTests,
     OutOfRangeChannelWindowIsRejectedWithoutSynthesizingSilence) {
    TxPcmStagingRing ring{};
    ASSERT_TRUE(ring.Configure(2, 16));
    std::array<float, 16> host{};
    host.fill(0.5f);
    ASSERT_EQ(ring.Stage({host.data(), 0, 8, 8, 2}),
              TxPcmStageResult::kStaged);
    std::array<float, 16> out{};
    EXPECT_EQ(ring.ReadFloat32Interleaved(
                  {0, 8, 1, 2}, out.data(), out.size()),
              TxPcmReadResult::kInvalidRequest);
}

TEST(TxPcmStagingRingTests,
     StageRejectsHostChannelGeometryMismatch) {
    TxPcmStagingRing ring{};
    ASSERT_TRUE(ring.Configure(2, 16));
    std::array<float, 8> monoHost{};
    monoHost.fill(0.5f);
    EXPECT_EQ(ring.Stage({monoHost.data(), 0, 8, 8, 1}),
              TxPcmStageResult::kInvalidView);
    EXPECT_EQ(ring.WrittenEndFrame(), 0U);
}

TEST(TxPcmStagingRingTests, ProductionCapacityCoversMaximumRateHorizon) {
    using Geometry = ASFW::Audio::Shared::AudioTimingGeometry;
    EXPECT_GE(Geometry::kTxPcmStagingFrames,
              Geometry::TxDataHorizonFrames(192000) +
                  2 * Geometry::kFrameRingFrames);
}

TEST(TxPcmStagingRingTests, ConcurrentSnapshotsAreWholeOrRetryable) {
    TxPcmStagingRing ring{};
    ASSERT_TRUE(ring.Configure(2, 1024));
    std::vector<float> host(1024 * 2);
    std::atomic<bool> producerDone{false};
    std::atomic<uint64_t> readyReads{0};
    std::atomic<uint64_t> inconsistentReads{0};

    std::thread producer([&] {
        for (uint64_t start = 0; start < 32'768; start += 32) {
            for (uint32_t frame = 0; frame < 32; ++frame) {
                const uint64_t absolute = start + frame;
                const uint64_t slot = absolute % 1024;
                const float value = static_cast<float>(absolute + 1);
                host[slot * 2] = value;
                host[slot * 2 + 1] = -value;
            }
            (void)ring.Stage({host.data(), start, 32, 1024, 2});
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        std::array<float, 16> out{};
        while (!producerDone.load(std::memory_order_acquire)) {
            const uint64_t end = ring.WrittenEndFrame();
            if (end < 8) continue;
            const uint64_t first = end - 8;
            const auto result = ring.ReadFloat32Interleaved(
                {first, 8, 0, 2}, out.data(), out.size());
            if (result != TxPcmReadResult::kReady) continue;
            ++readyReads;
            for (uint32_t frame = 0; frame < 8; ++frame) {
                if (out[frame * 2 + 1] != -out[frame * 2]) {
                    ++inconsistentReads;
                }
            }
        }
    });

    producer.join();
    consumer.join();
    EXPECT_GT(readyReads.load(), 0U);
    EXPECT_EQ(inconsistentReads.load(), 0U);
}

TEST(TxPcmStagingRingTests,
     ConcurrentAppendDoesNotInvalidateRetainedPrefixSnapshot) {
    constexpr uint32_t kChannels = 32;
    constexpr uint32_t kCapacity = 4096;
    TxPcmStagingRing ring{};
    ASSERT_TRUE(ring.Configure(kChannels, kCapacity));
    std::vector<float> host(kCapacity * kChannels, 0.25f);
    ASSERT_EQ(ring.Stage({host.data(), 0, 128, kCapacity, kChannels}),
              TxPcmStageResult::kStaged);

    std::atomic<bool> producerDone{false};
    std::atomic<uint64_t> nonReadyReads{0};
    std::thread producer([&] {
        for (uint64_t start = 128; start < 2048; start += 128) {
            (void)ring.Stage(
                {host.data(), start, 128, kCapacity, kChannels});
        }
        producerDone.store(true, std::memory_order_release);
    });
    std::thread consumer([&] {
        std::array<float, 32> out{};
        while (!producerDone.load(std::memory_order_acquire)) {
            if (ring.ReadFloat32Interleaved(
                    {0, 1, 0, kChannels}, out.data(), out.size()) !=
                TxPcmReadResult::kReady) {
                nonReadyReads.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    producer.join();
    consumer.join();
    EXPECT_EQ(nonReadyReads.load(), 0U);
}

} // namespace
