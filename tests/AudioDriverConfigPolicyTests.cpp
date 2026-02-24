#include <gtest/gtest.h>

#include "Isoch/Audio/AudioDriverConfig.hpp"

#include <cstring>

namespace {

using ASFW::Isoch::Audio::BoolControlDescriptor;
using ASFW::Isoch::Audio::ParsedAudioDriverConfig;
using ASFW::Isoch::Audio::StreamMode;

TEST(AudioDriverConfigPolicyTests, InitializesExpectedDefaults) {
    ParsedAudioDriverConfig config{};
    ASFW::Isoch::Audio::InitializeAudioDriverConfigDefaults(config);

    EXPECT_STREQ(config.deviceName, "FireWire Audio");
    EXPECT_EQ(config.channelCount, ASFW::Isoch::Audio::kDefaultChannelCount);
    EXPECT_EQ(config.inputChannelCount, ASFW::Isoch::Audio::kDefaultChannelCount);
    EXPECT_EQ(config.outputChannelCount, ASFW::Isoch::Audio::kDefaultChannelCount);
    EXPECT_DOUBLE_EQ(config.sampleRates[0], ASFW::Isoch::Audio::kDefaultSampleRate);
    EXPECT_EQ(config.sampleRateCount, 1u);
    EXPECT_DOUBLE_EQ(config.currentSampleRate, ASFW::Isoch::Audio::kDefaultSampleRate);
    EXPECT_EQ(config.streamMode, StreamMode::kNonBlocking);
    EXPECT_STREQ(config.inputPlugName, "Input");
    EXPECT_STREQ(config.outputPlugName, "Output");
    EXPECT_STREQ(config.inputChannelNames[0], "In 1");
    EXPECT_STREQ(config.outputChannelNames[1], "Out 2");
}

TEST(AudioDriverConfigPolicyTests, ClampChannelsFallsBackToDefaults) {
    ParsedAudioDriverConfig config{};
    ASFW::Isoch::Audio::InitializeAudioDriverConfigDefaults(config);
    config.channelCount = 0;
    config.inputChannelCount = 0;
    config.outputChannelCount = 0;

    ASFW::Isoch::Audio::ClampAudioDriverChannels(config, 16);

    EXPECT_EQ(config.channelCount, ASFW::Isoch::Audio::kDefaultChannelCount);
    EXPECT_EQ(config.inputChannelCount, ASFW::Isoch::Audio::kDefaultChannelCount);
    EXPECT_EQ(config.outputChannelCount, ASFW::Isoch::Audio::kDefaultChannelCount);
}

TEST(AudioDriverConfigPolicyTests, ClampChannelsInheritsAggregateWhenDirectionalCountsMissing) {
    ParsedAudioDriverConfig config{};
    ASFW::Isoch::Audio::InitializeAudioDriverConfigDefaults(config);
    config.channelCount = 48;
    config.inputChannelCount = 0;
    config.outputChannelCount = 0;

    ASFW::Isoch::Audio::ClampAudioDriverChannels(config, 16);

    EXPECT_EQ(config.channelCount, 48u);
    EXPECT_EQ(config.inputChannelCount, 48u);
    EXPECT_EQ(config.outputChannelCount, 48u);
}

TEST(AudioDriverConfigPolicyTests, ClampChannelsRespectsMaxSupportedForExplicitDirectionalCounts) {
    ParsedAudioDriverConfig config{};
    ASFW::Isoch::Audio::InitializeAudioDriverConfigDefaults(config);
    config.channelCount = 48;
    config.inputChannelCount = 32;
    config.outputChannelCount = 24;

    ASFW::Isoch::Audio::ClampAudioDriverChannels(config, 16);

    EXPECT_EQ(config.inputChannelCount, 16u);
    EXPECT_EQ(config.outputChannelCount, 16u);
    EXPECT_EQ(config.channelCount, 16u);
}

TEST(AudioDriverConfigPolicyTests, BuildFallbackBoolControlsMapsPhantomMask) {
    ParsedAudioDriverConfig config{};
    ASFW::Isoch::Audio::InitializeAudioDriverConfigDefaults(config);
    config.boolControlCount = 0;
    config.hasPhantomOverride = true;
    config.phantomSupportedMask = 0b1011U;  // elements 1,2,4
    config.phantomInitialMask = 0b1001U;    // elements 1,4 enabled

    ASFW::Isoch::Audio::BuildFallbackBoolControls(config);

    ASSERT_EQ(config.boolControlCount, 3u);
    const BoolControlDescriptor& first = config.boolControls[0];
    const BoolControlDescriptor& second = config.boolControls[1];
    const BoolControlDescriptor& third = config.boolControls[2];

    EXPECT_EQ(first.classIdFourCC, ASFW::Isoch::Audio::kClassIdPhantomPower);
    EXPECT_EQ(first.scopeFourCC, ASFW::Isoch::Audio::kScopeInput);
    EXPECT_EQ(first.element, 1u);
    EXPECT_TRUE(first.initialValue);

    EXPECT_EQ(second.element, 2u);
    EXPECT_FALSE(second.initialValue);

    EXPECT_EQ(third.element, 4u);
    EXPECT_TRUE(third.initialValue);
}

TEST(AudioDriverConfigPolicyTests, BuildFallbackBoolControlsIsNoopWhenOverridesExist) {
    ParsedAudioDriverConfig config{};
    ASFW::Isoch::Audio::InitializeAudioDriverConfigDefaults(config);
    config.boolControlCount = 1;
    config.boolControls[0] = BoolControlDescriptor{
        .classIdFourCC = static_cast<uint32_t>('test'),
        .scopeFourCC = ASFW::Isoch::Audio::kScopeInput,
        .element = 7,
        .isSettable = false,
        .initialValue = false,
    };
    config.hasPhantomOverride = true;
    config.phantomSupportedMask = 0xFFFF;

    ASFW::Isoch::Audio::BuildFallbackBoolControls(config);

    EXPECT_EQ(config.boolControlCount, 1u);
    EXPECT_EQ(config.boolControls[0].element, 7u);
}

TEST(AudioDriverConfigPolicyTests, BringupPolicyForcesSingle48kFormat) {
    ParsedAudioDriverConfig config{};
    ASFW::Isoch::Audio::InitializeAudioDriverConfigDefaults(config);
    config.sampleRateCount = 3;
    config.sampleRates[0] = 44100;
    config.sampleRates[1] = 48000;
    config.sampleRates[2] = 96000;
    config.currentSampleRate = 96000;

    ASFW::Isoch::Audio::ApplyBringupSingleFormatPolicy(config);

    EXPECT_EQ(config.sampleRateCount, 1u);
    EXPECT_DOUBLE_EQ(config.sampleRates[0], ASFW::Isoch::Audio::kDefaultSampleRate);
    EXPECT_DOUBLE_EQ(config.currentSampleRate, ASFW::Isoch::Audio::kDefaultSampleRate);
}

TEST(AudioDriverConfigPolicyTests, ScopeLabelMapsKnownScopes) {
    EXPECT_STREQ(ASFW::Isoch::Audio::ScopeLabel(static_cast<uint32_t>('inpt')), "Input");
    EXPECT_STREQ(ASFW::Isoch::Audio::ScopeLabel(static_cast<uint32_t>('outp')), "Output");
    EXPECT_STREQ(ASFW::Isoch::Audio::ScopeLabel(static_cast<uint32_t>('glob')), "Global");
    EXPECT_STREQ(ASFW::Isoch::Audio::ScopeLabel(static_cast<uint32_t>('none')), "Scope");
}

} // namespace
