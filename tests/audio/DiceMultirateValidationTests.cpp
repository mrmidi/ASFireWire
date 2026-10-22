// SPDX-License-Identifier: Apache-2.0
#include "Audio/Protocols/DICE/Core/DiceRateFormats.hpp"
#include "Audio/Runtime/RateValidation.hpp"
#include <gtest/gtest.h>

TEST(DiceMultirateValidation, ExplicitBatchEnablesKnownModesAndMatchingClockEncoding) {
    using namespace ASFW::Audio;
    static_assert(Runtime::kDiceHardwareBatch);
    DICE::DiceRateFormats modes;
    modes[0] = DICE::DiceModeFormat{{{16, 17, 1, {}, 8}}, {{16, 17, 1, {}, 8}}};
    modes[1] = DICE::DiceModeFormat{{{12, 13, 1, {}, 8}}, {{12, 13, 1, {}, 8}}};
    modes[2] = DICE::DiceModeFormat{{{8, 9, 1, {}, 8}}, {{8, 9, 1, {}, 8}}};
    const auto formations = DICE::DiceFormations(0x7f, modes);
    ASSERT_EQ(formations.size(), 7);
    const auto allocation = Runtime::MaximumFormationAllocation(formations, {49152, 16, 16, 0});
    ASSERT_TRUE(allocation);
    for (const auto& formation : formations) {
        EXPECT_TRUE(Runtime::RateEnabled(formation, 48000, true));
        EXPECT_FALSE(formation.hardwareValidated);
        uint32_t encoded = 0;
        ASSERT_TRUE(DICE::DiceClockSelectForRate(formation.sampleRateHz, DICE::ClockSource::Internal, encoded));
        EXPECT_TRUE(DICE::IsSupportedDiceClockConfiguration({formation.sampleRateHz, encoded}, Runtime::kDiceHardwareBatch));
        EXPECT_FALSE(DICE::IsSupportedDiceClockConfiguration({formation.sampleRateHz, encoded ^ 0x100}, true));
        const auto config = Runtime::ResolveAudioConfiguration(formation.sampleRateHz, formations,
            {64, 64, 128, 128}, *allocation, 1, Runtime::ConfigurationValidationPolicy::HardwareBatch);
        ASSERT_TRUE(config) << formation.sampleRateHz;
        EXPECT_EQ(config->playbackChannels, formation.playback[0].pcmChannels);
        EXPECT_EQ(config->captureChannels, formation.capture[0].pcmChannels);
    }
}

// Hardware, 2026-10-08 (Pro 24 DSP device report): attached while locked at
// 88.2 kHz with its stream registers still in the 48 kHz layout (16 in). EAP
// current_config says 2x is 12 in. With EAP, its formats stand for every mode
// (Linux dice-stream.c:618-621); the registers are the fallback only.
TEST(DiceMultirateValidation, EapFormatsAreNotOverwrittenByStaleRegisters) {
    using namespace ASFW::Audio;
    DICE::DiceRateFormats eap;
    eap[0] = DICE::DiceModeFormat{{{8, 9, 1, {}, 1}}, {{16, 17, 1, {}, 1}}};
    eap[1] = DICE::DiceModeFormat{{{8, 9, 1, {}, 1}}, {{12, 13, 1, {}, 1}}};
    eap[2] = DICE::DiceModeFormat{{{8, 9, 1, {}, 1}}, {{8, 9, 1, {}, 1}}};
    AudioStreamRuntimeCaps stale{};
    stale.sampleRateHz = 88200;
    stale.deviceToHostStreamCount = 1;
    stale.deviceToHostStreams[0] = {.pcmChannels = 16, .am824Slots = 17, .midiPorts = 1};
    stale.hostToDeviceStreamCount = 1;
    stale.hostToDeviceStreams[0] = {.pcmChannels = 8, .am824Slots = 9, .midiPorts = 1};

    const auto withEap = DICE::SelectPublishedFormats(true, eap, stale);
    ASSERT_TRUE(withEap[1].has_value());
    ASSERT_EQ(withEap[1]->capture.size(), 1U);
    EXPECT_EQ(withEap[1]->capture[0].pcmChannels, 12U) << "the stale 48 kHz layout must not become 2x";
    EXPECT_TRUE(withEap[0].has_value());
    EXPECT_TRUE(withEap[2].has_value());

    // Without EAP only the observed mode is known (Linux's fallback).
    const auto withoutEap = DICE::SelectPublishedFormats(false, eap, stale);
    EXPECT_FALSE(withoutEap[0].has_value());
    ASSERT_TRUE(withoutEap[1].has_value());
    EXPECT_EQ(withoutEap[1]->capture[0].pcmChannels, 16U);
    EXPECT_FALSE(withoutEap[2].has_value());
}

namespace {
constexpr uint64_t kZedGuid = 0x0004c404000186a1ULL;
ASFW::Audio::AudioStreamRuntimeCaps ZedCaps(uint32_t rate) {
    ASFW::Audio::AudioStreamRuntimeCaps caps{};
    caps.sampleRateHz = rate;
    caps.deviceRateMask = 0x1e;
    caps.deviceToHostStreamCount = caps.hostToDeviceStreamCount = 2;
    const auto second = static_cast<uint16_t>(rate > 48000 ? 2 : 10);
    caps.deviceToHostStreams[0] = {.pcmChannels = 16, .am824Slots = 17, .midiPorts = 1};
    caps.hostToDeviceStreams[0] = {.pcmChannels = 16, .am824Slots = 16, .midiPorts = 0};
    caps.deviceToHostStreams[1] = {.pcmChannels = second, .am824Slots = second, .midiPorts = 0};
    caps.hostToDeviceStreams[1] = caps.deviceToHostStreams[1];
    return caps;
}
} // namespace

