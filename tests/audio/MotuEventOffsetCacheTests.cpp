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
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using ASFW::Encoding::Motu::BaseTickForCycle;
using ASFW::Encoding::Motu::kTicksPerCycle;
using ASFW::Encoding::Motu::kTicksPerSecond;
using ASFW::Encoding::Motu::MotuEventOffsetCache;
using ASFW::Encoding::Motu::OffsetCacheTakeResult;
using ASFW::Encoding::Motu::ReadSph;
using ASFW::Encoding::Motu::SphFromTick;
using ASFW::Encoding::Motu::TickFromSph;
using ASFW::Encoding::Motu::WritePacketSph;
using ASFW::Encoding::Motu::WritePacketSphZero;

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

    EXPECT_EQ(cache.Capture(packet, kDbs, 4, 0u), 4u);
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

TEST(MotuEventOffsetCacheTests, CaptureBasesEachPacketOnItsOwnReceiveCycle) {
    // Two packets whose blocks sit at the same absolute ticks, received one cycle apart:
    // the second packet's offset must come out one cycle smaller, because each offset is
    // measured from the cycle its packet arrived in (amdtp-motu.c:309).
    MotuEventOffsetCache cache{};
    const uint32_t absolute = 5u * kTicksPerCycle;

    const auto first = MakeRxPacket(0u, {absolute});
    EXPECT_EQ(cache.Capture(first, kDbs, 1, /*receiveCycle=*/0u), 1u);

    const auto second = MakeRxPacket(0u, {absolute});
    EXPECT_EQ(cache.Capture(second, kDbs, 1, /*receiveCycle=*/1u), 1u);

    std::array<uint32_t, 2> taken{};
    ASSERT_TRUE(cache.Take(taken));
    EXPECT_EQ(taken[0], absolute);
    EXPECT_EQ(taken[1], absolute - kTicksPerCycle);
}

TEST(MotuEventOffsetCacheTests, OffsetIsTheInCyclePresentationOffsetNotTheBusTime) {
    // The regression this pins: the previous port based every capture on a counter that
    // started at 0 while the device stamps real bus time, so a block the device timed a
    // few hundred ticks into cycle 3803 came out as an offset of ~3803 cycles. Replayed
    // onto a transmit cycle, that lands nowhere near the packet and the device drops it.
    MotuEventOffsetCache cache{};
    constexpr uint32_t kReceiveCycle = 3803u;
    const std::vector<uint32_t> inCycle{120u, 520u};
    const auto packet = MakeRxPacket(BaseTickForCycle(kReceiveCycle), inCycle);
    ASSERT_EQ(cache.Capture(packet, kDbs, 2, kReceiveCycle), 2u);

    std::array<uint32_t, 2> taken{};
    ASSERT_TRUE(cache.Take(taken));
    EXPECT_EQ(taken[0], inCycle[0]);
    EXPECT_EQ(taken[1], inCycle[1]);
    EXPECT_LT(taken[1], kTicksPerCycle);
}

TEST(MotuEventOffsetCacheTests, CaptureHandlesTheOneSecondWrap) {
    MotuEventOffsetCache cache{};
    // A fresh cache bases at cycle 0. MakeRxPacket wraps the absolute tick modulo one
    // second, so an offset near the top of the second exercises the wrap in
    // TickOffsetFromBase without needing to advance the counter 7999 times.
    const auto packet = MakeRxPacket(0u, {kTicksPerSecond - kTicksPerCycle});
    EXPECT_EQ(cache.Capture(packet, kDbs, 1, 0u), 1u);

    std::array<uint32_t, 1> taken{};
    ASSERT_TRUE(cache.Take(taken));
    EXPECT_EQ(taken[0], kTicksPerSecond - kTicksPerCycle);
    EXPECT_LT(taken[0], kTicksPerSecond);
}

