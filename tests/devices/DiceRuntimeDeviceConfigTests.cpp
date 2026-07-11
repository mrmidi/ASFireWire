#include <gtest/gtest.h>

#include "Audio/Protocols/Backends/DiceRuntimeDeviceConfig.hpp"

namespace {

using ASFW::Audio::ApplyDiceRuntimeCapsToDeviceConfig;
using ASFW::Audio::AudioStreamRuntimeCaps;
using ASFW::Audio::Model::ASFWAudioDevice;

TEST(DiceRuntimeDeviceConfigTests, AppliesDiscoveredPcmGeometryAndRateWithoutChannelTable) {
    ASFWAudioDevice config{};
    config.deviceName = "Generic DICE";

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
    EXPECT_EQ(config.currentSampleRate, 48000U);
    ASSERT_EQ(config.sampleRates.size(), 1U);
    EXPECT_EQ(config.sampleRates[0], 48000U);
    EXPECT_EQ(config.deviceName, "Generic DICE");
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

} // namespace
