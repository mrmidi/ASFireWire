// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceIdentityTests.cpp - The generic DICE rule and the catalog fallback it
// drives. Positive cases use GUIDs recorded from real units
// (documentation/fixtures/DICE/*.txt).

#include "Audio/Protocols/DICE/Core/DICETypes.hpp"
#include "Audio/Protocols/DeviceProtocolChoice.hpp"
#include "DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "DeviceProfiles/Audio/DiceIdentity.hpp"
#include "Discovery/DiscoveryTypes.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>

namespace {

using namespace ASFW::DeviceProfiles::Audio;
namespace Discovery = ASFW::Discovery;
using ASFW::Audio::DICE::GeneralSections;

struct Recorded {
    const char* what;
    uint64_t guid;
    uint32_t vendorId;
    uint32_t modelId;
};

// GUID, vendor and unit model as the units report them.
constexpr std::array kRecorded{
    Recorded{"Saffire Pro 24 DSP", 0x00130E0402004713ULL, kFocusriteVendorId, kSPro24DspModelId},
    Recorded{"Venice F32", 0x10C73F04004011DFULL, kMidasVendorId, kMidasVeniceModelId},
    Recorded{"StudioLive 24.4.2", 0x000A9204049204CBULL, kPreSonusVendorId,
             kStudioLive2442ModelId},
    Recorded{"FireStudio Project", 0x000A920402D07FACULL, kPreSonusVendorId,
             kFireStudioProjectModelId},
};

TEST(DiceIdentity, EveryRecordedUnitPassesTheRule) {
    for (const auto& unit : kRecorded) {
        EXPECT_TRUE(IsDiceIdentity(unit.guid, unit.vendorId, kDiceInterfaceVersion, unit.modelId))
            << unit.what;
    }
}

TEST(DiceIdentity, EachFieldOfTheRuleIsChecked) {
    const auto& pro24 = kRecorded[0];
    // Wrong interface version: a TA 1394 AV/C unit.
    EXPECT_FALSE(IsDiceIdentity(pro24.guid, pro24.vendorId, 0x010001, pro24.modelId));
    // Specifier is not the GUID's OUI.
    EXPECT_FALSE(IsDiceIdentity(pro24.guid, kPreSonusVendorId, kDiceInterfaceVersion,
                                pro24.modelId));
    // Product field disagrees with the unit's model.
    EXPECT_FALSE(IsDiceIdentity(pro24.guid, pro24.vendorId, kDiceInterfaceVersion,
                                pro24.modelId + 1));
    // Category byte is not 0x04.
    EXPECT_FALSE(IsDiceIdentity(pro24.guid ^ (uint64_t{0x04 ^ 0x10} << 32U), pro24.vendorId,
                                kDiceInterfaceVersion, pro24.modelId));
    // Any field missing from the unit directory.
    EXPECT_FALSE(IsDiceIdentity(pro24.guid, std::nullopt, kDiceInterfaceVersion, pro24.modelId));
    EXPECT_FALSE(IsDiceIdentity(pro24.guid, pro24.vendorId, std::nullopt, pro24.modelId));
    EXPECT_FALSE(IsDiceIdentity(pro24.guid, pro24.vendorId, kDiceInterfaceVersion, std::nullopt));
}

TEST(DiceIdentity, WeissLoudAndHarmanUseTheirOwnCategory) {
    const auto guidFor = [](uint32_t oui, uint32_t category, uint32_t model) {
        return (uint64_t{oui} << 40U) | (uint64_t{category} << 32U) | (uint64_t{model} << 22U);
    };
    EXPECT_TRUE(IsDiceIdentity(guidFor(kWeissDiceOui, 0x00, 7), kWeissDiceOui,
                               kDiceInterfaceVersion, 7));
    EXPECT_TRUE(IsDiceIdentity(guidFor(kLoudDiceOui, 0x10, 7), kLoudDiceOui,
                               kDiceInterfaceVersion, 7));
    EXPECT_TRUE(IsDiceIdentity(guidFor(kHarmanDiceOui, 0x20, 1), kHarmanDiceOui,
                               kDiceInterfaceVersion, 1));
    // The Oxford-run Onyx 820i carries 0x04 under the Loud OUI
    // (AudioDeviceIds.hpp, live capture 2026-08-17): not a DICE.
    EXPECT_FALSE(IsDiceIdentity(guidFor(kLoudDiceOui, 0x04, 7), kLoudDiceOui,
                                kDiceInterfaceVersion, 7));
}

// An unlisted vendor whose unit follows the DICE layout.
constexpr uint32_t kUnlistedOui = 0x001ee8;
constexpr uint32_t kUnlistedModel = 0x000005;
constexpr uint64_t kUnlistedGuid =
    (uint64_t{kUnlistedOui} << 40U) | (uint64_t{0x04} << 32U) | (uint64_t{kUnlistedModel} << 22U) |
    0x1234U;

[[nodiscard]] Discovery::DeviceRecord UnlistedDice(uint64_t guid, std::optional<uint32_t> model) {
    Discovery::DeviceRecord device{};
    device.instanceId = Discovery::DeviceInstanceId{3};
    device.identity.observedGuid = guid;
    device.identity.nodeVendorOui = kUnlistedOui;
    device.identity.rootVendorId = kUnlistedOui;
    device.identity.rootModelId = kUnlistedModel;
    device.identity.rootVendorName = "Mytek";
    device.identity.rootModelName = "Stereo192-DSD";
    Discovery::UnitIdentityEvidence unit{};
    unit.unitDirectoryOffset = 5;
    unit.specifierId = kUnlistedOui;
    unit.version = kDiceInterfaceVersion;
    unit.modelId = model;
    device.identity.units.push_back(unit);
    return device;
}

TEST(DiceIdentity, AnUnlistedDiceUnitGetsTheGenericDicePath) {
    const auto plan = AudioDeviceCatalog::Resolve(UnlistedDice(kUnlistedGuid, kUnlistedModel));
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->support, SupportDisposition::GenericFallback);
    EXPECT_EQ(plan->family, AudioFamilyProviderId::DICE);
    EXPECT_EQ(plan->probePolicy, ProbePolicyId::DiceTcat);
    EXPECT_EQ(plan->profileBuilder, ProfileBuilderId::GenericDice);
    EXPECT_EQ(plan->protocolImplementation, ProtocolImplementationId::DiceTcat);
    EXPECT_EQ(plan->streamTraits.wire.forcedStreamMode, ForcedStreamMode::Blocking);
    EXPECT_EQ(plan->vendorName, "Mytek");
    EXPECT_EQ(plan->modelName, "Stereo192-DSD");
    EXPECT_EQ(ASFW::Audio::ChooseAudioBackend(*plan), ASFW::Audio::AudioBackendKind::Dice);
    const auto choice = ASFW::Audio::ChooseDeviceProtocol(*plan);
    ASSERT_TRUE(choice.has_value());
    EXPECT_EQ(choice->builder, ProfileBuilderId::GenericDice);
}

