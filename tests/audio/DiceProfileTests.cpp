// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceProfileTests.cpp
// Unit tests for the Dext DICE audio profile registry and profiles.

#include <gtest/gtest.h>

#include "Audio/DriverKit/Config/AudioProfileRegistry.hpp"
#include "Audio/DriverKit/Config/AudioStreamProfile.hpp"
#include "Audio/DriverKit/Config/DICE/DiceDeviceProfile.hpp"
#include "Audio/DriverKit/Config/DICE/DiceProfileRegistry.hpp"
#include "Audio/DriverKit/Config/DICE/Isoch/Profiles/FocusriteSaffireProfile.hpp"
#include "Audio/DriverKit/Config/DICE/Isoch/Profiles/GenericDiceProfile.hpp"
#include "Audio/DriverKit/Config/DICE/Isoch/Profiles/MidasVeniceProfile.hpp"
#include "Audio/DriverKit/Config/DICE/Isoch/Profiles/PreSonusStudioLiveProfile.hpp"
#include "Audio/DriverKit/Config/AVC/ApogeeDuetProfile.hpp"
#include "Audio/DriverKit/Config/AVC/Phase88Profile.hpp"
#include "Audio/Protocols/BeBoB/BeBoBPlug0StreamDiscovery.hpp"

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

    const auto* diceProfile =
        static_cast<const IDiceDeviceProfile*>(profile);
    EXPECT_TRUE(diceProfile->Quirks().tx.preserveFdfInNoDataPackets);
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

    const auto* diceProfile =
        static_cast<const IDiceDeviceProfile*>(profile);
    EXPECT_FALSE(diceProfile->Quirks().tx.preserveFdfInNoDataPackets);
}

TEST(DiceProfileTests, ResolvesApogeeDuetProfileWithoutDICEName) {
    const auto* profile = AudioProfileRegistry::FindProfile(
        0x0003DB, 0x01DDDD, 0x0003DB0A0000D112ULL);

    ASSERT_NE(profile, nullptr);
    EXPECT_STREQ(profile->Name(), "Duet");
    EXPECT_EQ(profile->TxWireFormat(), ASFW::Encoding::AudioWireFormat::kAM824);
    EXPECT_EQ(profile->RxWireFormat(), ASFW::Encoding::AudioWireFormat::kAM824);
    EXPECT_EQ(profile->TxChannelCount(), 2u);
    EXPECT_EQ(profile->RxChannelCount(), 2u);
    EXPECT_EQ(profile->SupportedSampleRates(), (std::vector<uint32_t>{44100u, 48000u}));
}

