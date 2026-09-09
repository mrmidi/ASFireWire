// SPDX-License-Identifier: Apache-2.0
//
// MotuTxTiming tests.
//
// Pins the transmit-side SPH replay against Linux write_sph() (amdtp-motu.c:373-393):
// one cached presentation offset per data block, rebased onto the packet's cycle, with
// wrap at the one-second boundary.

#include "Audio/Wire/MOTU/MotuTxTiming.hpp"
#include "Audio/Wire/MOTU/MotuRxTiming.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {

using ASFW::Encoding::Motu::AdvanceCycleCount;
using ASFW::Encoding::Motu::BaseTickForCycle;
using ASFW::Encoding::Motu::kCyclesPerSecond;
using ASFW::Encoding::Motu::kTicksPerCycle;
using ASFW::Encoding::Motu::kTicksPerSecond;
using ASFW::Encoding::Motu::ReadSph;
using ASFW::Encoding::Motu::TickFromSph;
using ASFW::Encoding::Motu::WritePacketSph;

constexpr uint32_t kCipHeaderBytes = 8;
constexpr uint32_t kDbs = 4;
constexpr uint32_t kBlockBytes = kDbs * 4;

[[nodiscard]] std::vector<uint8_t> MakePacket(uint32_t blocks) {
    return std::vector<uint8_t>(kCipHeaderBytes + blocks * kBlockBytes, 0u);
}

[[nodiscard]] uint32_t SphOfBlock(const std::vector<uint8_t>& packet, uint32_t block) {
    return ReadSph(std::span<const uint8_t>(
        packet.data() + kCipHeaderBytes + block * kBlockBytes, 4));
}

//==============================================================================

TEST(MotuTxTimingTests, StampsOnePerBlockUsingItsOwnCachedOffset) {
    // The whole point of MOTU's replay: blocks are NOT evenly spaced, each carries the
    // offset the device itself used.
    auto packet = MakePacket(4);
    const std::array<uint32_t, 4> offsets{0u, 100u, 350u, 351u};

    const auto result = WritePacketSph(packet, kDbs, 4, /*cycleCount=*/10, offsets);

    ASSERT_EQ(result.blocksStamped, 4u);
    const uint32_t base = BaseTickForCycle(10);
    for (uint32_t i = 0; i < 4; ++i) {
        EXPECT_EQ(TickFromSph(SphOfBlock(packet, i)), (base + offsets[i]) % kTicksPerSecond)
            << "block " << i;
    }
}

TEST(MotuTxTimingTests, WrapsAtTheOneSecondBoundary) {
    // Last cycle of the second with an offset that pushes past the wrap.
    const uint32_t lastCycle = kCyclesPerSecond - 1u;
    auto packet = MakePacket(1);
    const std::array<uint32_t, 1> offsets{2u * kTicksPerCycle};

    const auto result = WritePacketSph(packet, kDbs, 1, lastCycle, offsets);

    ASSERT_EQ(result.blocksStamped, 1u);
    const uint32_t expected =
        (BaseTickForCycle(lastCycle) + offsets[0]) % kTicksPerSecond;
    EXPECT_EQ(TickFromSph(SphOfBlock(packet, 0)), expected);
    // Wrapped, so it lands early in the second rather than beyond its end.
    EXPECT_LT(expected, kTicksPerSecond);
    EXPECT_EQ(expected, kTicksPerCycle);
}

TEST(MotuTxTimingTests, AdvancesTheCycleCountByExactlyOnePacket) {
    auto packet = MakePacket(1);
    const std::array<uint32_t, 1> offsets{0u};

    const auto result = WritePacketSph(packet, kDbs, 1, /*cycleCount=*/7999, offsets);
    // 7999 is the last cycle; the next packet starts the second again.
    EXPECT_EQ(result.nextCycleCount, 0u);
    EXPECT_EQ(AdvanceCycleCount(7999u), 0u);
    EXPECT_EQ(AdvanceCycleCount(0u), 1u);
}

TEST(MotuTxTimingTests, StopsWhenTheOffsetSequenceRunsShort) {
    // An under-supplied replay sequence must not leave later blocks stamped with a
    // stale or invented SPH -- the caller has to see the shortfall.
    auto packet = MakePacket(4);
    const std::array<uint32_t, 2> offsets{10u, 20u};

    const auto result = WritePacketSph(packet, kDbs, 4, /*cycleCount=*/0, offsets);

    EXPECT_EQ(result.blocksStamped, 2u);
    // Blocks 2 and 3 were left as-is (zero-filled here), not invented.
    EXPECT_EQ(SphOfBlock(packet, 2), 0u);
    EXPECT_EQ(SphOfBlock(packet, 3), 0u);
}

TEST(MotuTxTimingTests, StopsAtTheEndOfAShortPayload) {
    auto packet = MakePacket(2); // room for 2 blocks
    const std::array<uint32_t, 4> offsets{1u, 2u, 3u, 4u};

    const auto result = WritePacketSph(packet, kDbs, 4, /*cycleCount=*/0, offsets);

    EXPECT_EQ(result.blocksStamped, 2u);
}

TEST(MotuTxTimingTests, RejectsZeroDbsOrZeroBlocks) {
    auto packet = MakePacket(2);
    const std::array<uint32_t, 2> offsets{1u, 2u};

    EXPECT_EQ(WritePacketSph(packet, 0, 2, 0, offsets).blocksStamped, 0u);
    EXPECT_EQ(WritePacketSph(packet, kDbs, 0, 0, offsets).blocksStamped, 0u);
}

TEST(MotuTxTimingTests, RoundTripsAnOffsetCapturedFromReceive) {
    // Capture an offset from a received packet, then replay it on transmit at a
    // different cycle: the presentation must sit the same distance ahead of the new
    // base as it did the old one. This is the whole replay contract in one assertion.
    using ASFW::Encoding::Motu::DecodeRxTiming;
    using ASFW::Encoding::Motu::SphFromTick;

    const uint32_t rxCycle = 300u;
    const uint32_t rxBase = rxCycle * kTicksPerCycle;
    const uint32_t deviceOffset = 700u;

    std::vector<uint8_t> rxPacket(kCipHeaderBytes + kBlockBytes, 0u);
    const uint32_t sph = SphFromTick(rxBase + deviceOffset);
    uint8_t* b = rxPacket.data() + kCipHeaderBytes;
    b[0] = static_cast<uint8_t>(sph >> 24);
    b[1] = static_cast<uint8_t>(sph >> 16);
    b[2] = static_cast<uint8_t>(sph >> 8);
    b[3] = static_cast<uint8_t>(sph);

    const auto captured = DecodeRxTiming(rxPacket, kDbs, (rxCycle << 12));
    ASSERT_TRUE(captured.valid);
    EXPECT_EQ(captured.presentationOffsetTicks, deviceOffset);

    auto txPacket = MakePacket(1);
    const std::array<uint32_t, 1> offsets{captured.presentationOffsetTicks};
    const uint32_t txCycle = 4321u;
    ASSERT_EQ(WritePacketSph(txPacket, kDbs, 1, txCycle, offsets).blocksStamped, 1u);

    EXPECT_EQ(TickFromSph(SphOfBlock(txPacket, 0)),
              (BaseTickForCycle(txCycle) + deviceOffset) % kTicksPerSecond);
}

} // namespace
