#include <gtest/gtest.h>

#include "Audio/Protocols/MOTU/MOTU828Mk3RegisterModel.hpp"

#include <array>
#include <cstdint>

namespace {

using namespace ASFW::Audio;
using namespace ASFW::Audio::MOTU;

} // namespace

TEST(Motu828Mk3RegisterModelTests, PinsCapturedPrepareWriteSequence) {
    ASSERT_EQ(kPrepareWrites.size(), 5u);

    EXPECT_EQ(kPrepareWrites[0].offset, 0x0b38u);
    EXPECT_EQ(kPrepareWrites[0].quadlets[0], 0xffc20002u);
    EXPECT_EQ(kPrepareWrites[0].quadlets[1], 0x00000000u);
    EXPECT_EQ(kPrepareWrites[0].quadletCount, 2u);
    EXPECT_FALSE(kPrepareWrites[0].fatalOnFailure);

    EXPECT_EQ(kPrepareWrites[1].offset, 0x0b08u);
    EXPECT_EQ(kPrepareWrites[1].quadlets[0], 0xffffffffu);
    EXPECT_EQ(kPrepareWrites[2].offset, 0x0b04u);
    EXPECT_EQ(kPrepareWrites[2].quadlets[0], 0xffc10001u);
    EXPECT_EQ(kPrepareWrites[3].offset, 0x0b08u);
    EXPECT_EQ(kPrepareWrites[3].quadlets[0], 0x00000000u);
    EXPECT_EQ(kPrepareWrites[4].offset, 0x0b10u);
    EXPECT_EQ(kPrepareWrites[4].quadlets[0], 0x00000002u);
    EXPECT_TRUE(kPrepareWrites[4].fatalOnFailure);
}

TEST(Motu828Mk3RegisterModelTests, IsocWordsZeroLowBitsAndEncodeSixBitChannels) {
    constexpr AudioDuplexChannels channels{
        .deviceToHostIsoChannel = 34,
        .hostToDeviceIsoChannel = 33,
    };
    // The device-reported low half (0x5b59) is intentionally dropped: the
    // official driver always writes low 16 bits = 0. currentControl no longer
    // reaches the produced word.
    const auto words = BuildIsocControlWords(0x00005b59u, channels);
    ASSERT_TRUE(words.has_value());
    EXPECT_EQ(words->deactivate, 0x80800000u);
    EXPECT_EQ(words->activate, 0xe1e20000u);
    // BuildStopIsocControl (teardown) is unchanged; only its input word lost the
    // low half, so its output follows. This is not the teardown parity fix.
    EXPECT_EQ(BuildStopIsocControl(words->activate), 0xa1a20000u);
}

TEST(Motu828Mk3RegisterModelTests, RejectsOutOfRangeIsoChannels) {
    AudioDuplexChannels channels{
        .deviceToHostIsoChannel = 64,
        .hostToDeviceIsoChannel = 33,
    };
    EXPECT_FALSE(BuildIsocControlWords(0, channels).has_value());
    channels.deviceToHostIsoChannel = 34;
    channels.hostToDeviceIsoChannel = 64;
    EXPECT_FALSE(BuildIsocControlWords(0, channels).has_value());
}

TEST(Motu828Mk3RegisterModelTests, FetchCommandDoesNotChangeRateField) {
    constexpr uint32_t clock48k = 0x00000108u;
    const uint32_t fetch = SetFetchPcmFrames(clock48k);
    EXPECT_EQ(fetch, 0x02000108u);
    EXPECT_EQ(ClearFetchPcmFrames(fetch), clock48k);
    ASSERT_TRUE(SampleRateFromClockStatus(fetch).has_value());
    EXPECT_EQ(*SampleRateFromClockStatus(fetch), 48000u);
}

TEST(Motu828Mk3RegisterModelTests, DecodesAllCapturedClockRateIndexes) {
    constexpr std::array<uint32_t, 6> rates{
        44100u, 48000u, 88200u, 96000u, 176400u, 192000u,
    };
    for (uint32_t index = 0; index < rates.size(); ++index) {
        const auto rate = SampleRateFromClockStatus(index << kClockRateShift);
        ASSERT_TRUE(rate.has_value());
        EXPECT_EQ(*rate, rates[index]);
    }
    EXPECT_FALSE(SampleRateFromClockStatus(6u << kClockRateShift).has_value());
}

TEST(Motu828Mk3RegisterModelTests, ExposesOnlyMeasuredStreamConfigWrite) {
    ASSERT_TRUE(StreamConfigForSampleRate(48000).has_value());
    EXPECT_EQ(*StreamConfigForSampleRate(48000), 0x00120000u);
    EXPECT_FALSE(StreamConfigForSampleRate(44100).has_value());
    EXPECT_FALSE(StreamConfigForSampleRate(96000).has_value());
}
