// SPDX-License-Identifier: Apache-2.0
//
// MotuTxTiming tests: the SPH arithmetic around the transmit stamper.

#include "Audio/Wire/MOTU/MotuTxTiming.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using ASFW::Encoding::Motu::BaseTickForCycle;
using ASFW::Encoding::Motu::kCyclesPerSecond;
using ASFW::Encoding::Motu::kTicksPerCycle;
using ASFW::Encoding::Motu::kTicksPerSecond;
using ASFW::Encoding::Motu::ReadSph;
using ASFW::Encoding::Motu::WritePacketSphZero;

constexpr uint32_t kCipHeaderBytes = 8;
constexpr uint32_t kDbs = 4;
constexpr uint32_t kBlockBytes = kDbs * 4;

[[nodiscard]] uint32_t SphOfBlock(const std::vector<uint8_t>& packet, uint32_t block) {
    return ReadSph(std::span<const uint8_t>(
        packet.data() + kCipHeaderBytes + block * kBlockBytes, 4));
}

//==============================================================================

TEST(MotuTxTimingTests, BaseTickIsTheCycleOnTheOneSecondTimeline) {
    EXPECT_EQ(BaseTickForCycle(0), 0u);
    EXPECT_EQ(BaseTickForCycle(10), 10u * kTicksPerCycle);
    EXPECT_EQ(BaseTickForCycle(kCyclesPerSecond - 1), kTicksPerSecond - kTicksPerCycle);
    EXPECT_EQ(BaseTickForCycle(kCyclesPerSecond + 3), 3u * kTicksPerCycle);
}

TEST(MotuTxTimingTests, WritePacketSphZeroClearsEveryBlockThatFits) {
    std::vector<uint8_t> packet(kCipHeaderBytes + 4u * kBlockBytes, 0xFFu);
    EXPECT_EQ(WritePacketSphZero(packet, kDbs, 4), 4u);
    for (uint32_t i = 0; i < 4; ++i) {
        EXPECT_EQ(SphOfBlock(packet, i), 0u) << "block " << i;
        // Only the SPH quadlet: the rest of the block is not the stamper's.
        EXPECT_EQ(packet[kCipHeaderBytes + i * kBlockBytes + 4], 0xFFu) << "block " << i;
    }

    std::vector<uint8_t> shortPacket(kCipHeaderBytes + 2u * kBlockBytes, 0xFFu);
    EXPECT_EQ(WritePacketSphZero(shortPacket, kDbs, 4), 2u);
    EXPECT_EQ(WritePacketSphZero(shortPacket, 0, 4), 0u);
    EXPECT_EQ(WritePacketSphZero(shortPacket, kDbs, 0), 0u);
}

// The SPH carries no seconds field, so tick arithmetic happens on a one-second
// circle: wrapping is modular for negative counts too, and a difference across
// the rollover is the short distance, not almost a second.
TEST(MotuTxTimingTests, SignedTickArithmeticWrapsOnTheOneSecondTimeline) {
    using ASFW::Encoding::Motu::TickDifference;
    using ASFW::Encoding::Motu::WrapTick;
    constexpr int64_t kSecond = kTicksPerSecond;

    EXPECT_EQ(WrapTick(0), 0u);
    EXPECT_EQ(WrapTick(kSecond), 0u);
    EXPECT_EQ(WrapTick(3 * kSecond + 7), 7u);
    EXPECT_EQ(WrapTick(-1), kTicksPerSecond - 1);
    EXPECT_EQ(WrapTick(-3 * kSecond - 7), kTicksPerSecond - 7);

    // One 48 kHz packet of eight frames straddling the rollover.
    EXPECT_EQ(TickDifference(4096 - 1000, kSecond - 1000), 4096);
    EXPECT_EQ(TickDifference(kSecond - 1000, 4096 - 1000), -4096);
    // Exactly half a second is the positive end of the range.
    EXPECT_EQ(TickDifference(kSecond / 2, 0), kSecond / 2);
    EXPECT_EQ(TickDifference(kSecond / 2 + 1, 0), -(kSecond / 2 - 1));
    // Whole seconds in either operand do not change the distance.
    EXPECT_EQ(TickDifference(5 * kSecond + 100, -2 * kSecond + 40), 60);
}

} // namespace
