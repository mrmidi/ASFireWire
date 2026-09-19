// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceProfileTests.cpp
// Unit tests for the Dext DICE audio profile registry and profiles.

#include <gtest/gtest.h>

#include "Audio/DriverKit/Config/AudioProfileRegistry.hpp"
#include "Audio/DriverKit/Config/AudioStreamProfile.hpp"
#include "Audio/DriverKit/Config/DICE/DiceDeviceProfile.hpp"
#include "DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "Discovery/DiscoveryTypes.hpp"
#include "Audio/DriverKit/Config/DICE/DiceDeviceProfile.hpp"
#include "DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "Discovery/DiscoveryTypes.hpp"
#include "Audio/DriverKit/Config/DICE/Isoch/Profiles/AlesisMultiMixProfile.hpp"
#include "Audio/DriverKit/Config/DICE/Isoch/Profiles/FocusriteSaffireProfile.hpp"
#include "Audio/DriverKit/Config/DICE/Isoch/Profiles/GenericDiceProfile.hpp"
#include "Audio/DriverKit/Config/DICE/Isoch/Profiles/MidasVeniceProfile.hpp"
#include "Audio/DriverKit/Config/DICE/Isoch/Profiles/PreSonusStudioLiveProfile.hpp"
#include "Audio/DriverKit/Config/DICE/Isoch/Profiles/WeissIntProfile.hpp"
#include "Audio/DriverKit/Config/AVC/ApogeeDuetProfile.hpp"
#include "Audio/DriverKit/Config/AVC/MackieOnyx820iProfile.hpp"
#include "Audio/DriverKit/Config/AVC/Phase88Profile.hpp"
#include "Audio/Protocols/BeBoB/BeBoBPlug0StreamDiscovery.hpp"

