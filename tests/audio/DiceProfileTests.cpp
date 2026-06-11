// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// DiceProfileTests.cpp
// Unit tests for the Dext DICE audio profile registry and profiles.

#include <gtest/gtest.h>

#include "Audio/DriverKit/Config/AudioProfileRegistry.hpp"
#include "Audio/DriverKit/Config/DICE/DiceDeviceProfile.hpp"
#include "Audio/DriverKit/Config/DICE/DiceProfileRegistry.hpp"
#include "Audio/DriverKit/Config/DICE/Isoch/Profiles/FocusriteSaffireProfile.hpp"
#include "Audio/DriverKit/Config/DICE/Isoch/Profiles/GenericDiceProfile.hpp"

namespace {

using namespace ASFW::Isoch::Audio;
using namespace ASFW::Isoch::Audio::DICE;

TEST(DiceProfileTests, ResolvesFocusriteSaffireProfileByVendor) {
    const uint32_t kFocusriteVendorId = 0x00130E;
    const auto* profile = AudioProfileRegistry::FindProfile(kFocusriteVendorId, 0x000007, 0x123456789ULL);

    ASSERT_NE(profile, nullptr);
    EXPECT_STREQ(profile->Name(), "Focusrite Saffire (DICE)");
    EXPECT_EQ(profile->TxWireFormat(), ASFW::Encoding::AudioWireFormat::kRawPcm24In32);
    EXPECT_EQ(profile->RxWireFormat(), ASFW::Encoding::AudioWireFormat::kAM824);

    EXPECT_EQ(profile->TxChannelCount(), 8);
    EXPECT_EQ(profile->RxChannelCount(), 16);
    EXPECT_EQ(profile->TxMidiSlots(), 1);
    EXPECT_EQ(profile->RxMidiSlots(), 1);
    EXPECT_EQ(profile->TxDbs(), 9);
    EXPECT_EQ(profile->RxDbs(), 17);
}

TEST(DiceProfileTests, ResolvesGenericDiceProfileForUnknownDevices) {
    const auto* profile = AudioProfileRegistry::FindProfile(0x999999, 0x000001, 0x123456789ULL);

    ASSERT_NE(profile, nullptr);
    EXPECT_STREQ(profile->Name(), "Generic DICE");
    EXPECT_EQ(profile->TxWireFormat(), ASFW::Encoding::AudioWireFormat::kAM824);
    EXPECT_EQ(profile->RxWireFormat(), ASFW::Encoding::AudioWireFormat::kAM824);

    EXPECT_EQ(profile->TxChannelCount(), 2);
    EXPECT_EQ(profile->RxChannelCount(), 2);
    EXPECT_EQ(profile->TxMidiSlots(), 0);
    EXPECT_EQ(profile->RxMidiSlots(), 0);
}

TEST(DiceProfileTests, FocusriteAsymmetricSafetyOffsetsAndLatencies) {
    const uint32_t kFocusriteVendorId = 0x00130E;
    const auto* profile = AudioProfileRegistry::FindProfile(kFocusriteVendorId, 0x000007, 0x123456789ULL);
    ASSERT_NE(profile, nullptr);

    // 48 kHz
    // Tx (Output): 6 packets * 8 frames = 48 frames
    EXPECT_EQ(profile->TxSafetyOffsetFrames(48000.0), 48);
    // Rx (Input): 16 packets * 8 frames = 128 frames
    EXPECT_EQ(profile->RxSafetyOffsetFrames(48000.0), 128);
    EXPECT_EQ(profile->TxReportedLatencyFrames(48000.0), 29);
    EXPECT_EQ(profile->RxReportedLatencyFrames(48000.0), 29);

    // 96 kHz
    // Tx (Output): (6 + 2) packets * 16 frames = 128 frames
    EXPECT_EQ(profile->TxSafetyOffsetFrames(96000.0), 128);
    // Rx (Input): (16 + 2) packets * 16 frames = 288 frames
    EXPECT_EQ(profile->RxSafetyOffsetFrames(96000.0), 288);
    EXPECT_EQ(profile->TxReportedLatencyFrames(96000.0), 59);
    EXPECT_EQ(profile->RxReportedLatencyFrames(96000.0), 59);
}

TEST(DiceProfileTests, GenericDiceDefaultOffsetsAndLatencies) {
    const auto* profile = AudioProfileRegistry::FindProfile(0x999999, 0x000001, 0x123456789ULL);
    ASSERT_NE(profile, nullptr);

    EXPECT_EQ(profile->TxSafetyOffsetFrames(48000.0), 64);
    EXPECT_EQ(profile->RxSafetyOffsetFrames(48000.0), 64);
    EXPECT_EQ(profile->TxReportedLatencyFrames(48000.0), 128);
    EXPECT_EQ(profile->RxReportedLatencyFrames(48000.0), 128);
}

} // namespace
