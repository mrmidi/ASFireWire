// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DeviceStreamTraitsAgreementTests.cpp
//
// The catalog's DeviceStreamTraits replace two hand-written matchers: the
// vendor/model table in DeviceStreamModeQuirks and the nine Is*() predicates in
// DuplexStreamProfile.
//
// These ran first against the live old table, in fa6c48f4, so the answers below
// were an observed oracle before they became written-down expectations. The
// table is deleted now; the expectations remain.
//
// Every difference from it is deliberate and named below. There are two, and
// neither is reachable.

#include "DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "Discovery/DiscoveryTypes.hpp"

#include <gtest/gtest.h>

#include <ios>
#include <optional>

namespace {

using namespace ASFW::DeviceProfiles::Audio;
namespace Discovery = ASFW::Discovery;

constexpr uint32_t kDiceInterfaceVersion = 0x000001;
constexpr uint32_t kTa1394AvcSpecifier = 0x00A02D;
constexpr uint32_t kTa1394AvcVersion = 0x010001;
// Echo Fireworks units publish the TA 1394 specifier with version 0x010000,
// not 0x010001 (Linux firewire/fireworks/fireworks.c).
constexpr uint32_t kFireworksVersion = 0x010000;

[[nodiscard]] Discovery::DeviceIdentityEvidence
Identity(uint32_t vendorId, uint32_t modelId, uint32_t unitSpecifier,
         uint32_t unitVersion) {
    Discovery::DeviceIdentityEvidence identity{};
    identity.observedGuid = (static_cast<uint64_t>(vendorId) << 40U) | 0x04'0000'0000ULL;
    identity.nodeVendorOui = vendorId;
    identity.rootVendorId = vendorId;
    identity.rootModelId = modelId;
    Discovery::UnitIdentityEvidence unit{};
    unit.unitDirectoryOffset = 5;
    unit.specifierId = unitSpecifier;
    unit.version = unitVersion;
    identity.units.push_back(unit);
    return identity;
}

// Only a unit whose specifier is 0x00A02D reaches AVCDiscovery
// (AVCDiscovery::IsAVCUnit), and that is the sole consumer of the forced stream
// mode. A DICE unit publishes the vendor OUI as its specifier, so it never gets
// there -- which is why the Focusrite and Midas rows in the old quirk table
// were dead code, and why the catalog stating Blocking for every DICE part
// (correct per snd-dice, which is unconditionally CIP_BLOCKING) changes nothing
// observable.
[[nodiscard]] Discovery::DeviceIdentityEvidence AvcIdentity(uint32_t vendorId,
                                                            uint32_t modelId) {
    return Identity(vendorId, modelId, kTa1394AvcSpecifier, kTa1394AvcVersion);
}

[[nodiscard]] Discovery::DeviceIdentityEvidence DiceIdentity(uint32_t vendorId,
                                                             uint32_t modelId) {
    return Identity(vendorId, modelId, vendorId, kDiceInterfaceVersion);
}

// ---------------------------------------------------------------------------
// Forced stream mode
// ---------------------------------------------------------------------------

// The equivalence that matters: every identity that can actually reach the
// consumer must get the same answer it does today.
TEST(DeviceStreamTraitsAgreement, ForcedModeMatchesTheOldTableForEveryAvcDevice) {
    struct Case {
        uint32_t vendorId;
        uint32_t modelId;
        uint32_t unitVersion;
        ForcedStreamMode expected;
    };
    const Case avcDevices[] = {
        {kApogeeVendorId, kApogeeDuetModelId, kTa1394AvcVersion,
         ForcedStreamMode::Blocking},
        {kTerraTecVendorId, kPhase88RackFwModelId, kTa1394AvcVersion,
         ForcedStreamMode::Blocking},
        {kMackieVendorId, kOnyxIOxfwModelId, kTa1394AvcVersion,
         ForcedStreamMode::Blocking},
        {kMackieVendorId, kOnyx1640iOxfwModelId, kTa1394AvcVersion,
         ForcedStreamMode::Blocking},
        {kMackieVendorId, kOnyx400FModelId, kFireworksVersion,
         ForcedStreamMode::Blocking},
        {kMackieVendorId, kOnyx1200FModelId, kFireworksVersion,
         ForcedStreamMode::Blocking},
        // M-Audio was never in the old BeBoB list, so it must stay unforced.
        {kMAudioVendorId, kMAudioFireWire1814ModelId, kTa1394AvcVersion,
         ForcedStreamMode::Unspecified},
        {kMAudioVendorId, kMAudioProjectMixModelId, kTa1394AvcVersion,
         ForcedStreamMode::Unspecified},
        // Not a device we know at all.
        {0x00AABB, 0x000042, kTa1394AvcVersion, ForcedStreamMode::Unspecified},
    };
    for (const auto& [vendorId, modelId, unitVersion, expected] : avcDevices) {
        EXPECT_EQ(AudioDeviceCatalog::StreamTraitsFor(
                      Identity(vendorId, modelId, kTa1394AvcSpecifier, unitVersion))
                      .forcedStreamMode,
                  expected)
            << "vendor 0x" << std::hex << vendorId << " model 0x" << modelId;
    }
}

// The one deliberate difference, stated rather than hidden. The old table gave
// Blocking to the Focusrite Saffires and the Midas Venice but not to Weiss,
// Alesis or PreSonus -- an inconsistency that never showed because no DICE unit
// reaches the consumer. The catalog states the same thing for all of them,
// which is what snd-dice does (dice-stream.c:508).
TEST(DeviceStreamTraitsAgreement, EveryDicePartStatesBlockingEvenWhereTheOldTableDidNot) {
    const std::pair<uint32_t, uint32_t> diceParts[] = {
        {kFocusriteVendorId, kSPro24DspModelId},   // old table: Blocking
        {kMidasVendorId, kMidasVeniceModelId},     // old table: Blocking
        {kWeissVendorId, kWeissInt202ModelId},     // old table: nothing
        {kAlesisVendorId, kAlesisMultiMixModelId}, // old table: nothing
        {kPreSonusVendorId, kStudioLive2442ModelId},  // old table: nothing
        {kFocusriteVendorId, kSPro40Tcd3070ModelId},  // old table: nothing
    };
    for (const auto& [vendorId, modelId] : diceParts) {
        EXPECT_EQ(AudioDeviceCatalog::StreamTraitsFor(DiceIdentity(vendorId, modelId))
                      .forcedStreamMode,
                  ForcedStreamMode::Blocking)
            << "vendor 0x" << std::hex << vendorId << " model 0x" << modelId;
    }
}

// The second deliberate difference. The old table forced blocking for the
// Mackie OUI *vendor-wide*; the catalog states it per model. The gap is an
// unlisted Mackie, and it is not reachable: the Oxford production run is fully
// covered here (the shared 0x081216 and the 1640i's 0x001640 are the only ids
// libffado records for it), and the ids still missing are DICE-run ones, whose
// units publish the vendor OUI as their specifier and so never reach
// AVCDiscovery -- the sole consumer of this value.
TEST(DeviceStreamTraitsAgreement, AnUnlistedMackieNoLongerGetsVendorWideBlocking) {
    // The old table returned Blocking here on the vendor alone.
    constexpr uint32_t kUnlistedMackieModel = 0x00AAAA;
    EXPECT_EQ(AudioDeviceCatalog::StreamTraitsFor(
                  AvcIdentity(kMackieVendorId, kUnlistedMackieModel)).forcedStreamMode,
              ForcedStreamMode::Unspecified);

    // And the id that motivates the gap is still a sentinel, so no real device
    // is behind it yet.
    EXPECT_EQ(kOnyx820iModelId, kMackieModelIdPendingCapture);
}

// An unknown device must stay unspecified: forcing a cadence on a device we
// have never seen is a guess, and the probe's answer is better than a guess.
TEST(DeviceStreamTraitsAgreement, AnUnknownDeviceGetsNoForcedMode) {
    EXPECT_EQ(AudioDeviceCatalog::StreamTraitsFor(AvcIdentity(0x00AABB, 0x000042))
                  .forcedStreamMode,
              ForcedStreamMode::Unspecified);
}

// ---------------------------------------------------------------------------
// The Is*() predicates in DuplexStreamProfile
// ---------------------------------------------------------------------------

// IsCmpDriven decided whether the iso channel is fixed or IRM-chosen, and it
// was the OR of four predicates. The catalog says the same thing as a start
// shape, because the two always travelled together: every CMP-driven family
// used the receive-then-transmit choreography, and nothing else did.
TEST(DeviceStreamTraitsAgreement, CmpDrivenFamiliesCarryTheCmpStartShape) {
    struct Case {
        uint32_t vendorId;
        uint32_t modelId;
        uint32_t unitVersion;
    };
    const Case cmpDriven[] = {
        {kTerraTecVendorId, kPhase88RackFwModelId, kTa1394AvcVersion},  // IsBeBoB
        {kMackieVendorId, kOnyxIOxfwModelId, kTa1394AvcVersion},     // IsMackieOnyxI
        {kMackieVendorId, kOnyx400FModelId, kFireworksVersion},   // IsMackieOnyx400F
    };
    for (const auto& [vendorId, modelId, unitVersion] : cmpDriven) {
        EXPECT_EQ(AudioDeviceCatalog::StreamTraitsFor(
                      Identity(vendorId, modelId, kTa1394AvcSpecifier, unitVersion))
                      .startShape,
                  StreamStartShape::CmpReceiveThenTransmit)
            << "vendor 0x" << std::hex << vendorId << " model 0x" << modelId;
    }
    // IsApogeeDuet is CMP-driven too but keeps its own interleaved ordering.
    EXPECT_EQ(AudioDeviceCatalog::StreamTraitsFor(
                  AvcIdentity(kApogeeVendorId, kApogeeDuetModelId)).startShape,
              StreamStartShape::ApogeeInterleaved);
}

// The PHASE 88 is the only supported BeBoB device with start shape today.
TEST(DeviceStreamTraitsAgreement, TheOnlyBeBoBDeviceIsStillThePhase88) {
    EXPECT_EQ(AudioDeviceCatalog::StreamTraitsFor(
                  AvcIdentity(kTerraTecVendorId, kPhase88RackFwModelId)).startShape,
              StreamStartShape::CmpReceiveThenTransmit);
    // The M-Audio personas are BeBoB by family but are not in the old list, so
    // they must not pick up a start shape either -- nothing starts them.
    EXPECT_EQ(AudioDeviceCatalog::StreamTraitsFor(
                  AvcIdentity(kMAudioVendorId, kMAudioFireWire1814ModelId)).startShape,
              StreamStartShape::Default);
}

TEST(DeviceStreamTraitsAgreement, WeissIsTheOnlyTransmitFirstDevice) {
    for (const uint32_t modelId : {kWeissInt202ModelId, kWeissInt203ModelId}) {
        EXPECT_EQ(AudioDeviceCatalog::StreamTraitsFor(
                      DiceIdentity(kWeissVendorId, modelId)).startShape,
                  StreamStartShape::TransmitFirst);
    }
    // A Weiss part we do not stream keeps the default: the transmit-first order
    // is a property of the INT interfaces, not of the vendor.
    EXPECT_EQ(AudioDeviceCatalog::StreamTraitsFor(
                  DiceIdentity(kWeissVendorId, kWeissDac202ModelId)).startShape,
              StreamStartShape::Default);
}

TEST(DeviceStreamTraitsAgreement, OnlyTheTwoLoudRunsDistrustTheCaptureStride) {
    EXPECT_TRUE(AudioDeviceCatalog::StreamTraitsFor(
                    AvcIdentity(kMackieVendorId, kOnyxIOxfwModelId))
                    .captureTrustConfiguredStride);
    EXPECT_TRUE(AudioDeviceCatalog::StreamTraitsFor(
                    Identity(kMackieVendorId, kOnyx400FModelId,
                             kTa1394AvcSpecifier, kFireworksVersion))
                    .captureTrustConfiguredStride);
    EXPECT_FALSE(AudioDeviceCatalog::StreamTraitsFor(
                     AvcIdentity(kApogeeVendorId, kApogeeDuetModelId))
                     .captureTrustConfiguredStride);
    EXPECT_FALSE(AudioDeviceCatalog::StreamTraitsFor(
                     DiceIdentity(kFocusriteVendorId, kSPro24DspModelId))
                     .captureTrustConfiguredStride);
}

// HasAlesisCaptureStreamQuirk covered models 0x000000 (MultiMix) and 0x000001
// (the iO). Only the MultiMix has a catalog row, and only the MultiMix is ever
// streamed -- the iO gets no builder, so no protocol, so no nub, and the clamp
// could never have run for it. The evidence is kept where it belongs rather
// than in a predicate no code path reaches.
TEST(DeviceStreamTraitsAgreement, OnlyTheAlesisMultiMixClampsItsCaptureStreams) {
    EXPECT_TRUE(AudioDeviceCatalog::StreamTraitsFor(
                    DiceIdentity(kAlesisVendorId, kAlesisMultiMixModelId))
                    .clampCaptureStreamsToOne);
    EXPECT_FALSE(AudioDeviceCatalog::StreamTraitsFor(
                     DiceIdentity(kMidasVendorId, kMidasVeniceModelId))
                     .clampCaptureStreamsToOne);
}

// IsSPro24Dsp gated a runtime-conditional wire format: raw 24-in-32 only when
// the geometry is 8 PCM in 9 slots, AM824 otherwise. The condition stays in the
// profile builder; only the permission to apply it is catalog data.
TEST(DeviceStreamTraitsAgreement, OnlyTheSaffirePro24DspSwitchesWireFormat) {
    EXPECT_TRUE(AudioDeviceCatalog::StreamTraitsFor(
                    DiceIdentity(kFocusriteVendorId, kSPro24DspModelId))
                    .rawPcm24In32WhenEightInNineSlots);
    for (const uint32_t modelId : {kSPro14ModelId, kSPro24ModelId, kSPro40ModelId}) {
        EXPECT_FALSE(AudioDeviceCatalog::StreamTraitsFor(
                         DiceIdentity(kFocusriteVendorId, modelId))
                         .rawPcm24In32WhenEightInNineSlots)
            << "model 0x" << std::hex << modelId;
    }
}

// MOTU states no traits at all: its framing comes from the family, its chunk
// counts from its own registers. A trait here would be a second source.
TEST(DeviceStreamTraitsAgreement, MotuStatesNoStreamTraits) {
    Discovery::DeviceIdentityEvidence motu{};
    motu.observedGuid = (static_cast<uint64_t>(kMotuVendorId) << 40U);
    motu.nodeVendorOui = kMotuVendorId;
    motu.rootVendorId = kMotuVendorId;
    motu.rootModelId = 0U;
    Discovery::UnitIdentityEvidence unit{};
    unit.unitDirectoryOffset = 5;
    unit.specifierId = kMotuVendorId;
    unit.version = kMotu828mk2SwVersion;
    motu.units.push_back(unit);

    const auto traits = AudioDeviceCatalog::StreamTraitsFor(motu);
    EXPECT_EQ(traits.forcedStreamMode, ForcedStreamMode::Unspecified);
    EXPECT_EQ(traits.startShape, StreamStartShape::Default);
    EXPECT_FALSE(traits.clampCaptureStreamsToOne);
    EXPECT_FALSE(traits.captureTrustConfiguredStride);
    EXPECT_FALSE(traits.rawPcm24In32WhenEightInNineSlots);
}

} // namespace