// ZED-R16 format-selection regression fixtures for issue #223.
TEST(DiceMultirateValidation, ZedMeasuredModesResolveFromLowAndHighRateAttachment) {
    using namespace ASFW::Audio;
    for (const auto attachedRate : {44100U, 48000U, 88200U, 96000U}) {
        const auto caps = ZedCaps(attachedRate);
        const auto modes = DICE::SelectPublishedFormats(false, {}, caps, kZedGuid);
        const auto formations = DICE::DiceFormations(caps.deviceRateMask, modes);
        ASSERT_EQ(formations.size(), 4U);
        const auto allocation = Runtime::MaximumFormationAllocation(formations, {49152, 26, 26, 0});
        ASSERT_TRUE(allocation);
        for (const auto& formation : formations) {
            EXPECT_FALSE(formation.hardwareValidated);
            ASSERT_EQ(formation.capture.size(), 2U);
            ASSERT_EQ(formation.playback.size(), 2U);
            EXPECT_EQ(formation.capture[0].dataBlockSize, 17U);
            EXPECT_EQ(formation.capture[0].midiPortCount, 1U);
            EXPECT_EQ(formation.playback[0].dataBlockSize, 16U);
            EXPECT_EQ(formation.playback[0].midiPortCount, 0U);
            const auto config = Runtime::ResolveAudioConfiguration(
                formation.sampleRateHz, formations, {64, 64, 128, 128}, *allocation, 1,
                Runtime::ConfigurationValidationPolicy::HardwareBatch);
            ASSERT_TRUE(config) << formation.sampleRateHz;
            const auto expected = formation.sampleRateHz > 48000 ? 18U : 26U;
            EXPECT_EQ(config->captureChannels, expected);
            EXPECT_EQ(config->playbackChannels, expected);
        }
        EXPECT_EQ(DICE::DiceFormations(0x06, modes).size(), 2U);
        EXPECT_EQ(DICE::DiceFormations(0x60, modes).size(), 0U);
    }
}

TEST(DiceMultirateValidation, ZedFallbackRejectsOtherIdentityAndUnmeasuredGeometry) {
    using namespace ASFW::Audio;
    auto caps = ZedCaps(48000);
    for (const auto guid :
         {0ULL, kZedGuid ^ (1ULL << 40), kZedGuid ^ (1ULL << 32), kZedGuid ^ (1ULL << 22)})
        EXPECT_FALSE(DICE::ZedR16MeasuredFormats(guid, caps));
    EXPECT_TRUE(DICE::ZedR16MeasuredFormats(kZedGuid + 1, caps));
    caps.hostToDeviceStreams[0].am824Slots = 17;
    EXPECT_FALSE(DICE::ZedR16MeasuredFormats(kZedGuid, caps));
    caps = ZedCaps(48000);
    caps.hostToDeviceStreams[1].pcmChannels = 8;
    caps.hostToDeviceStreams[1].am824Slots = 8;
    EXPECT_FALSE(DICE::ZedR16MeasuredFormats(kZedGuid, caps));
    caps = ZedCaps(48000);
    caps.deviceToHostStreams[0].midiPorts = 2;
    EXPECT_FALSE(DICE::ZedR16MeasuredFormats(kZedGuid, caps));
    caps = ZedCaps(48000);
    caps.sampleRateHz = 96000; // stale low-rate descriptors must not authorize the table
    EXPECT_FALSE(DICE::ZedR16MeasuredFormats(kZedGuid, caps));
    caps = ZedCaps(32000); // not a supported ZED attachment state
    EXPECT_FALSE(DICE::ZedR16MeasuredFormats(kZedGuid, caps));
    caps = ZedCaps(48000);
    caps.hostToDeviceStreamCount = 1;
    EXPECT_FALSE(DICE::ZedR16MeasuredFormats(kZedGuid, caps));
}

TEST(DiceMultirateValidation, ZedNeverOverridesSuccessfulEapEvenWhenEmpty) {
    using namespace ASFW::Audio;
    const auto caps = ZedCaps(48000);
    DICE::DiceRateFormats eap{};
    EXPECT_FALSE(DICE::SelectPublishedFormats(true, eap, caps, kZedGuid)[0]);
    eap[1] = DICE::DiceModeFormat{{{8, 8, 0, {}, 0}}, {{8, 8, 0, {}, 0}}};
    const auto modes = DICE::SelectPublishedFormats(true, eap, caps, kZedGuid);
    EXPECT_FALSE(modes[0]);
    ASSERT_TRUE(modes[1]);
    EXPECT_EQ(modes[1]->capture[0].pcmChannels, 8U);
}

TEST(DiceMultirateValidation, NonZedIdentityKeepsObservedModeOnly) {
    using namespace ASFW::Audio;
    const auto caps = ZedCaps(48000);
    const auto modes = DICE::SelectPublishedFormats(false, {}, caps, kZedGuid ^ (1ULL << 22));
    ASSERT_TRUE(modes[0]);
    EXPECT_FALSE(modes[1]);
    EXPECT_FALSE(modes[2]);
    EXPECT_EQ(DICE::DiceFormations(caps.deviceRateMask, modes).size(), 2U);
}
