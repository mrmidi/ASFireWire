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