TEST(MotuEventOffsetCacheTests, TakeIsAllOrNothing) {
    MotuEventOffsetCache cache{};
    const auto packet = MakeRxPacket(BaseTickForCycle(0), {10u, 20u});
    EXPECT_EQ(cache.Capture(packet, kDbs, 2, 0u), 2u);

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

TEST(MotuEventOffsetCacheTests, ResyncsToTheNewestRunWhenHistoryWasOverwritten) {
    MotuEventOffsetCache cache{};
    // Overfill by more than the ring so the earliest history is gone, then make the
    // newest packet distinguishable.
    const auto stale = MakeRxPacket(BaseTickForCycle(0), std::vector<uint32_t>(8, 7u));
    for (uint32_t p = 0; p < (MotuEventOffsetCache::kCapacity / 8u) + 4u; ++p) {
        (void)cache.Capture(stale, kDbs, 8, 0u);
    }
    const auto newest = MakeRxPacket(BaseTickForCycle(0), std::vector<uint32_t>(8, 99u));
    (void)cache.Capture(newest, kDbs, 8, 0u);
    ASSERT_GT(cache.CaptureCursor(), MotuEventOffsetCache::kCapacity);

    // Playback is far behind the ring window. Failing here would fail every later call
    // and leave the stream permanently unstamped; Linux never fails (write_sph reads its
    // head unconditionally). The drain must instead come from the newest complete run.
    std::array<uint32_t, 8> taken{};
    ASSERT_TRUE(cache.Take(taken));
    for (uint32_t offset : taken) {
        EXPECT_EQ(offset, 99u);
    }
    EXPECT_EQ(cache.PlaybackCursor(), cache.CaptureCursor());
}

TEST(MotuEventOffsetCacheTests, TruncatedPacketContributesOnlyWhatItHolds) {
    MotuEventOffsetCache cache{};
    // Claim 8 blocks but supply bytes for 3: the missing five must not be invented.
    auto packet = MakeRxPacket(BaseTickForCycle(0), {1u, 2u, 3u});
    EXPECT_EQ(cache.Capture(packet, kDbs, 8, 0u), 3u);
    EXPECT_EQ(cache.Available(), 3u);
}

TEST(MotuEventOffsetCacheTests, RejectsZeroDbsOrZeroBlocks) {
    MotuEventOffsetCache cache{};
    const auto packet = MakeRxPacket(BaseTickForCycle(0), {1u});
    EXPECT_EQ(cache.Capture(packet, 0, 1, 0u), 0u);
    EXPECT_EQ(cache.Capture(packet, kDbs, 0, 0u), 0u);
    EXPECT_FALSE(cache.IsEstablished());
}

TEST(MotuEventOffsetCacheTests, ResetClearsEverything) {
    MotuEventOffsetCache cache{};
    const auto packet = MakeRxPacket(BaseTickForCycle(0), {1u, 2u});
    (void)cache.Capture(packet, kDbs, 2, 0u);
    ASSERT_TRUE(cache.IsEstablished());

    cache.Reset();
    EXPECT_FALSE(cache.IsEstablished());
    EXPECT_EQ(cache.Available(), 0u);
    EXPECT_EQ(cache.CaptureCursor(), 0u);
    EXPECT_EQ(cache.PlaybackCursor(), 0u);
}

TEST(MotuEventOffsetCacheTests, FeedsWritePacketSphEndToEnd) {
    // The whole contract: capture a device packet's per-block timing, then stamp it back
    // onto a transmit packet at a different cycle. Each block must land at its own
    // captured offset from the new base -- not evenly spaced.
    MotuEventOffsetCache cache{};
    const std::vector<uint32_t> deviceOffsets{0u, 400u, 401u, 900u};
    const auto rxPacket = MakeRxPacket(BaseTickForCycle(0), deviceOffsets);
    ASSERT_EQ(cache.Capture(rxPacket, kDbs, 4, 0u), 4u);

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

TEST(MotuEventOffsetCacheTests, ResyncsToTheNewestRunOnUnderrunWhenHistoryExists) {
    MotuEventOffsetCache cache{};
    const auto packet = MakeRxPacket(BaseTickForCycle(0), {10u, 20u, 30u, 40u});
    EXPECT_EQ(cache.Capture(packet, kDbs, 4, 0u), 4u);

    std::array<uint32_t, 4> first{};
    OffsetCacheTakeResult result{};
    ASSERT_TRUE(cache.Take(first, &result));
    EXPECT_EQ(result, OffsetCacheTakeResult::Success);
    EXPECT_EQ(cache.Available(), 0u);

    // Playback has caught up with capture (underrun).
    // Instead of failing and leaving the packet unstamped, it resyncs to the newest
    // complete run (marked unverified on MOTU hardware).
    std::array<uint32_t, 4> underrunTaken{};
    ASSERT_TRUE(cache.Take(underrunTaken, &result));
    EXPECT_EQ(result, OffsetCacheTakeResult::SuccessUnderrunResync);
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(underrunTaken[i], first[i]);
    }
}

TEST(MotuEventOffsetCacheTests, AbsenceCasesExplicitContract) {
    MotuEventOffsetCache cache{};
    std::array<uint32_t, 4> out{};
    OffsetCacheTakeResult result{};

    // 1. Startup: not established
    EXPECT_FALSE(cache.Take(out, &result));
    EXPECT_EQ(result, OffsetCacheTakeResult::NotEstablished);

    // 2. Insufficient history: capture 2 blocks, but 4 requested
    const auto shortPacket = MakeRxPacket(BaseTickForCycle(0), {100u, 200u});
    EXPECT_EQ(cache.Capture(shortPacket, kDbs, 2, 0u), 2u);
    EXPECT_TRUE(cache.IsEstablished());
    EXPECT_FALSE(cache.Take(out, &result));
    EXPECT_EQ(result, OffsetCacheTakeResult::NotEnoughHistory);

    // 3. Invalid request: empty span or exceeds capacity
    EXPECT_FALSE(cache.Take(std::span<uint32_t>{}, &result));
    EXPECT_EQ(result, OffsetCacheTakeResult::InvalidRequest);

    std::vector<uint32_t> tooLarge(MotuEventOffsetCache::kCapacity + 1, 0u);
    EXPECT_FALSE(cache.Take(tooLarge, &result));
    EXPECT_EQ(result, OffsetCacheTakeResult::InvalidRequest);
}

TEST(MotuEventOffsetCacheTests, WritePacketSphZeroClearsAllBlocks) {
    std::vector<uint8_t> txPacket(kCipHeaderBytes + 4u * kBlockBytes, 0xFFu);
    const uint32_t zeroed = WritePacketSphZero(txPacket, kDbs, 4);
    EXPECT_EQ(zeroed, 4u);

    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t sph = ReadSph(std::span<const uint8_t>(
            txPacket.data() + kCipHeaderBytes + i * kBlockBytes, 4));
        EXPECT_EQ(sph, 0u) << "block " << i;
    }
}

