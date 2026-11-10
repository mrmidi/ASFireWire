// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Digi00xRegistersTests.cpp - Digi 002/003 register codes, geometry and
// handshake values. Cross-validated with Linux sound/firewire/digi00x
// (digi00x.h, digi00x-stream.c, digi00x-transaction.c).

#include <gtest/gtest.h>

#include "Audio/Protocols/Digi00x/Digi00xRegisters.hpp"

#include <vector>

using namespace ASFW::Audio::Digi00x;

namespace {

[[nodiscard]] std::vector<uint32_t> Steps(uint32_t state) {
    const auto steps = StartStepsFor(state);
    if (!steps) {
        return {0xdeadbeefU};
    }
    const auto values = steps->Values();
    return {values.begin(), values.end()};
}

TEST(Digi00xRegisters, AddressesAreOffsetsFromTheRegisterBase) {
    EXPECT_EQ(Register::kStreamingState, 0xffffe0000000ULL);
    EXPECT_EQ(Register::kStreamingSet, 0xffffe0000004ULL);
    EXPECT_EQ(Register::kMessageAddress, 0xffffe0000014ULL);
    EXPECT_EQ(Register::kIsochChannels, 0xffffe0000100ULL);
    EXPECT_EQ(Register::kLocalRate, 0xffffe0000110ULL);
    EXPECT_EQ(Register::kExternalRate, 0xffffe0000114ULL);
    EXPECT_EQ(Register::kClockSource, 0xffffe0000118ULL);
    EXPECT_EQ(Register::kOpticalMode, 0xffffe000011cULL);
    EXPECT_EQ(Register::kDetectExternal, 0xffffe000012cULL);
}

TEST(Digi00xRegisters, RateCodesRoundTrip) {
    for (const uint32_t hz : kSupportedRatesHz) {
        const auto code = RateCodeForHz(hz);
        ASSERT_TRUE(code.has_value()) << hz;
        EXPECT_EQ(DecodeRateHz(static_cast<uint32_t>(*code)), hz);
    }
    EXPECT_EQ(RateCodeForHz(48000), RateCode::k48000);
    EXPECT_FALSE(RateCodeForHz(192000).has_value());
    EXPECT_FALSE(RateCodeForHz(32000).has_value());
}

TEST(Digi00xRegisters, RateDecodeUsesTheLowNibbleAndRejectsDesync) {
    EXPECT_EQ(DecodeRateHz(0xfffffff1U), 48000U);
    // An external rate code outside 0..3 means no lock to the external source.
    EXPECT_FALSE(DecodeRateHz(0x4).has_value());
    EXPECT_FALSE(DecodeRateHz(0xf).has_value());
}

TEST(Digi00xRegisters, ClockSourceDecode) {
    EXPECT_EQ(DecodeClockSource(0), ClockSource::kInternal);
    EXPECT_EQ(DecodeClockSource(1), ClockSource::kSpdif);
    EXPECT_EQ(DecodeClockSource(2), ClockSource::kAdat);
    EXPECT_EQ(DecodeClockSource(0x13), ClockSource::kWordClock);
    EXPECT_FALSE(DecodeClockSource(4).has_value());
    EXPECT_TRUE(DecodeExternalDetected(1));
    EXPECT_FALSE(DecodeExternalDetected(0));
}

TEST(Digi00xRegisters, GeometryIsOneMidiChannelPlusPcm) {
    EXPECT_EQ(PcmChannelsForRate(44100), 18U);
    EXPECT_EQ(PcmChannelsForRate(48000), 18U);
    EXPECT_EQ(PcmChannelsForRate(88200), 10U);
    EXPECT_EQ(PcmChannelsForRate(96000), 10U);
    EXPECT_FALSE(PcmChannelsForRate(176400).has_value());
    EXPECT_EQ(DataBlockQuadletsForRate(48000), 19U);
    EXPECT_EQ(DataBlockQuadletsForRate(96000), 11U);
}

TEST(Digi00xRegisters, ChannelWordPutsCaptureHighAndPlaybackLow) {
    EXPECT_EQ(ChannelWord(/*capture=*/1, /*playback=*/0), 0x00010000U);
    EXPECT_EQ(ChannelWord(/*capture=*/0x3f, /*playback=*/0x2a), 0x003f002aU);
    EXPECT_EQ(kReleaseChannels, 0U);
}

TEST(Digi00xRegisters, StartStepsCountDownToOne) {
    EXPECT_EQ(Steps(0), (std::vector<uint32_t>{1}));  // 0 counts as 2
    EXPECT_EQ(Steps(1), (std::vector<uint32_t>{}));
    EXPECT_EQ(Steps(2), (std::vector<uint32_t>{1}));
    EXPECT_EQ(Steps(3), (std::vector<uint32_t>{2, 1}));
    EXPECT_EQ(Steps(4), (std::vector<uint32_t>{3, 2, 1}));
}

TEST(Digi00xRegisters, StartStepsRefuseUnobservedStates) {
    EXPECT_FALSE(StartStepsFor(5).has_value());
    EXPECT_FALSE(StartStepsFor(0xffffffffU).has_value());
}

TEST(Digi00xRegisters, StopAndTimingValues) {
    EXPECT_EQ(kStreamingStop, 3U);
    EXPECT_EQ(kStopSettleMs, 50U);
    EXPECT_EQ(kStartStepDelayMs, 20U);
}

TEST(Digi00xRegisters, MessageAddressCarriesNodeIdAndThe48BitAddress) {
    const auto words = MessageAddressWords(0xffc1, 0xffffe0001234ULL);
    EXPECT_EQ(words[0], 0xffc1ffffU);
    EXPECT_EQ(words[1], 0xe0001234U);
}

} // namespace
