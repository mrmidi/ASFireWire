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