TEST(DiceProfileTests, ResolvesPhase88AsAvcWireProfileNotGenericDice) {
    const auto* profile = AudioProfileRegistry::FindProfile(
        0x000AAC, 0x000003, 0x000AAC0300B1D1F7ULL);

    ASSERT_NE(profile, nullptr);
    EXPECT_STREQ(profile->Name(), "PHASE 88 Rack FW");
    EXPECT_EQ(profile->TxWireFormat(), ASFW::Encoding::AudioWireFormat::kAM824);
    EXPECT_EQ(profile->RxWireFormat(), ASFW::Encoding::AudioWireFormat::kAM824);
    EXPECT_EQ(profile->TxChannelCount(), 10U);
    EXPECT_EQ(profile->RxChannelCount(), 10U);
    EXPECT_EQ(profile->TxMidiSlots(), 1U);
    EXPECT_EQ(profile->RxMidiSlots(), 1U);
    EXPECT_EQ(profile->TxDbs(), 11U);
    EXPECT_EQ(profile->RxDbs(), 11U);
    EXPECT_EQ(profile->SupportedSampleRates(), (std::vector<uint32_t>{48000U}));

    const auto* wireProfile = static_cast<const IAudioStreamProfile*>(profile);
    AudioStreamConfig tx{};
    ASSERT_TRUE(wireProfile->BuildDefaultTxStreamConfig(tx));
    EXPECT_EQ(tx.framesPerDataPacket, 8U);
    EXPECT_EQ(8U + tx.framesPerDataPacket * tx.dbs * 4U, 360U);
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

TEST(DiceProfileTests, ResolvesMidasVeniceProfileByVendorAndModel) {
    const auto* profile = AudioProfileRegistry::FindProfile(0x10c73f, 0x000001, 0x10c73f04004011dfULL);

    ASSERT_NE(profile, nullptr);
    EXPECT_STREQ(profile->Name(), "Midas Venice F32 (DICE)");
    EXPECT_EQ(profile->TxWireFormat(), ASFW::Encoding::AudioWireFormat::kRawPcm24In32);
    EXPECT_EQ(profile->RxWireFormat(), ASFW::Encoding::AudioWireFormat::kAM824);

    // Venice F32 at 48 kHz: 2 streams/direction × 16ch. Both wire configs are
    // per-stream (DBS=16) with Tx/RxStreamCount()==2, so the HAL aggregate is
    // 32 per side.
    EXPECT_EQ(profile->TxChannelCount(), 32); // 16 × 2 streams
    EXPECT_EQ(profile->RxChannelCount(), 32); // 16 × 2 streams
    EXPECT_EQ(profile->TxMidiSlots(), 0);
    EXPECT_EQ(profile->RxMidiSlots(), 0);
    EXPECT_EQ(profile->TxDbs(), 16); // per wire stream
    EXPECT_EQ(profile->RxDbs(), 16); // per wire stream

    const auto* diceProfile = static_cast<const IDiceDeviceProfile*>(profile);
    EXPECT_EQ(diceProfile->TxStreamCount(), 2u);
    EXPECT_EQ(diceProfile->RxStreamCount(), 2u);
    EXPECT_TRUE(diceProfile->Quirks().tx.preserveFdfInNoDataPackets);
    EXPECT_EQ(diceProfile->Quirks().tx.hostToDevicePcmEncoding,
              ASFW::Encoding::AudioWireFormat::kRawPcm24In32);
}

TEST(DiceProfileTests, MidasVeniceSafetyOffsetsAndLatencies) {
    const auto* profile = AudioProfileRegistry::FindProfile(0x10c73f, 0x000001, 0x10c73f04004011dfULL);
    ASSERT_NE(profile, nullptr);

    // 48 kHz: Tx = 6 * 8 = 48, Rx = 16 * 8 = 128
    EXPECT_EQ(profile->TxSafetyOffsetFrames(48000.0), 48);
    EXPECT_EQ(profile->RxSafetyOffsetFrames(48000.0), 128);
    EXPECT_EQ(profile->TxReportedLatencyFrames(48000.0), 29);
    EXPECT_EQ(profile->RxReportedLatencyFrames(48000.0), 29);

    // 96 kHz: Tx = 8 * 16 = 128, Rx = 18 * 16 = 288
    EXPECT_EQ(profile->TxSafetyOffsetFrames(96000.0), 128);
    EXPECT_EQ(profile->RxSafetyOffsetFrames(96000.0), 288);
    EXPECT_EQ(profile->TxReportedLatencyFrames(96000.0), 59);
    EXPECT_EQ(profile->RxReportedLatencyFrames(96000.0), 59);
}

TEST(DiceProfileTests, MidasVendorWithWrongModelDoesNotMatchVeniceProfile) {
    // Only vendor+model together should match — no vendor-only fallback for Midas.
    const auto* profile = AudioProfileRegistry::FindProfile(0x10c73f, 0x999999, 0x0ULL);
    // Should fall through to generic profile, not Venice.
    if (profile != nullptr) {
        EXPECT_STRNE(profile->Name(), "Midas Venice F32 (DICE)");
    }
}

TEST(DiceProfileTests, ResolvesPreSonusStudioLive1602ProfileByVendorAndModel) {
    // Identity captured live from the hardware (2026-07-08): GUID 0x000A920404FE2011,
    // vendor 0x000A92, model 0x000013.
    const auto* profile = AudioProfileRegistry::FindProfile(0x000A92, 0x000013, 0x000A920404FE2011ULL);

    ASSERT_NE(profile, nullptr);
    EXPECT_STREQ(profile->Name(), "PreSonus StudioLive 16.0.2 (DICE)");
    EXPECT_EQ(profile->TxWireFormat(), ASFW::Encoding::AudioWireFormat::kRawPcm24In32);
    EXPECT_EQ(profile->RxWireFormat(), ASFW::Encoding::AudioWireFormat::kAM824);

    // DICE TX/RX sections at 48 kHz: single stream per direction, NB_AUDIO=16,
    // NB_MIDI=1, so DBS = 17 both ways.
    EXPECT_EQ(profile->TxChannelCount(), 16);
    EXPECT_EQ(profile->RxChannelCount(), 16);
    EXPECT_EQ(profile->TxMidiSlots(), 1);
    EXPECT_EQ(profile->RxMidiSlots(), 1);
    EXPECT_EQ(profile->TxDbs(), 17);
    EXPECT_EQ(profile->RxDbs(), 17);

    const auto* diceProfile = static_cast<const IDiceDeviceProfile*>(profile);
    EXPECT_TRUE(diceProfile->Quirks().tx.preserveFdfInNoDataPackets);
    EXPECT_EQ(diceProfile->Quirks().tx.hostToDevicePcmEncoding,
              ASFW::Encoding::AudioWireFormat::kRawPcm24In32);
}

TEST(DiceProfileTests, PreSonusStudioLiveSafetyOffsetsAndLatencies) {
    const auto* profile = AudioProfileRegistry::FindProfile(0x000A92, 0x000013, 0x000A920404FE2011ULL);
    ASSERT_NE(profile, nullptr);

    // Device clock caps are 44.1/48 kHz only; both rates sit in the DICE low rate
    // mode (8 frames per packet). Tx = 6 * 8 = 48, Rx = 16 * 8 = 128.
    EXPECT_EQ(profile->TxSafetyOffsetFrames(44100.0), 48);
    EXPECT_EQ(profile->RxSafetyOffsetFrames(44100.0), 128);
    EXPECT_EQ(profile->TxSafetyOffsetFrames(48000.0), 48);
    EXPECT_EQ(profile->RxSafetyOffsetFrames(48000.0), 128);
    EXPECT_EQ(profile->TxReportedLatencyFrames(48000.0), 29);
    EXPECT_EQ(profile->RxReportedLatencyFrames(48000.0), 29);
}

TEST(DiceProfileTests, PreSonusVendorWithWrongModelDoesNotMatchStudioLiveProfile) {
    // PreSonus also shipped BeBoB-era devices (FireBox/FP10/Inspire), the DICE
    // FireStudio (0x000008), and the StudioLive siblings 16.4.2/24.4.2/32.4.2
    // (0x000010/0x000012/0x000014) whose channel counts are uncaptured; none of
    // them may inherit the 16.0.2 stream geometry.
    for (const uint32_t modelId : {0x000008u, 0x000010u, 0x000012u, 0x000014u}) {
        const auto* profile = AudioProfileRegistry::FindProfile(0x000A92, modelId, 0x0ULL);
        if (profile != nullptr) {
            EXPECT_STRNE(profile->Name(), "PreSonus StudioLive 16.0.2 (DICE)");
        }
    }
}

TEST(DiceProfileTests, GenericDiceDefaultOffsetsAndLatencies) {
    const auto* profile = AudioProfileRegistry::FindProfile(0x999999, 0x000001, 0x123456789ULL);
    ASSERT_NE(profile, nullptr);

    EXPECT_EQ(profile->TxSafetyOffsetFrames(48000.0), 64);
    EXPECT_EQ(profile->RxSafetyOffsetFrames(48000.0), 64);
    EXPECT_EQ(profile->TxReportedLatencyFrames(48000.0), 128);
    EXPECT_EQ(profile->RxReportedLatencyFrames(48000.0), 128);
}

// Discovery-derived geometry for a BeBoB device without a curated profile.
ASFW::Audio::BeBoB::DeviceModel MakeStereoBeBoBDiscoveryModel() {
    ASFW::Audio::BeBoB::DeviceModel model{};
    ASFW::Audio::BeBoB::StreamFormation formation{};
    formation.pcmChannels = 2;
    formation.midiSlots = 0;
    formation.rateCode = 0x02; // 48 kHz
    model.input.supportedFormations.push_back(formation);
    model.output.supportedFormations.push_back(formation);
    return model;
}

TEST(DiceProfileTests, DynamicBeBoBProfileNeverShadowsCuratedPhase88) {
    // Regression for BUGLIST.md Bug 2a: AVCDiscovery registers a per-GUID
    // generic BeBoBProfile for every BeBoB device it probes, including the
    // PHASE 88. The curated Phase88Profile (name, emptyPacketsDuringIdle
    // warm-up policy from FW-105) must still win the lookup.
    const uint64_t kPhase88Guid = 0x000AAC0300B1D1F7ULL;
    const auto model = MakeStereoBeBoBDiscoveryModel();
    ASSERT_NE(AudioProfileRegistry::RegisterBeBoBProfile(kPhase88Guid, &model), nullptr);

    const auto* profile = AudioProfileRegistry::FindProfile(0x000AAC, 0x000003, kPhase88Guid);
    ASSERT_NE(profile, nullptr);
    EXPECT_STREQ(profile->Name(), "PHASE 88 Rack FW");
    const auto* wireProfile = static_cast<const IAudioStreamProfile*>(profile);
    EXPECT_TRUE(wireProfile->TxStreamPolicy().emptyPacketsDuringIdle);

    AudioProfileRegistry::UnregisterProfile(kPhase88Guid);
}

TEST(DiceProfileTests, DynamicBeBoBProfileServesUncuratedBeBoBDevices) {
    // For a BeBoB device with no curated/static match, the per-GUID
    // discovery-derived profile is the resolver (before the DICE generic
    // fallback).
    const uint64_t kUnknownBeBoBGuid = 0x00089ABCDEF01234ULL;
    const auto model = MakeStereoBeBoBDiscoveryModel();
    ASSERT_NE(AudioProfileRegistry::RegisterBeBoBProfile(kUnknownBeBoBGuid, &model), nullptr);

    const auto* profile =
        AudioProfileRegistry::FindProfile(0x0089AB, 0x000042, kUnknownBeBoBGuid);
    ASSERT_NE(profile, nullptr);
    EXPECT_STREQ(profile->Name(), "BeBoB Device");
    EXPECT_EQ(profile->TxChannelCount(), 2U);

    AudioProfileRegistry::UnregisterProfile(kUnknownBeBoBGuid);

    // Without the dynamic registration the same identity falls back to the
    // DICE generic profile.
    const auto* fallback =
        AudioProfileRegistry::FindProfile(0x0089AB, 0x000042, kUnknownBeBoBGuid);
    ASSERT_NE(fallback, nullptr);
    EXPECT_STREQ(fallback->Name(), "Generic DICE");
}

} // namespace