namespace {

using namespace ASFW::Isoch::Audio;
using namespace ASFW::Isoch::Audio::DICE;

// The profile registry no longer matches on identity: the device catalog does
// that once, and its answer travels to the audio side as a ProfileBuilderId on
// the nub. These tests resolve it the same way production does, from
// Config-ROM evidence, rather than re-stating a mapping that would then be free
// to drift from the catalog.
[[nodiscard]] uint32_t BuilderIdFor(uint32_t vendorId, uint32_t modelId,
                                    uint32_t unitSpecifier, uint32_t unitVersion) {
    ASFW::Discovery::DeviceIdentityEvidence identity{};
    identity.observedGuid = (static_cast<uint64_t>(vendorId) << 40U) | 0x04'0000'0000ULL;
    identity.nodeVendorOui = vendorId;
    identity.rootVendorId = vendorId;
    identity.rootModelId = modelId;
    ASFW::Discovery::UnitIdentityEvidence unit{};
    unit.unitDirectoryOffset = 5;
    unit.specifierId = unitSpecifier;
    unit.version = unitVersion;
    identity.units.push_back(unit);
    return static_cast<uint32_t>(
        ASFW::DeviceProfiles::Audio::AudioDeviceCatalog::ProfileBuilderFor(identity));
}

// A DICE unit publishes the vendor OUI as its specifier with interface
// version 1 (Linux dice.c); a TA 1394 AV/C unit publishes 0x00A02D / 0x010001.
[[nodiscard]] const IAudioDeviceProfile* FindDiceProfile(uint32_t vendorId,
                                                         uint32_t modelId,
                                                         uint64_t guid = 0) {
    return AudioProfileRegistry::FindProfile(
        vendorId, modelId, guid, BuilderIdFor(vendorId, modelId, vendorId, 0x000001));
}

[[nodiscard]] const IAudioDeviceProfile* FindAvcProfile(uint32_t vendorId,
                                                        uint32_t modelId,
                                                        uint64_t guid = 0) {
    return AudioProfileRegistry::FindProfile(
        vendorId, modelId, guid, BuilderIdFor(vendorId, modelId, 0x00A02D, 0x010001));
}

// The base Saffire profile used to match the Focusrite OUI alone, so every
// other Focusrite DICE part inherited its 8-in/16-out geometry. Three devices
// were affected, and the TCD3070 Pro 40 is the one that proves the rule cannot
// be relaxed again: it is a different chip with no TCAT protocol extension, so
// its geometry is not readable from the device at all (Linux hardcodes it,
// dice-focusrite.c:8-22, and Focusrite's own kext has no entry for it).
TEST(DiceProfileTests, FocusriteSiblingsDoNotInheritTheSaffireProfile) {
    constexpr uint32_t kFocusriteVendorId = 0x00130E;
    constexpr uint32_t kSPro40Tcd3070ModelId = 0x0000de;
    constexpr uint32_t kLiquidS56ModelId = 0x000006;
    constexpr uint32_t kSPro26ModelId = 0x000012;

    for (const uint32_t modelId :
         {kSPro40Tcd3070ModelId, kLiquidS56ModelId, kSPro26ModelId}) {
        const auto* profile =
            FindDiceProfile(kFocusriteVendorId, modelId);
        ASSERT_NE(profile, nullptr) << "model 0x" << std::hex << modelId;
        EXPECT_STRNE(profile->Name(), "Focusrite Saffire (DICE)")
            << "model 0x" << std::hex << modelId
            << " inherited a sibling's geometry through vendor-wide matching";
        EXPECT_STRNE(profile->Name(), "Focusrite Saffire Pro 40")
            << "model 0x" << std::hex << modelId;
    }
}

// The three models the base profile does serve.
TEST(DiceProfileTests, TheSaffireProfileStillServesItsOwnModels) {
    constexpr uint32_t kFocusriteVendorId = 0x00130E;
    for (const uint32_t modelId : {0x000009U /*Pro 14*/, 0x000007U /*Pro 24*/,
                                   0x000008U /*Pro 24 DSP*/}) {
        const auto* profile =
            FindDiceProfile(kFocusriteVendorId, modelId);
        ASSERT_NE(profile, nullptr) << "model 0x" << std::hex << modelId;
        EXPECT_STREQ(profile->Name(), "Focusrite Saffire (DICE)")
            << "model 0x" << std::hex << modelId;
    }
}

TEST(DiceProfileTests, ResolvesFocusriteSaffireProfileByVendorAndModel) {
    const uint32_t kFocusriteVendorId = 0x00130E;
    const auto* profile = FindDiceProfile(kFocusriteVendorId, 0x000007, 0x123456789ULL);

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

TEST(DiceProfileTests, ResolvesOriginalPro40LowRateGeometry) {
    constexpr uint32_t kFocusriteVendorId = 0x00130E;
    const auto* profile = FindDiceProfile(kFocusriteVendorId, 0x000005U);

    ASSERT_NE(profile, nullptr);
    EXPECT_STREQ(profile->Name(), "Focusrite Saffire Pro 40");
    EXPECT_EQ(profile->TxChannelCount(), 20U);
    EXPECT_EQ(profile->RxChannelCount(), 20U);
    const auto* streamProfile = static_cast<const IAudioStreamProfile*>(profile);
    EXPECT_EQ(streamProfile->TxStreamCount(), 2U);
    EXPECT_EQ(streamProfile->RxStreamCount(), 2U);
    EXPECT_EQ(profile->TxWireFormat(), ASFW::Encoding::AudioWireFormat::kAM824);

    AudioStreamConfig primary{}, secondary{};
    ASSERT_TRUE(streamProfile->BuildTxStreamConfig(0, primary));
    ASSERT_TRUE(streamProfile->BuildTxStreamConfig(1, secondary));
    EXPECT_EQ(primary.pcmChannels, 12U);
    EXPECT_EQ(primary.dbs, 13U);
    EXPECT_EQ(primary.midiSlots, 1U);
    EXPECT_EQ(primary.sourceChannelOffset, 0U);
    EXPECT_EQ(secondary.pcmChannels, 8U);
    EXPECT_EQ(secondary.dbs, 8U);
    EXPECT_EQ(secondary.midiSlots, 0U);
    EXPECT_EQ(secondary.sourceChannelOffset, 12U);
    AudioStreamConfig capture{};
    ASSERT_TRUE(streamProfile->BuildDefaultRxStreamConfig(capture));
    EXPECT_EQ(capture.pcmChannels, 10U);
    EXPECT_EQ(capture.dbs, 11U);
    EXPECT_EQ(capture.midiSlots, 1U);
    EXPECT_FALSE(streamProfile->BuildTxStreamConfig(2, secondary));
}

// The recorded dump is capture 10 PCM + MIDI, then 10 PCM alone. Both streams
// are the same width, so the PCM total is right either way -- what the base's
// repeat-stream-0 default got wrong is the DATA BLOCK SIZE, by putting a MIDI
// slot on a stream that does not carry one.
TEST(DiceProfileTests, Pro40CaptureStreamsDifferOnlyInTheirMidiSlot) {
    const auto* profile = FindDiceProfile(0x00130E, 0x000005U);
    ASSERT_NE(profile, nullptr);
    const auto* streamProfile = static_cast<const IAudioStreamProfile*>(profile);

    AudioStreamConfig first{}, second{};
    ASSERT_TRUE(streamProfile->BuildRxStreamConfig(0, first));
    ASSERT_TRUE(streamProfile->BuildRxStreamConfig(1, second));

    EXPECT_EQ(first.pcmChannels, 10U);
    EXPECT_EQ(first.midiSlots, 1U);
    EXPECT_EQ(first.dbs, 11U);
    EXPECT_EQ(first.sourceChannelOffset, 0U);

    EXPECT_EQ(second.pcmChannels, 10U);
    EXPECT_EQ(second.midiSlots, 0U);
    EXPECT_EQ(second.dbs, 10U);  // 11 would be stream 0's shape repeated.
    EXPECT_EQ(second.sourceChannelOffset, 10U);

    EXPECT_FALSE(streamProfile->BuildRxStreamConfig(2, second));

    // And the aggregate is the sum of those two, not a separately stated total:
    // the hand-written TxChannelCount() override that used to say 20 is gone,
    // so 12 + 8 has to produce it.
    EXPECT_EQ(profile->TxChannelCount(), 20U);
    EXPECT_EQ(profile->RxChannelCount(), 20U);
}

// The Pro 40 knows its geometry -- a register dump plus FFADO
// saffire_pro40.cpp:50-97 -- so it asserts it, and a device contradicting it is
// a real conflict rather than a seed being replaced.
TEST(DiceProfileTests, Pro40AssertsItsGeometryInBothDirections) {
    const auto* profile = FindDiceProfile(0x00130E, 0x000005U);
    ASSERT_NE(profile, nullptr);
    const auto* streamProfile = static_cast<const IAudioStreamProfile*>(profile);
    EXPECT_EQ(streamProfile->CaptureGeometryAuthority(),
              ASFW::Isoch::Audio::StreamGeometryAuthority::kAsserted);
    EXPECT_EQ(streamProfile->PlaybackGeometryAuthority(),
              ASFW::Isoch::Audio::StreamGeometryAuthority::kAsserted);
}

// Alesis: the two directions carry different authority, which is why it is a
// per-direction question. Playback is libffado's forced nb_rx = 1
// (dice_avdevice.cpp:1686-1700); capture never had a reference behind it.
TEST(DiceProfileTests, AlesisMultiMixAssertsPlaybackButSeedsCapture) {
    Profiles::AlesisMultiMixProfile profile;
    EXPECT_EQ(profile.PlaybackGeometryAuthority(),
              ASFW::Isoch::Audio::StreamGeometryAuthority::kAsserted);
    EXPECT_EQ(profile.CaptureGeometryAuthority(),
              ASFW::Isoch::Audio::StreamGeometryAuthority::kSeed);
    EXPECT_EQ(profile.TxStreamCount(), 1U);
}

TEST(DiceProfileTests, UniformPlaybackStreamsKeepDisjointChannelSlices) {
    Profiles::MidasVeniceProfile profile;
    AudioStreamConfig primary{}, secondary{};
    ASSERT_TRUE(profile.BuildTxStreamConfig(0, primary));
    ASSERT_TRUE(profile.BuildTxStreamConfig(1, secondary));
    EXPECT_EQ(primary.sourceChannelOffset, 0U);
    EXPECT_EQ(secondary.sourceChannelOffset, 16U);
}

TEST(DiceProfileTests, ResolvesGenericDiceProfileForUnknownDevices) {
    const auto* profile = FindDiceProfile(0x999999, 0x000001, 0x123456789ULL);

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

TEST(DiceProfileTests, WeissIntProfileKeepsDuplexWireShapeButHidesCaptureFromCoreAudio) {
    constexpr uint32_t kWeissVendorId = 0x001c6a;
    constexpr uint32_t kInt202ModelId = 0x000006;
    constexpr uint32_t kInt203ModelId = 0x00000a;

    for (const uint32_t modelId : {kInt202ModelId, kInt203ModelId}) {
        const auto* profile = FindDiceProfile(kWeissVendorId, modelId);
        ASSERT_NE(profile, nullptr);
        EXPECT_STREQ(profile->Name(), "Weiss INT (DICE)");
        EXPECT_EQ(profile->TxChannelCount(), 2U);
        EXPECT_EQ(profile->RxChannelCount(), 0U);
        EXPECT_EQ(profile->SupportedSampleRates(), (std::vector<uint32_t>{44100U, 48000U}));

        const auto* wireProfile = static_cast<const IAudioStreamProfile*>(profile);
        AudioStreamConfig capture{};
        ASSERT_TRUE(wireProfile->BuildDefaultRxStreamConfig(capture));
        EXPECT_EQ(capture.pcmChannels, 2U);
        EXPECT_EQ(capture.dbs, 2U);
    }
}

TEST(DiceProfileTests, ResolvesApogeeDuetProfileWithoutDICEName) {
    const auto* profile = FindAvcProfile(0x0003DB, 0x01DDDD, 0x0003DB0A0000D112ULL);

    ASSERT_NE(profile, nullptr);
    EXPECT_STREQ(profile->Name(), "Duet");
    EXPECT_EQ(profile->TxWireFormat(), ASFW::Encoding::AudioWireFormat::kAM824);
    EXPECT_EQ(profile->RxWireFormat(), ASFW::Encoding::AudioWireFormat::kAM824);
    EXPECT_EQ(profile->TxChannelCount(), 2u);
    EXPECT_EQ(profile->RxChannelCount(), 2u);
    EXPECT_EQ(profile->SupportedSampleRates(), (std::vector<uint32_t>{48000u}));
}

TEST(DiceProfileTests, ResolvesPhase88AsAvcWireProfileNotGenericDice) {
    const auto* profile = FindAvcProfile(0x000AAC, 0x000003, 0x000AAC0300B1D1F7ULL);

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
    const auto* profile = FindDiceProfile(kFocusriteVendorId, 0x000007, 0x123456789ULL);
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
    const auto* profile = FindDiceProfile(0x10c73f, 0x000001, 0x10c73f04004011dfULL);

    ASSERT_NE(profile, nullptr);
    // One row serves the whole F range, so identity alone cannot name a
    // variant -- the recorded F24 even reports its model string as "Venice
    // F32". Name() therefore names the range; NameForGeometry names the device.
    EXPECT_STREQ(profile->Name(), "Midas Venice F (DICE)");
    EXPECT_EQ(profile->TxWireFormat(), ASFW::Encoding::AudioWireFormat::kRawPcm24In32);
    EXPECT_EQ(profile->RxWireFormat(), ASFW::Encoding::AudioWireFormat::kAM824);

    // The seeded F32 geometry at 48 kHz: 2 streams/direction × 16ch. Both wire
    // configs are per-stream (DBS=16) with Tx/RxStreamCount()==2, so the seeded
    // aggregate is 32 per side.
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

    // Those counts are a seed in BOTH directions. If either were asserted, the
    // resolver would treat the recorded F24's 16 + 8 as a conflict and refuse
    // to publish it.
    EXPECT_EQ(diceProfile->CaptureGeometryAuthority(),
              ASFW::Isoch::Audio::StreamGeometryAuthority::kSeed);
    EXPECT_EQ(diceProfile->PlaybackGeometryAuthority(),
              ASFW::Isoch::Audio::StreamGeometryAuthority::kSeed);
}

// The Stage 4 exit criterion: one catalog row, three devices, told apart by
// what the device turned out to carry rather than by what it calls itself.
TEST(DiceProfileTests, MidasVeniceNamesTheVariantFromMeasuredCaptureChannels) {
    const auto* profile = FindDiceProfile(0x10c73f, 0x000001, 0x10c73f04004011dfULL);
    ASSERT_NE(profile, nullptr);

    // 16 + 8 is the recorded F24 (documentation/fixtures/DICE/midasF24.txt),
    // whose own model string says F32 -- so this is exactly the case the
    // identity path gets wrong.
    EXPECT_STREQ(profile->NameForGeometry(24, 24), "Midas Venice F24");
    EXPECT_STREQ(profile->NameForGeometry(32, 32), "Midas Venice F32");
    EXPECT_STREQ(profile->NameForGeometry(16, 16), "Midas Venice F16");

    // An unrecognised width stays at the range name rather than being rounded
    // to the nearest variant.
    EXPECT_STREQ(profile->NameForGeometry(20, 20), "Midas Venice F (DICE)");
    EXPECT_STREQ(profile->NameForGeometry(0, 0), "Midas Venice F (DICE)");
}

// Everything else is one model per row, so naming must not move.
TEST(DiceProfileTests, SingleModelProfilesIgnoreMeasuredGeometryWhenNaming) {
    const auto* saffire = FindDiceProfile(0x00130e, 0x000005, 0x00130E0401405B54ULL);
    ASSERT_NE(saffire, nullptr);
    EXPECT_STREQ(saffire->NameForGeometry(20, 20), saffire->Name());
    EXPECT_STREQ(saffire->NameForGeometry(2, 2), saffire->Name());
}

TEST(DiceProfileTests, MidasVeniceSafetyOffsetsAndLatencies) {
    const auto* profile = FindDiceProfile(0x10c73f, 0x000001, 0x10c73f04004011dfULL);
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
    const auto* profile = FindDiceProfile(0x10c73f, 0x999999);
    // Should fall through to generic profile, not Venice.
    if (profile != nullptr) {
        EXPECT_STRNE(profile->Name(), "Midas Venice F (DICE)");
    }
}

TEST(DiceProfileTests, ResolvesPreSonusStudioLive1602ProfileByVendorAndModel) {
    // Identity captured live from the hardware (2026-07-08): GUID 0x000A920404FE2011,
    // vendor 0x000A92, model 0x000013.
    const auto* profile = FindDiceProfile(0x000A92, 0x000013, 0x000A920404FE2011ULL);

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
    const auto* profile = FindDiceProfile(0x000A92, 0x000013, 0x000A920404FE2011ULL);
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
        const auto* profile = FindDiceProfile(0x000A92, modelId);
        if (profile != nullptr) {
            EXPECT_STRNE(profile->Name(), "PreSonus StudioLive 16.0.2 (DICE)");
        }
    }
}

TEST(DiceProfileTests, ResolvesAlesisMultiMixProfileByVendorAndModel) {
    // Alesis MultiMix 8/12/16 FireWire all share vendor 0x000595 / model 0x000000
    // (libffado configuration:622-628; snd-firewire-ctl-services model.rs:140).
    const auto* profile = FindDiceProfile(0x000595, 0x000000, 0x000595040000ABCDULL);

    ASSERT_NE(profile, nullptr);
    EXPECT_STREQ(profile->Name(), "Alesis MultiMix FireWire (DICE)");
    EXPECT_EQ(profile->TxWireFormat(), ASFW::Encoding::AudioWireFormat::kRawPcm24In32);
    EXPECT_EQ(profile->RxWireFormat(), ASFW::Encoding::AudioWireFormat::kAM824);

    // Seed geometry only — the real per-variant counts are read back from the
    // device's TX/RX stream-format registers and overwrite these via
    // ApplyDiceRuntimeCapsToDeviceConfig. Pinned here so a change is deliberate.
    EXPECT_EQ(profile->TxChannelCount(), 2);
    EXPECT_EQ(profile->RxChannelCount(), 16);
    EXPECT_EQ(profile->TxMidiSlots(), 0);
    EXPECT_EQ(profile->RxMidiSlots(), 0);
    EXPECT_EQ(profile->TxDbs(), 2);
    EXPECT_EQ(profile->RxDbs(), 16);

    const auto* diceProfile = static_cast<const IDiceDeviceProfile*>(profile);
    // ONE stream per direction. The device over-reports its host-to-device
    // stream count (libffado dice_avdevice.cpp:1684-1693 forces nb_rx = 1 for
    // Alesis models 0x000000/0x000001), so these must stay at 1.
    EXPECT_EQ(diceProfile->TxStreamCount(), 1u);
    EXPECT_EQ(diceProfile->RxStreamCount(), 1u);
    EXPECT_TRUE(diceProfile->Quirks().tx.preserveFdfInNoDataPackets);
    EXPECT_EQ(diceProfile->Quirks().tx.hostToDevicePcmEncoding,
              ASFW::Encoding::AudioWireFormat::kRawPcm24In32);

    // The MultiMix has no MIDI I/O: the host-to-device block is pure PCM
    // (DBS == pcmChannels), so there is no non-audio slot and the 0x80000000
    // MIDI-slot fill the other TCAT profiles carry stays off. If a non-audio
    // slot is ever confirmed, midiSlots and this flag must be raised together.
    EXPECT_FALSE(diceProfile->Quirks().tx.initializeNonAudioSlots);
    EXPECT_EQ(profile->TxDbs(), profile->TxChannelCount());
}

TEST(DiceProfileTests, AlesisMultiMixSafetyOffsetsAndLatencies) {
    const auto* profile = FindDiceProfile(0x000595, 0x000000, 0x000595040000ABCDULL);
    ASSERT_NE(profile, nullptr);

    // Focusrite Saffire baseline ladder: Tx = 6 * 8 = 48, Rx = 16 * 8 = 128.
    EXPECT_EQ(profile->TxSafetyOffsetFrames(48000.0), 48);
    EXPECT_EQ(profile->RxSafetyOffsetFrames(48000.0), 128);
    EXPECT_EQ(profile->TxReportedLatencyFrames(48000.0), 29);
    EXPECT_EQ(profile->RxReportedLatencyFrames(48000.0), 29);

    // 96 kHz: Tx = 8 * 16 = 128, Rx = 18 * 16 = 288.
    EXPECT_EQ(profile->TxSafetyOffsetFrames(96000.0), 128);
    EXPECT_EQ(profile->RxSafetyOffsetFrames(96000.0), 288);
    EXPECT_EQ(profile->TxReportedLatencyFrames(96000.0), 59);
    EXPECT_EQ(profile->RxReportedLatencyFrames(96000.0), 59);
}

TEST(DiceProfileTests, AlesisVendorWithWrongModelDoesNotMatchMultiMixProfile) {
    // The Alesis OUI also covers the iO14/iO26 (0x000001) and MasterControl
    // (0x000002), which have different stream geometry and their own format
    // detection in Linux (dice-alesis.c). Neither may inherit MultiMix geometry.
    for (const uint32_t modelId : {0x000001u, 0x000002u}) {
        const auto* profile = FindDiceProfile(0x000595, modelId);
        if (profile != nullptr) {
            EXPECT_STRNE(profile->Name(), "Alesis MultiMix FireWire (DICE)");
        }
    }
}

// The assertion that keeps the two halves in step. Every builder the catalog
// calls Supported must resolve to a profile object here, or the device
// publishes a nub and then gets the generic DICE geometry, which rejects every
// packet it receives. That is issue #115's failure shape moved one layer down.
TEST(DiceProfileTests, EverySupportedBuilderResolvesToAProfile) {
    using ASFW::DeviceProfiles::Audio::AudioDeviceCatalog;
    using ASFW::DeviceProfiles::Audio::ProfileBuilderId;
    using ASFW::DeviceProfiles::Audio::SupportDisposition;

    for (const auto& definition : AudioDeviceCatalog::Definitions()) {
        if (definition.support != SupportDisposition::Supported) {
            continue;
        }
        const auto builderId = static_cast<uint32_t>(definition.profileBuilder);
        const auto* profile = AudioProfileRegistry::ProfileForBuilderId(builderId);
        EXPECT_NE(profile, nullptr)
            << "definition " << static_cast<uint32_t>(definition.id)
            << " is Supported with builder " << builderId
            << " but no profile object resolves it";
        if (profile != nullptr) {
            EXPECT_STRNE(profile->Name(), "Generic DICE")
                << "definition " << static_cast<uint32_t>(definition.id)
                << " resolved the generic fallback";
        }
    }
}

// A builder that is not a DICE one must not come back through the DICE-typed
// accessor: the caller would use the richer interface on an object that does
// not implement it.
TEST(DiceProfileTests, TheDiceAccessorReturnsOnlyDiceProfiles) {
    using ASFW::DeviceProfiles::Audio::ProfileBuilderId;
    for (const auto builder : {ProfileBuilderId::ApogeeDuet,
                               ProfileBuilderId::TerraTecPhase88,
                               ProfileBuilderId::MackieOnyxIOxfw,
                               ProfileBuilderId::MackieOnyx400F,
                               ProfileBuilderId::Motu828mk2,
                               ProfileBuilderId::MotuUltralite,
                               ProfileBuilderId::None}) {
        EXPECT_EQ(AudioProfileRegistry::DiceProfileForBuilderId(
                      static_cast<uint32_t>(builder)),
                  nullptr)
            << "builder " << static_cast<uint32_t>(builder);
    }
    EXPECT_NE(AudioProfileRegistry::DiceProfileForBuilderId(
                  static_cast<uint32_t>(ProfileBuilderId::FocusriteSPro24Dsp)),
              nullptr);
}

// An out-of-range builder id -- a nub from an older driver, or a corrupt
// property -- must be refused rather than indexed.
TEST(DiceProfileTests, AnOutOfRangeBuilderIdResolvesToNothing) {
    EXPECT_EQ(AudioProfileRegistry::ProfileForBuilderId(0xFFFFFFFFU), nullptr);
    EXPECT_EQ(AudioProfileRegistry::ProfileForBuilderId(0U), nullptr);
    EXPECT_EQ(AudioProfileRegistry::DiceProfileForBuilderId(0xFFFFFFFFU), nullptr);
}

TEST(DiceProfileTests, GenericDiceDefaultOffsetsAndLatencies) {
    const auto* profile = FindDiceProfile(0x999999, 0x000001, 0x123456789ULL);
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

    const auto* profile = FindAvcProfile(0x000AAC, 0x000003, kPhase88Guid);
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

TEST(DiceProfileTests, ResolvesMackieOnyx820iAsymmetricProfileNotGenericDice) {
    const uint32_t kMackieVendorId = 0x000FF2;
    const uint32_t kOnyxIOxfwModelId = 0x081216;
    const auto* profile =
        FindAvcProfile(kMackieVendorId, kOnyxIOxfwModelId);

    ASSERT_NE(profile, nullptr);
    // Falling through to "Generic DICE" would hand the device a symmetric 2x2
    // geometry that the RX path rejects on every 8-channel packet.
    EXPECT_STREQ(profile->Name(), "Onyx-i (Oxford)");

    // Asymmetric duplex captured from a real 820i: 8-in (device->host), 2-out.
    EXPECT_EQ(profile->TxChannelCount(), 2);
    EXPECT_EQ(profile->RxChannelCount(), 8);
    EXPECT_EQ(profile->TxMidiSlots(), 0);
    EXPECT_EQ(profile->RxMidiSlots(), 0);

    // Stream-config builders live on the DICE profile interface; exercise the
    // concrete class for the wire geometry.
    const AVC::Profiles::MackieOnyx820iProfile concrete{};

    DiceStreamConfig tx{};
    ASSERT_TRUE(concrete.BuildDefaultTxStreamConfig(tx));
    EXPECT_EQ(tx.pcmChannels, 2u);
    EXPECT_EQ(tx.dbs, 2u);
    EXPECT_EQ(tx.sampleRate, 44100u);
    EXPECT_EQ(tx.streamMode, ASFW::Encoding::StreamMode::kBlocking);

    DiceStreamConfig rx{};
    ASSERT_TRUE(concrete.BuildDefaultRxStreamConfig(rx));
    EXPECT_EQ(rx.pcmChannels, 8u);
    EXPECT_EQ(rx.dbs, 8u);
    EXPECT_EQ(rx.sampleRate, 44100u);
    EXPECT_EQ(rx.streamMode, ASFW::Encoding::StreamMode::kBlocking);

    // 44.1 kHz only until the ADK reconfig path supports AV/C rate changes —
    // offering 48 kHz re-arms the stale-pendingClock regression (see
    // MackieOnyxProtocol::SupportedRates).
    const auto rates = concrete.SupportedSampleRates();
    ASSERT_EQ(rates.size(), 1u);
    EXPECT_EQ(rates.front(), 44100u);
}

} // namespace
