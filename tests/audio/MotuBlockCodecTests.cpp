// MotuBlockCodecTests.cpp
// ASFW - MOTU v2 data-block layout, SPH math, and codec tests
//
// Golden values cross-validated with Linux sound/firewire/motu
// (amdtp-motu.c, motu-protocol-v2.c, motu-stream.c, motu.c).

#include <gtest/gtest.h>
#include "Audio/Wire/MOTU/MotuBlockLayout.hpp"
#include "Audio/Wire/MOTU/MotuSph.hpp"
#include "Audio/Wire/MOTU/MotuBlockCodec.hpp"
#include <array>
#include <vector>

using namespace ASFW::Encoding::Motu;

//==============================================================================
// Layout / geometry (motu-protocol-v2.c, amdtp-motu.c:69-90)
//==============================================================================

TEST(MotuBlockLayoutTests, RateIndexAndModeMapping) {
    EXPECT_EQ(RateToIndex(44100u), 0);
    EXPECT_EQ(RateToIndex(48000u), 1);
    EXPECT_EQ(RateToIndex(96000u), 3);
    EXPECT_EQ(RateToIndex(192000u), 5);
    EXPECT_EQ(RateToIndex(22050u), -1);

    EXPECT_EQ(IndexToMode(0u), 0u);  // 44.1k -> mode 0
    EXPECT_EQ(IndexToMode(1u), 0u);  // 48k   -> mode 0
    EXPECT_EQ(IndexToMode(2u), 1u);  // 88.2k -> mode 1
    EXPECT_EQ(IndexToMode(3u), 1u);  // 96k   -> mode 1
}

TEST(MotuBlockLayoutTests, Motu828mk2ChunkCounts) {
    // Fixed {14, 14, 0} (motu-protocol-v2.c:274-282); optical ADAT adds
    // +8 at mode 0, +4 at mode 1 (motu-protocol-v2.c:253-269).
    EXPECT_EQ(k828mk2FixedPcmChunks[0], 14u);
    EXPECT_EQ(k828mk2FixedPcmChunks[1], 14u);
    EXPECT_EQ(k828mk2FixedPcmChunks[0] + AdatExtraChunks(0), 22u);
    EXPECT_EQ(k828mk2FixedPcmChunks[1] + AdatExtraChunks(1), 18u);
}

TEST(MotuBlockLayoutTests, DataBlockQuadletMath) {
    // quadlets = 1 + ceil((2 + pcm) * 3 / 4)  (amdtp-motu.c:69-74)
    EXPECT_EQ(DataBlockQuadlets(14u), 13u);  // 828mk2, mode 0, optical off
    EXPECT_EQ(DataBlockQuadlets(22u), 19u);  // 828mk2, mode 0, ADAT on
    EXPECT_EQ(DataBlockQuadlets(18u), 16u);  // 828mk2, mode 1, ADAT on
    EXPECT_EQ(DataBlockBytes(14u), 52u);
}

TEST(MotuBlockLayoutTests, MidiPacingInterval) {
    // rate / 3093 (amdtp-motu.c:31,87-88)
    EXPECT_EQ(MidiDataBlockInterval(44100u), 14u);
    EXPECT_EQ(MidiDataBlockInterval(48000u), 15u);
    EXPECT_EQ(MidiDataBlockInterval(96000u), 31u);
}

//==============================================================================
// SPH math (amdtp-motu.c:19-25,303-393)
//==============================================================================

TEST(MotuSphTests, ComposesAndDecomposesSph) {
    // cycle 5, offset 100 -> (5 << 12) | 100
    EXPECT_EQ(SphFromTick(5u * kTicksPerCycle + 100u), (5u << 12) | 100u);
    EXPECT_EQ(TickFromSph((5u << 12) | 100u), 5u * kTicksPerCycle + 100u);
}

TEST(MotuSphTests, RoundTripsAtTimelineEdges) {
    const uint32_t ticks[] = {0u, kTicksPerCycle - 1, kTicksPerCycle,
                              kTicksPerSecond - 1};
    for (uint32_t t : ticks) {
        EXPECT_EQ(TickFromSph(SphFromTick(t)), t) << "tick " << t;
    }
    // One full second wraps to zero.
    EXPECT_EQ(SphFromTick(kTicksPerSecond), 0u);
}

TEST(MotuSphTests, OffsetFromBaseHandlesSecondWrap) {
    // Presentation just after the wrap, base just before it
    // (amdtp-motu.c:319-320: tick < base -> += one second).
    const uint32_t baseTick = kTicksPerSecond - 50u;
    const uint32_t sph = SphFromTick(25u);
    EXPECT_EQ(TickOffsetFromBase(sph, baseTick), 75u);
}

TEST(MotuSphTests, OffsetFromBaseWithoutWrap) {
    const uint32_t baseTick = 1000u;
    const uint32_t sph = SphFromTick(1500u);
    EXPECT_EQ(TickOffsetFromBase(sph, baseTick), 500u);
}

