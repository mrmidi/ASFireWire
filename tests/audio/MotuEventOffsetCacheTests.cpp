// SPDX-License-Identifier: Apache-2.0
//
// MotuEventOffsetCache tests.
//
// The cache is the per-data-block half of MOTU's replay contract: capture fills one
// offset per received block (amdtp-motu.c:303-329), playback drains one per transmitted
// block (amdtp-motu.c:373-393). These pin that pairing, the one-second wrap, and the
// all-or-nothing drain that keeps a stale SPH off the wire.

#include "Audio/Wire/MOTU/MotuEventOffsetCache.hpp"
#include "Audio/Wire/MOTU/MotuTxTiming.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {

using ASFW::Encoding::Motu::BaseTickForCycle;
using ASFW::Encoding::Motu::kTicksPerCycle;
using ASFW::Encoding::Motu::kTicksPerSecond;
using ASFW::Encoding::Motu::MotuEventOffsetCache;
using ASFW::Encoding::Motu::ReadSph;
using ASFW::Encoding::Motu::SphFromTick;
using ASFW::Encoding::Motu::TickFromSph;
using ASFW::Encoding::Motu::WritePacketSph;

constexpr uint32_t kCipHeaderBytes = 8;
constexpr uint32_t kDbs = 4;
constexpr uint32_t kBlockBytes = kDbs * 4;

/// Build a received packet whose blocks carry SPH values at `baseTick + offsets[i]`.
[[nodiscard]] std::vector<uint8_t> MakeRxPacket(uint32_t baseTick,
                                                const std::vector<uint32_t>& offsets) {
    std::vector<uint8_t> bytes(kCipHeaderBytes + offsets.size() * kBlockBytes, 0u);
    for (size_t i = 0; i < offsets.size(); ++i) {
        const uint32_t sph = SphFromTick((baseTick + offsets[i]) % kTicksPerSecond);
        uint8_t* b = bytes.data() + kCipHeaderBytes + i * kBlockBytes;
        b[0] = static_cast<uint8_t>(sph >> 24);
        b[1] = static_cast<uint8_t>(sph >> 16);
        b[2] = static_cast<uint8_t>(sph >> 8);
        b[3] = static_cast<uint8_t>(sph);
    }
    return bytes;
}

//==============================================================================

TEST(MotuEventOffsetCacheTests, CapturesOneOffsetPerDataBlock) {
    MotuEventOffsetCache cache{};
    const std::vector<uint32_t> offsets{0u, 384u, 768u, 1152u};
    const auto packet = MakeRxPacket(BaseTickForCycle(0), offsets);

    EXPECT_EQ(cache.Capture(packet, kDbs, 4), 4u);
    EXPECT_EQ(cache.Available(), 4u);
    EXPECT_TRUE(cache.IsEstablished());

    std::array<uint32_t, 4> taken{};
    ASSERT_TRUE(cache.Take(taken));
    for (size_t i = 0; i < offsets.size(); ++i) {
        EXPECT_EQ(taken[i], offsets[i]) << "block " << i;
    }
}

TEST(MotuEventOffsetCacheTests, IsNotEstablishedBeforeAnythingIsCaptured) {
    MotuEventOffsetCache cache{};
    EXPECT_FALSE(cache.IsEstablished());
    EXPECT_EQ(cache.Available(), 0u);

    std::array<uint32_t, 1> taken{};
    EXPECT_FALSE(cache.Take(taken));
}

TEST(MotuEventOffsetCacheTests, CaptureAdvancesItsCycleCountOncePerPacket) {
    // Two packets whose blocks sit at the same absolute ticks: because the base advances
    // one cycle per packet, the second packet's offsets must come out one cycle smaller.
    MotuEventOffsetCache cache{};
    const uint32_t absolute = 5u * kTicksPerCycle;

    const auto first = MakeRxPacket(0u, {absolute});
    EXPECT_EQ(cache.Capture(first, kDbs, 1), 1u);

    const auto second = MakeRxPacket(0u, {absolute});
    EXPECT_EQ(cache.Capture(second, kDbs, 1), 1u);

    std::array<uint32_t, 2> taken{};
    ASSERT_TRUE(cache.Take(taken));
    EXPECT_EQ(taken[0], absolute);
    EXPECT_EQ(taken[1], absolute - kTicksPerCycle);
}

TEST(MotuEventOffsetCacheTests, CaptureHandlesTheOneSecondWrap) {
    MotuEventOffsetCache cache{};
    // A fresh cache bases at cycle 0. MakeRxPacket wraps the absolute tick modulo one
    // second, so an offset near the top of the second exercises the wrap in
    // TickOffsetFromBase without needing to advance the counter 7999 times.
    const auto packet = MakeRxPacket(0u, {kTicksPerSecond - kTicksPerCycle});
    EXPECT_EQ(cache.Capture(packet, kDbs, 1), 1u);

    std::array<uint32_t, 1> taken{};
    ASSERT_TRUE(cache.Take(taken));
    EXPECT_EQ(taken[0], kTicksPerSecond - kTicksPerCycle);
    EXPECT_LT(taken[0], kTicksPerSecond);
}

