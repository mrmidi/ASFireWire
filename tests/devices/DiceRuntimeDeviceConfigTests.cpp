#include <gtest/gtest.h>

#include "Audio/Protocols/Backends/DiceRuntimeDeviceConfig.hpp"

namespace {

using ASFW::Audio::ApplyDiceRuntimeCapsToDeviceConfig;
using ASFW::Audio::AudioStreamRuntimeCaps;
using ASFW::Audio::Model::ASFWAudioDevice;

TEST(DiceRuntimeDeviceConfigTests, AppliesDiscoveredPcmGeometryWithoutChannelTable) {
    ASFWAudioDevice config{};
    config.deviceName = "Generic DICE";
    // Seeded by the caller before runtime caps arrive: the profile's supported
    // rate set, and the 48 kHz DICE default that bring-up programs.
    config.sampleRates = {44100U, 48000U};
    config.currentSampleRate = 48000U;

    const AudioStreamRuntimeCaps caps{
        .hostInputPcmChannels = 16,
        .hostOutputPcmChannels = 2,
        .deviceToHostAm824Slots = 16,
        .hostToDeviceAm824Slots = 2,
        .sampleRateHz = 48000,
        .deviceToHostStreamCount = 2,
        .hostToDeviceStreamCount = 1,
    };

    ASSERT_TRUE(ApplyDiceRuntimeCapsToDeviceConfig(caps, config));
    EXPECT_EQ(config.inputChannelCount, 16U);
    EXPECT_EQ(config.outputChannelCount, 2U);
    EXPECT_EQ(config.channelCount, 16U);
    EXPECT_EQ(config.deviceName, "Generic DICE");

    // The clock is not adopted from the device: the advertised set survives and
    // the default stays at what bring-up will actually program.
    EXPECT_EQ(config.currentSampleRate, 48000U);
    ASSERT_EQ(config.sampleRates.size(), 2U);
    EXPECT_EQ(config.sampleRates[0], 44100U);
    EXPECT_EQ(config.sampleRates[1], 48000U);
}

TEST(DiceRuntimeDeviceConfigTests, DeviceFoundAtNon48kKeepsTheAdvertisedRateSet) {
    // The regression: a device discovered at 44.1 kHz used to be published as a
    // 44.1-kHz-only device -- every other rate refused by ASFWAudioDevice's
    // validation -- while bring-up drove it at 48 kHz. Both halves must hold.
    ASFWAudioDevice config{};
    config.sampleRates = {44100U, 48000U};
    config.currentSampleRate = 48000U;

    const AudioStreamRuntimeCaps caps{
        .hostInputPcmChannels = 14,
        .hostOutputPcmChannels = 2,
        .deviceToHostAm824Slots = 14,
        .hostToDeviceAm824Slots = 2,
        .sampleRateHz = 44100,
        .deviceToHostStreamCount = 1,
        .hostToDeviceStreamCount = 1,
    };

    ASSERT_TRUE(ApplyDiceRuntimeCapsToDeviceConfig(caps, config));

    // Geometry still comes from the device.
    EXPECT_EQ(config.inputChannelCount, 14U);
    EXPECT_EQ(config.outputChannelCount, 2U);

    // The clock does not.
    EXPECT_EQ(config.currentSampleRate, 48000U);
    ASSERT_EQ(config.sampleRates.size(), 2U);
    EXPECT_EQ(config.sampleRates[0], 44100U);
    EXPECT_EQ(config.sampleRates[1], 48000U);
}

TEST(DiceRuntimeDeviceConfigTests, RejectsPartialCapsWithoutChangingFallbackConfig) {
    ASFWAudioDevice config{};
    const ASFWAudioDevice before = config;
    const AudioStreamRuntimeCaps partial{
        .hostInputPcmChannels = 16,
        .hostOutputPcmChannels = 0,
        .sampleRateHz = 48000,
    };

    EXPECT_FALSE(ApplyDiceRuntimeCapsToDeviceConfig(partial, config));
    EXPECT_EQ(config.inputChannelCount, before.inputChannelCount);
    EXPECT_EQ(config.outputChannelCount, before.outputChannelCount);
    EXPECT_EQ(config.channelCount, before.channelCount);
    EXPECT_EQ(config.currentSampleRate, before.currentSampleRate);
    EXPECT_EQ(config.sampleRates, before.sampleRates);
}

TEST(DiceRuntimeDeviceConfigTests, AppliesPlaybackOnlyCoreAudioGeometryWithDuplexWireCaps) {
    ASFWAudioDevice config{};
    config.sampleRates = {44100U, 48000U};
    config.currentSampleRate = 48000U;
    const AudioStreamRuntimeCaps caps{
        .hostInputPcmChannels = 0,
        .hostOutputPcmChannels = 2,
        .deviceToHostAm824Slots = 2,
        .hostToDeviceAm824Slots = 2,
        .sampleRateHz = 48000,
        .deviceToHostStreamCount = 1,
        .hostToDeviceStreamCount = 1,
    };

    ASSERT_TRUE(ApplyDiceRuntimeCapsToDeviceConfig(caps, config));
    EXPECT_EQ(config.inputChannelCount, 0U);
    EXPECT_EQ(config.outputChannelCount, 2U);
    EXPECT_EQ(config.channelCount, 2U);
}

} // namespace