TEST(MotuSphTests, ReplayRebasesCachedOffsets) {
    // Offsets cached on the receive side are preserved relative to whatever
    // base the transmit side rebases onto (amdtp-motu.c:373-393).
    const uint32_t offsets[] = {0u, 7u, 557u, 1114u};
    const uint32_t txBase = 400u * kTicksPerCycle;

    for (uint32_t cached : offsets) {
        const uint32_t sph = ReplaySph(cached, txBase);
        EXPECT_EQ(TickFromSph(sph), (txBase + cached) % kTicksPerSecond);
    }
}

TEST(MotuSphTests, ReplayWrapsOneSecondTimeline) {
    const uint32_t txBase = kTicksPerSecond - 10u;
    EXPECT_EQ(TickFromSph(ReplaySph(30u, txBase)), 20u);
}

//==============================================================================
// PCM codec (amdtp-motu.c:93-187)
//==============================================================================

TEST(MotuBlockCodecTests, PacksSampleBigEndian3Bytes) {
    std::array<uint8_t, 3> chunk{};
    WritePcmSample(chunk, 0x12345600);  // 24-bit sample 0x123456, MSB-aligned
    EXPECT_EQ(chunk[0], 0x12u);
    EXPECT_EQ(chunk[1], 0x34u);
    EXPECT_EQ(chunk[2], 0x56u);
}

TEST(MotuBlockCodecTests, SampleRoundTrips) {
    const int32_t samples[] = {0, 0x7FFFFF00, static_cast<int32_t>(0x80000000),
                               static_cast<int32_t>(0xFFFFFF00), 0x00000100};
    std::array<uint8_t, 3> chunk{};
    for (int32_t s : samples) {
        WritePcmSample(chunk, s);
        EXPECT_EQ(ReadPcmSample(chunk), s);
    }
}

TEST(MotuBlockCodecTests, WritesFrameAtPcmByteOffset) {
    std::vector<uint8_t> block(DataBlockBytes(14u), 0xEE);
    const std::array<int32_t, 14> frame{0x01020300, 0x0A0B0C00};

    WritePcmFrame(block, frame);

    // Channel 0 lands at byte offset 10 (motu-protocol-v2.c:234).
    EXPECT_EQ(block[10], 0x01u);
    EXPECT_EQ(block[11], 0x02u);
    EXPECT_EQ(block[12], 0x03u);
    // Channel 1 packs immediately after, 3-byte stride.
    EXPECT_EQ(block[13], 0x0Au);
    EXPECT_EQ(ReadPcmChannel(block, 1u), 0x0A0B0C00);
    // SPH and message bytes untouched.
    EXPECT_EQ(block[0], 0xEEu);
    EXPECT_EQ(block[9], 0xEEu);
}

TEST(MotuBlockCodecTests, SilenceZeroesOnlyPcmChunks) {
    std::vector<uint8_t> block(DataBlockBytes(14u), 0xEE);
    WritePcmSilence(block, 14u);

    for (uint32_t c = 0; c < 14u; ++c) {
        EXPECT_EQ(ReadPcmChannel(block, c), 0) << "channel " << c;
    }
    EXPECT_EQ(block[kMidiFlagByteOffset2ndQ], 0xEEu);  // msg bytes untouched
}

//==============================================================================
// SPH quadlet placement
//==============================================================================

TEST(MotuBlockCodecTests, SphOccupiesLeadingQuadletBigEndian) {
    std::vector<uint8_t> block(DataBlockBytes(14u), 0x00);
    const uint32_t sph = (1234u << 12) | 567u;
    WriteSph(block, sph);

    EXPECT_EQ(block[0], static_cast<uint8_t>(sph >> 24));
    EXPECT_EQ(block[3], static_cast<uint8_t>(sph & 0xFF));
    EXPECT_EQ(ReadSph(block), sph);
}

//==============================================================================
// MIDI slot, second-quadlet flavor (motu-stream.c:117-131, amdtp-motu.c:211-254)
//==============================================================================

TEST(MotuBlockCodecTests, WritesMidiByteWithFlag) {
    std::vector<uint8_t> block(DataBlockBytes(14u), 0x00);
    WriteMidi2ndQ(block, 0x90u);

    EXPECT_EQ(block[4], 0x01u);  // flag byte offset 4
    EXPECT_EQ(block[6], 0x90u);  // MIDI byte offset 6

    const auto read = ReadMidi2ndQ(block);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, 0x90u);
}

TEST(MotuBlockCodecTests, EmptyMidiSlotClearsFlagAndByte) {
    std::vector<uint8_t> block(DataBlockBytes(14u), 0xFF);
    WriteMidi2ndQ(block, std::nullopt);

    EXPECT_EQ(block[4], 0x00u);
    EXPECT_EQ(block[6], 0x00u);
    EXPECT_FALSE(ReadMidi2ndQ(block).has_value());
}
