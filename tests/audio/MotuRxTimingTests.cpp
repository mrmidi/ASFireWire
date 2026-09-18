// SPDX-License-Identifier: Apache-2.0
//
// MotuRxTiming tests.
//
// MOTU carries presentation time in a per-data-block SPH quadlet rather than the CIP
// SYT field, but feeds the same RxSequenceEntry::sytOffset the TX replay path consumes.
// These pin that conversion, including the one-second wrap, against Linux
// amdtp-motu.c:19-25,303-393.

#include "Audio/Wire/MOTU/MotuRxTiming.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {

using ASFW::Encoding::Motu::BaseTickFromCycleTimer;
using ASFW::Encoding::Motu::DecodeRxTiming;
using ASFW::Encoding::Motu::kCipHeaderBytes;
using ASFW::Encoding::Motu::kTicksPerCycle;
using ASFW::Encoding::Motu::kTicksPerSecond;
using ASFW::Encoding::Motu::SphFromTick;

constexpr uint32_t kDbs = 4;               // 16-byte data block
constexpr uint32_t kBlockBytes = kDbs * 4;

/// One packet: CIP header plus `blocks` data blocks, with `sph` written at the head of
/// the first block.
[[nodiscard]] std::vector<uint8_t> MakePacket(uint32_t sph, uint32_t blocks = 1) {
    std::vector<uint8_t> bytes(kCipHeaderBytes + blocks * kBlockBytes, 0u);
    uint8_t* block = bytes.data() + kCipHeaderBytes;
    block[0] = static_cast<uint8_t>(sph >> 24);
    block[1] = static_cast<uint8_t>(sph >> 16);
    block[2] = static_cast<uint8_t>(sph >> 8);
    block[3] = static_cast<uint8_t>(sph);
    return bytes;
}

/// Cycle timer word: seconds [24:19], cycle [18:12], offset [11:0] per TimingUtils.
[[nodiscard]] constexpr uint32_t MakeCycleTimer(uint32_t seconds, uint32_t cycle,
                                                uint32_t offset) {
    return ((seconds & 0x7Fu) << 25) | ((cycle & 0x1FFFu) << 12) | (offset & 0x0FFFu);
}

//==============================================================================

TEST(MotuRxTimingTests, BaseTickDropsTheSecondsFieldBecauseSphWrapsEachSecond) {
    // Same cycle/offset in two different seconds must map to the same SPH-timeline tick:
    // the SPH domain is one second wide.
    const uint32_t a = BaseTickFromCycleTimer(MakeCycleTimer(0, 100, 500));
    const uint32_t b = BaseTickFromCycleTimer(MakeCycleTimer(5, 100, 500));
    EXPECT_EQ(a, b);
    EXPECT_EQ(a, 100u * kTicksPerCycle + 500u);
}

TEST(MotuRxTimingTests, DecodesAnOffsetAheadOfThePacketArrival) {
    // Packet arrives at cycle 100; the device asks for presentation 2 cycles later.
    const uint32_t baseTick = 100u * kTicksPerCycle;
    const uint32_t presentationTick = baseTick + 2u * kTicksPerCycle;

    const auto packet = MakePacket(SphFromTick(presentationTick));
    const auto timing = DecodeRxTiming(packet, kDbs, MakeCycleTimer(0, 100, 0));

    ASSERT_TRUE(timing.valid);
    EXPECT_EQ(timing.presentationOffsetTicks, 2u * kTicksPerCycle);
}

TEST(MotuRxTimingTests, HandlesTheOneSecondWrap) {
    // Packet arrives near the end of the second; presentation lands after the wrap.
    // A naive subtraction would underflow to a huge offset.
    const uint32_t baseTick = kTicksPerSecond - kTicksPerCycle; // last cycle
    const uint32_t presentationTick = kTicksPerCycle;           // cycle 1 of next second

    const uint32_t arrivalCycle = (kTicksPerSecond / kTicksPerCycle) - 1u;
    const auto packet = MakePacket(SphFromTick(presentationTick));
    const auto timing = DecodeRxTiming(packet, kDbs, MakeCycleTimer(0, arrivalCycle, 0));

    ASSERT_TRUE(timing.valid);
    // Two cycles forward across the boundary, not (almost) a whole second backwards.
    EXPECT_EQ(timing.presentationOffsetTicks, 2u * kTicksPerCycle);
    EXPECT_LT(timing.presentationOffsetTicks, kTicksPerSecond);
    (void)baseTick;
}

TEST(MotuRxTimingTests, ZeroOffsetWhenPresentationMatchesArrival) {
    const uint32_t tick = 4000u * kTicksPerCycle + 17u;
    const auto packet = MakePacket(SphFromTick(tick));
    const auto timing = DecodeRxTiming(packet, kDbs, MakeCycleTimer(3, 4000, 17));

    ASSERT_TRUE(timing.valid);
    EXPECT_EQ(timing.presentationOffsetTicks, 0u);
}

TEST(MotuRxTimingTests, PreservesTheRawSphForDiagnostics) {
    const uint32_t sph = SphFromTick(1234u * kTicksPerCycle + 56u);
    const auto packet = MakePacket(sph);
    const auto timing = DecodeRxTiming(packet, kDbs, MakeCycleTimer(0, 0, 0));

    ASSERT_TRUE(timing.valid);
    EXPECT_EQ(timing.firstBlockSph, sph);
}

TEST(MotuRxTimingTests, RejectsAPayloadTooShortForOneDataBlock) {
    // CIP header present but the block is truncated: nothing may be decoded, and the
    // caller must not get a plausible-looking offset from whatever follows.
    std::vector<uint8_t> shortPacket(kCipHeaderBytes + kBlockBytes - 1u, 0u);
    const auto timing = DecodeRxTiming(shortPacket, kDbs, MakeCycleTimer(0, 0, 0));

    EXPECT_FALSE(timing.valid);
    EXPECT_EQ(timing.presentationOffsetTicks, UINT32_MAX);
}

TEST(MotuRxTimingTests, RejectsZeroDbs) {
    const auto packet = MakePacket(SphFromTick(0));
    const auto timing = DecodeRxTiming(packet, 0, MakeCycleTimer(0, 0, 0));
    EXPECT_FALSE(timing.valid);
}

TEST(MotuRxTimingTests, ReadsOnlyTheFirstBlockOfAMultiBlockPacket) {
    // Later blocks are evenly spaced by construction; the replay cache stores one
    // offset per packet, so only the first block's SPH is consulted.
    const uint32_t firstTick = 200u * kTicksPerCycle;
    auto packet = MakePacket(SphFromTick(firstTick), /*blocks=*/8);

    // Poison the second block's SPH: it must not influence the result.
    uint8_t* second = packet.data() + kCipHeaderBytes + kBlockBytes;
    second[0] = 0xFFu;
    second[1] = 0xFFu;
    second[2] = 0xFFu;
    second[3] = 0xFFu;

    const auto timing = DecodeRxTiming(packet, kDbs, MakeCycleTimer(0, 200, 0));
    ASSERT_TRUE(timing.valid);
    EXPECT_EQ(timing.presentationOffsetTicks, 0u);
}

} // namespace