TEST(MotuEventOffsetCacheTests, TakeIsAllOrNothing) {
    MotuEventOffsetCache cache{};
    const auto packet = MakeRxPacket(BaseTickForCycle(0), {10u, 20u});
    EXPECT_EQ(cache.Capture(packet, kDbs, 2), 2u);

    // Asking for more than is available must consume nothing: a partially stamped
    // packet would put a stale SPH on the wire.
    std::array<uint32_t, 4> tooMany{};
    EXPECT_FALSE(cache.Take(tooMany));
    EXPECT_EQ(cache.Available(), 2u);
    EXPECT_EQ(cache.PlaybackCursor(), 0u);

    std::array<uint32_t, 2> justRight{};
    EXPECT_TRUE(cache.Take(justRight));
    EXPECT_EQ(cache.Available(), 0u);
}

TEST(MotuEventOffsetCacheTests, RefusesARunOlderThanTheRing) {
    MotuEventOffsetCache cache{};
    // Overfill by more than the ring so the earliest history is gone.
    const auto packet = MakeRxPacket(BaseTickForCycle(0), std::vector<uint32_t>(8, 0u));
    for (uint32_t p = 0; p < (MotuEventOffsetCache::kCapacity / 8u) + 4u; ++p) {
        (void)cache.Capture(packet, kDbs, 8);
    }

    // The playback cursor is still at 0, which is now far behind the ring window.
    std::array<uint32_t, 8> taken{};
    EXPECT_FALSE(cache.Take(taken));
    EXPECT_GT(cache.CaptureCursor(), MotuEventOffsetCache::kCapacity);
}

TEST(MotuEventOffsetCacheTests, TruncatedPacketContributesOnlyWhatItHolds) {
    MotuEventOffsetCache cache{};
    // Claim 8 blocks but supply bytes for 3: the missing five must not be invented.
    auto packet = MakeRxPacket(BaseTickForCycle(0), {1u, 2u, 3u});
    EXPECT_EQ(cache.Capture(packet, kDbs, 8), 3u);
    EXPECT_EQ(cache.Available(), 3u);
}

TEST(MotuEventOffsetCacheTests, RejectsZeroDbsOrZeroBlocks) {
    MotuEventOffsetCache cache{};
    const auto packet = MakeRxPacket(BaseTickForCycle(0), {1u});
    EXPECT_EQ(cache.Capture(packet, 0, 1), 0u);
    EXPECT_EQ(cache.Capture(packet, kDbs, 0), 0u);
    EXPECT_FALSE(cache.IsEstablished());
}

TEST(MotuEventOffsetCacheTests, ResetClearsEverything) {
    MotuEventOffsetCache cache{};
    const auto packet = MakeRxPacket(BaseTickForCycle(0), {1u, 2u});
    (void)cache.Capture(packet, kDbs, 2);
    ASSERT_TRUE(cache.IsEstablished());

    cache.Reset();
    EXPECT_FALSE(cache.IsEstablished());
    EXPECT_EQ(cache.Available(), 0u);
    EXPECT_EQ(cache.CaptureCursor(), 0u);
    EXPECT_EQ(cache.PlaybackCursor(), 0u);
    EXPECT_EQ(cache.PlaybackCycleCount(), 0u);
}

TEST(MotuEventOffsetCacheTests, FeedsWritePacketSphEndToEnd) {
    // The whole contract: capture a device packet's per-block timing, then stamp it back
    // onto a transmit packet at a different cycle. Each block must land at its own
    // captured offset from the new base -- not evenly spaced.
    MotuEventOffsetCache cache{};
    const std::vector<uint32_t> deviceOffsets{0u, 400u, 401u, 900u};
    const auto rxPacket = MakeRxPacket(BaseTickForCycle(0), deviceOffsets);
    ASSERT_EQ(cache.Capture(rxPacket, kDbs, 4), 4u);

    std::array<uint32_t, 4> replay{};
    ASSERT_TRUE(cache.Take(replay));

    std::vector<uint8_t> txPacket(kCipHeaderBytes + 4u * kBlockBytes, 0u);
    const uint32_t txCycle = 1234u;
    const auto stamped = WritePacketSph(txPacket, kDbs, 4, txCycle, replay);
    ASSERT_EQ(stamped.blocksStamped, 4u);

    const uint32_t base = BaseTickForCycle(txCycle);
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t sph = ReadSph(std::span<const uint8_t>(
            txPacket.data() + kCipHeaderBytes + i * kBlockBytes, 4));
        EXPECT_EQ(TickFromSph(sph), (base + deviceOffsets[i]) % kTicksPerSecond)
            << "block " << i;
    }
    // Blocks 1 and 2 are one tick apart, as the device timed them.
    EXPECT_EQ(deviceOffsets[2] - deviceOffsets[1], 1u);
}

} // namespace