TEST(DiceIdentity, AVersionOneUnitWithoutTheDiceGuidGetsNothing) {
    // Same unit directory, but the GUID's product field says another model.
    const auto wrongProduct = UnlistedDice(kUnlistedGuid, kUnlistedModel + 1);
    EXPECT_EQ(AudioDeviceCatalog::Resolve(wrongProduct).error(), CatalogResolutionError::NoMatch);
    const auto noModel = UnlistedDice(kUnlistedGuid, std::nullopt);
    EXPECT_EQ(AudioDeviceCatalog::Resolve(noModel).error(), CatalogResolutionError::NoMatch);
}

TEST(DiceIdentity, TheFallbackIsOffWhenGenericFallbackIsOff) {
    const auto device = UnlistedDice(kUnlistedGuid, kUnlistedModel);
    const auto plan = AudioDeviceCatalog::ResolveWithDefinitions(
        device, device.identity.units[0], AudioDeviceCatalog::Definitions(),
        AudioDeviceCatalog::SafetyRules(), false);
    EXPECT_EQ(plan.error(), CatalogResolutionError::NoMatch);
}

// ---- the section table ----

// Ten big-endian quadlets: GLOBAL, TX, RX, EXT_SYNC, reserved as offset/size.
[[nodiscard]] std::array<uint8_t, GeneralSections::kWireSize> Table(
    std::array<uint32_t, 10> quadlets) {
    std::array<uint8_t, GeneralSections::kWireSize> bytes{};
    for (size_t i = 0; i < quadlets.size(); ++i) {
        bytes[i * 4 + 0] = static_cast<uint8_t>(quadlets[i] >> 24U);
        bytes[i * 4 + 1] = static_cast<uint8_t>(quadlets[i] >> 16U);
        bytes[i * 4 + 2] = static_cast<uint8_t>(quadlets[i] >> 8U);
        bytes[i * 4 + 3] = static_cast<uint8_t>(quadlets[i]);
    }
    return bytes;
}

// The Saffire Pro 24 DSP's table, in quadlets
// (documentation/fixtures/DICE/spro24dsp.txt:12-16).
constexpr std::array<uint32_t, 10> kRealTable{0x0a, 0x5f, 0x69, 0x8e, 0xf7, 0x11a,
                                              0x211, 0x04, 0x00, 0x00};

TEST(DiceSectionTable, ARealTableIsPlausible) {
    EXPECT_TRUE(GeneralSections::IsPlausibleWire(Table(kRealTable).data()));
}

TEST(DiceSectionTable, EachBoundIsChecked) {
    auto shortGlobal = kRealTable;
    shortGlobal[1] = 0x60 / 4 - 1;
    EXPECT_FALSE(GeneralSections::IsPlausibleWire(Table(shortGlobal).data()));

    auto txInsideTable = kRealTable;
    txInsideTable[2] = 9;
    EXPECT_FALSE(GeneralSections::IsPlausibleWire(Table(txInsideTable).data()));

    auto shortRx = kRealTable;
    shortRx[5] = 0x18 / 4 - 1;
    EXPECT_FALSE(GeneralSections::IsPlausibleWire(Table(shortRx).data()));

    auto outsideSpace = kRealTable;
    outsideSpace[8] = 0x40000;
    EXPECT_FALSE(GeneralSections::IsPlausibleWire(Table(outsideSpace).data()));

    // A non-DICE answer: an all-ones block.
    EXPECT_FALSE(GeneralSections::IsPlausibleWire(
        Table({0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
               0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff})
            .data()));
}

} // namespace
