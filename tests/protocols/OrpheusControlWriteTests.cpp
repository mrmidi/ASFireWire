// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include <gtest/gtest.h>
#include "ASFWDriver/Audio/Protocols/Prism/Orpheus/OrpheusControlWrite.hpp"

using namespace ASFW::Audio::Prism::Orpheus;
namespace AM = ASFW::AudioModel;

namespace {

OrpheusControlsState MakeState() {
    OrpheusControlsState state;
    state.unit.masterVolume = 0;
    state.unit.adatMode = 3;
    state.syncSource = OrpheusSyncSource::Local;
    state.analog[0].inputType = 0; // Mic
    state.analog[1].inputType = 2; // Inst
    return state;
}

} // namespace

TEST(OrpheusControlWriteTests, GainWriteBuildsMicGainFrame) {
    const auto state = MakeState();
    const auto write = BuildOrpheusParameterWrite(state, AM::ParameterId{OrpheusControl::Gain + 0}, 42.0);
    ASSERT_TRUE(write.has_value());
    EXPECT_EQ(write->group, OrpheusControlGroup::Input);
    ASSERT_EQ(write->frames.size(), 1U);
    const auto& frame = write->frames[0];
    EXPECT_EQ(frame[Wire::kCommandOffset], static_cast<uint8_t>(OrpheusCommand::MicGain));
    EXPECT_EQ(frame[7], 0);  // ch 0
    EXPECT_EQ(frame[8], 42); // gain
    EXPECT_EQ(write->desired.analog[0].micGain, 42);
}

TEST(OrpheusControlWriteTests, InstrumentGainWriteWhenInstrumentInputSelected) {
    const auto state = MakeState(); // analog[1].inputType == 2 (Instrument)
    const auto write = BuildOrpheusParameterWrite(state, AM::ParameterId{OrpheusControl::Gain + 1}, 25.0);
    ASSERT_TRUE(write.has_value());
    EXPECT_EQ(write->group, OrpheusControlGroup::Input);
    ASSERT_EQ(write->frames.size(), 1U);
    const auto& frame = write->frames[0];
    EXPECT_EQ(frame[Wire::kCommandOffset], static_cast<uint8_t>(OrpheusCommand::InstrumentGain));
    EXPECT_EQ(frame[7], 1);  // ch 1
    EXPECT_EQ(frame[8], 25); // inst gain
    EXPECT_EQ(write->desired.analog[1].instrumentGain, 25);
}

TEST(OrpheusControlWriteTests, PhantomPowerWrite) {
    const auto state = MakeState();
    const auto write = BuildOrpheusParameterWrite(state, AM::ParameterId{OrpheusControl::Phantom + 0}, true);
    ASSERT_TRUE(write.has_value());
    EXPECT_EQ(write->group, OrpheusControlGroup::Input);
    ASSERT_EQ(write->frames.size(), 1U);
    const auto& frame = write->frames[0];
    EXPECT_EQ(frame[Wire::kCommandOffset], static_cast<uint8_t>(OrpheusCommand::Phantom));
    EXPECT_EQ(frame[7], 0); // ch 0
    EXPECT_EQ(frame[8], 1); // enabled
    EXPECT_TRUE(write->desired.analog[0].phantom);
}

TEST(OrpheusControlWriteTests, AdatModeWrite) {
    const auto state = MakeState();
    const auto write = BuildOrpheusParameterWrite(state, AM::ParameterId{OrpheusControl::AdatMode}, int64_t(3));
    ASSERT_TRUE(write.has_value());
    EXPECT_EQ(write->group, OrpheusControlGroup::Settings);
    ASSERT_EQ(write->frames.size(), 1U);
    const auto& frame = write->frames[0];
    EXPECT_EQ(frame[Wire::kCommandOffset], static_cast<uint8_t>(OrpheusCommand::AdatMode));
    EXPECT_EQ(frame[7], 3);
    EXPECT_EQ(write->desired.unit.adatMode, 3);
}

TEST(OrpheusControlWriteTests, OutputVolumeWriteSnapsToBigKnobValues) {
    const auto state = MakeState();
    // -10 dB -> -2560 raw
    const auto write = BuildOrpheusParameterWrite(state, AM::ParameterId{OrpheusControl::OutputVolume}, -10.0);
    ASSERT_TRUE(write.has_value());
    EXPECT_EQ(write->group, OrpheusControlGroup::Output);
    ASSERT_EQ(write->frames.size(), 1U);
    const auto& frame = write->frames[0];
    EXPECT_EQ(frame[Wire::kCommandOffset], static_cast<uint8_t>(OrpheusCommand::MasterVolume));
    EXPECT_EQ(write->desired.unit.masterVolume, -2560);
}

TEST(OrpheusControlWriteTests, MixerCrosspointWrite) {
    const auto state = MakeState();
    // Bus 1 (Mix AO 3/4), Input 4: crosspoint = 1 * 12 + 4 = 16
    const auto paramId = AM::ParameterId{OrpheusControl::Mixer + 16};
    const auto write = BuildOrpheusParameterWrite(state, paramId, 0.0); // 0 dB
    ASSERT_TRUE(write.has_value());
    EXPECT_EQ(write->group, OrpheusControlGroup::Mixer);
    ASSERT_EQ(write->frames.size(), 1U);
    const auto& frame = write->frames[0];
    EXPECT_EQ(frame[Wire::kCommandOffset], static_cast<uint8_t>(OrpheusCommand::MixInputGain));
    EXPECT_EQ(frame[7], 1); // Bus 1
    EXPECT_EQ(frame[8], 4); // Input 4
    EXPECT_EQ(frame[9], 0x00); // 0 dB raw
    EXPECT_EQ(frame[10], 0x00);
    EXPECT_EQ(write->desired.mixer[1].inputGains[4], 0);
}

TEST(OrpheusControlWriteTests, ClockSourceWriteHandlesSignalSource) {
    const auto state = MakeState();
    const auto write = BuildOrpheusParameterWrite(state, AM::ParameterId{OrpheusControl::ClockSource}, int64_t(OrpheusSyncSource::WordClock));
    ASSERT_TRUE(write.has_value());
    EXPECT_EQ(write->group, OrpheusControlGroup::ClockSource);
    ASSERT_EQ(write->frames.size(), 1U);
    const auto& frame = write->frames[0];
    EXPECT_EQ(frame[2], 0x1A); // SIGNAL SOURCE
    EXPECT_EQ(write->desired.syncSource, OrpheusSyncSource::WordClock);
}