TEST(MotuEventOffsetCacheTests, ConcurrentCaptureAndTakeNoTornReads) {
    MotuEventOffsetCache cache{};
    std::atomic<bool> running{true};
    std::atomic<uint64_t> packetsCaptured{0};
    std::atomic<uint64_t> packetsTaken{0};
    std::atomic<uint64_t> tornReads{0};

    // Pre-seed cache with 1 packet
    const auto seed = MakeRxPacket(BaseTickForCycle(0), {1000u, 1001u, 1002u, 1003u});
    EXPECT_EQ(cache.Capture(seed, kDbs, 4, 0u), 4u);

    // Thread 1: continuous Capture
    std::thread captureThread([&]() {
        uint32_t cycle = 0;
        while (running.load(std::memory_order_relaxed)) {
            const uint32_t baseVal = (cycle * 10) % 2000;
            const auto pkt = MakeRxPacket(BaseTickForCycle(cycle),
                                          {baseVal, baseVal + 1, baseVal + 2, baseVal + 3});
            cache.Capture(pkt, kDbs, 4, cycle);
            cycle = (cycle + 1) % 8000;
            packetsCaptured.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Thread 2: continuous Take
    std::thread takeThread([&]() {
        std::array<uint32_t, 4> run{};
        while (running.load(std::memory_order_relaxed)) {
            OffsetCacheTakeResult res{};
            if (cache.Take(run, &res)) {
                packetsTaken.fetch_add(1, std::memory_order_relaxed);
                // Verify coherent run: each block must be sequential delta +1
                if (run[1] != run[0] + 1 || run[2] != run[1] + 1 || run[3] != run[2] + 1) {
                    tornReads.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    running.store(false, std::memory_order_release);
    captureThread.join();
    takeThread.join();

    EXPECT_GT(packetsCaptured.load(), 0u);
    EXPECT_GT(packetsTaken.load(), 0u);
    EXPECT_EQ(tornReads.load(), 0u);
}

TEST(MotuEventOffsetCacheTests, ConcurrentResetPreventsTornViews) {
    MotuEventOffsetCache cache{};
    std::atomic<bool> running{true};
    std::atomic<uint64_t> resetsPerformed{0};
    std::atomic<uint64_t> clobberedResets{0};

    // Thread 1: continuous Capture & Take
    std::thread workerThread([&]() {
        uint32_t cycle = 0;
        std::array<uint32_t, 4> run{};
        while (running.load(std::memory_order_relaxed)) {
            const auto pkt = MakeRxPacket(BaseTickForCycle(cycle), {10u, 20u, 30u, 40u});
            cache.Capture(pkt, kDbs, 4, cycle);
            (void)cache.Take(run);
            cycle = (cycle + 1) % 8000;
        }
    });

    // Thread 2: random Reset
    std::thread resetThread([&]() {
        while (running.load(std::memory_order_relaxed)) {
            cache.Reset();
            resetsPerformed.fetch_add(1, std::memory_order_relaxed);
            // Verify playback cursor never clobbers 0 to a huge stale value right after reset
            const uint64_t playCursor = cache.PlaybackCursor();
            const uint64_t capCursor = cache.CaptureCursor();
            if (playCursor > capCursor + MotuEventOffsetCache::kCapacity) {
                clobberedResets.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    running.store(false, std::memory_order_release);
    workerThread.join();
    resetThread.join();

    EXPECT_GT(resetsPerformed.load(), 0u);
    EXPECT_EQ(clobberedResets.load(), 0u);
}

} // namespace
