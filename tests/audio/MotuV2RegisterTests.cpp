// MotuV2RegisterTests.cpp
// ASFW - MOTU protocol-v2 register-plane codec tests
//
// Golden values cross-validated with Linux sound/firewire/motu
// (motu-protocol-v2.c, motu-stream.c, motu-transaction.c, motu.c).

#include <gtest/gtest.h>
#include "Audio/Protocols/MOTU/MotuV2Registers.hpp"

using namespace ASFW::Audio::Motu;

//==============================================================================
// Clock status register (motu-protocol-v2.c:10-23, motu.c:16-26)
//==============================================================================

TEST(MotuV2ClockTests, DecodesRateIndex) {
    EXPECT_EQ(DecodeRateV2(0x00000000u).value(), 44100u);  // index 0
    EXPECT_EQ(DecodeRateV2(0x00000008u).value(), 48000u);  // index 1
    EXPECT_EQ(DecodeRateV2(0x00000018u).value(), 96000u);  // index 3
    EXPECT_EQ(DecodeRateV2(0x00000028u).value(), 192000u); // index 5
}

TEST(MotuV2ClockTests, DecodeIgnoresUnrelatedBits) {
    EXPECT_EQ(DecodeRateV2(0xFFFFFF0Fu).value(), 48000u);  // index 1 amid noise
}

TEST(MotuV2ClockTests, EncodeRatePreservesOtherBits) {
    // Current: internal clock (source 0), 48k (index 1), some high bits set.
    const auto encoded = EncodeRateV2(0x02000009u, 96000u);
    ASSERT_TRUE(encoded.has_value());
    // Rate bits become index 3 (0x18); source bit 0x01 and 0x02000000 kept.
    EXPECT_EQ(*encoded, 0x02000019u);
}

TEST(MotuV2ClockTests, EncodeRejectsUnsupportedRate) {
    EXPECT_FALSE(EncodeRateV2(0u, 32000u).has_value());
}

TEST(MotuV2ClockTests, DecodesClockSources) {
    EXPECT_EQ(DecodeClockSourceV2(0x00000000u).value(), ClockSourceV2::Internal);
    EXPECT_EQ(DecodeClockSourceV2(0x00000001u).value(), ClockSourceV2::AdatOnOpt);
    EXPECT_EQ(DecodeClockSourceV2(0x00000003u).value(), ClockSourceV2::Sph);
    EXPECT_EQ(DecodeClockSourceV2(0x00000005u).value(), ClockSourceV2::AdatOnDsub);
    EXPECT_EQ(DecodeClockSourceV2(0x00000007u).value(), ClockSourceV2::AesEbuOnXlr);
    EXPECT_FALSE(DecodeClockSourceV2(0x00000006u).has_value());
}

//==============================================================================
// Optical interface config (motu-protocol-v2.c:25-32)
//==============================================================================

TEST(MotuV2OpticalTests, DecodesInAndOutModes) {
    // in = ADAT (bits [9:8] = 1), out = SPDIF (bits [11:10] = 2).
    const auto cfg = DecodeOptIfaceConfig(0x00000900u);
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->input, OptIfaceMode::Adat);
    EXPECT_EQ(cfg->output, OptIfaceMode::Spdif);
}

TEST(MotuV2OpticalTests, DecodesNoneModes) {
    const auto cfg = DecodeOptIfaceConfig(0x00000000u);
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->input, OptIfaceMode::None);
    EXPECT_EQ(cfg->output, OptIfaceMode::None);
}

TEST(MotuV2OpticalTests, RejectsReservedModeValues) {
    EXPECT_FALSE(DecodeOptIfaceConfig(0x00000300u).has_value());  // in = 3
}

//==============================================================================
// Iso comm control (motu-stream.c:12-21,62-107)
//==============================================================================

TEST(MotuV2IsoCommTests, StartActivatesBothDirectionsWithChannels) {
    // rx channel 5, tx channel 9; low 16 bits of current value preserved.
    const uint32_t encoded = EncodeIsoCommStart(0x00001234u, 5u, 9u);

    EXPECT_EQ(encoded & 0x0000ffffu, 0x1234u);
    EXPECT_TRUE(encoded & kChangeRxState);
    EXPECT_TRUE(encoded & kRxActivated);
    EXPECT_TRUE(encoded & kChangeTxState);
    EXPECT_TRUE(encoded & kTxActivated);

    const auto state = DecodeIsoCommState(encoded);
    EXPECT_TRUE(state.rxActivated);
    EXPECT_EQ(state.rxChannel, 5u);
    EXPECT_TRUE(state.txActivated);
    EXPECT_EQ(state.txChannel, 9u);
}

TEST(MotuV2IsoCommTests, StopClearsActivationKeepsChannels) {
    const uint32_t running = EncodeIsoCommStart(0u, 5u, 9u);
    const uint32_t stopped = EncodeIsoCommStop(running);

    EXPECT_TRUE(stopped & kChangeRxState);
    EXPECT_TRUE(stopped & kChangeTxState);

    const auto state = DecodeIsoCommState(stopped);
    EXPECT_FALSE(state.rxActivated);
    EXPECT_FALSE(state.txActivated);
    EXPECT_EQ(state.rxChannel, 5u);
    EXPECT_EQ(state.txChannel, 9u);
}

TEST(MotuV2IsoCommTests, ChannelFieldsMaskTo6Bits) {
    const auto state = DecodeIsoCommState(EncodeIsoCommStart(0u, 0x7fu, 0x41u));
    EXPECT_EQ(state.rxChannel, 0x3fu);
    EXPECT_EQ(state.txChannel, 0x01u);
}

//==============================================================================
// Packet format (motu-stream.c:23-26,201-225)
//==============================================================================

TEST(MotuV2PacketFormatTests, SetsExcludeFlagsAndSpeed) {
    // Optical off in both directions at S400 (speed code 2).
    const uint32_t encoded = EncodePacketFormat(0xFFFFFF00u, true, true, 2u);
    EXPECT_TRUE(encoded & kTxExcludeDifferedChunks);
    EXPECT_TRUE(encoded & kRxExcludeDifferedChunks);
    EXPECT_EQ(encoded & kTxSpeedMask, 2u);
    // Unrelated high bits preserved.
    EXPECT_EQ(encoded & 0xFFFFFF00u & ~(kTxExcludeDifferedChunks | kRxExcludeDifferedChunks),
              0xFFFFFF00u & ~(kTxExcludeDifferedChunks | kRxExcludeDifferedChunks));
}

TEST(MotuV2PacketFormatTests, ClearsFlagsWhenAdatActive) {
    const uint32_t prior = EncodePacketFormat(0u, true, true, 2u);
    const uint32_t encoded = EncodePacketFormat(prior, false, false, 2u);
    EXPECT_FALSE(encoded & kTxExcludeDifferedChunks);
    EXPECT_FALSE(encoded & kRxExcludeDifferedChunks);
}

//==============================================================================
// Async message registration (motu-transaction.c:74-121)
//==============================================================================

TEST(MotuV2AsyncTests, EncodesNodeAndAddressSplit) {
    const auto values = EncodeAsyncAddr(0xffc0u, 0xffffe0000020ull);
    EXPECT_EQ(values.hi, (0xffc0u << 16) | 0x0000ffffu);
    EXPECT_EQ(values.lo, 0xe0000020u);
}

TEST(MotuV2AsyncTests, MessageRegionMatchesReference) {
    EXPECT_EQ(kAsyncMessageRegionStart, 0xffffe0000000ull);
    EXPECT_EQ(kAsyncMessageRegionEnd, 0xffffe000ffffull);
}
