// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DeviceProtocolChoiceTests.cpp
//
// DeviceProtocolFactory::Create used to be a vendor/model if-chain that no test
// could reach: it constructs DriverKit objects, so it only ran on hardware. The
// half that decides *which* protocol is now split out as ChooseProtocol, which
// is pure -- and that half is where issue #115 lived, as a missing clause that
// nothing objected to.
//
// These pin the decision for every device this driver streams. A device whose
// row the catalog calls Supported and that resolves to no builder is the #115
// shape exactly, and is asserted impossible here rather than discovered on a
// bench.

#include "Audio/Protocols/DeviceProtocolChoice.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "Discovery/DiscoveryTypes.hpp"

#include <gtest/gtest.h>

#include <optional>

namespace {

using ASFW::Audio::ChooseDeviceProtocol;
using namespace ASFW::DeviceProfiles::Audio;
namespace Discovery = ASFW::Discovery;

constexpr uint32_t kDiceInterfaceVersion = 0x000001;
constexpr uint32_t kTa1394AvcSpecifier = 0x00A02D;
constexpr uint32_t kTa1394AvcVersion = 0x010001;
constexpr uint32_t kFireworksVersion = 0x010000;

struct UnitSpec {
    uint32_t offset{4};
    std::optional<uint32_t> specifierId{};
    std::optional<uint32_t> version{};
};

[[nodiscard]] Discovery::DeviceRecord MakeDevice(uint32_t vendorId,
                                                 std::optional<uint32_t> modelId,
                                                 std::vector<UnitSpec> units) {
    Discovery::DeviceRecord device{};
    device.instanceId = Discovery::DeviceInstanceId{7};
    device.identity.observedGuid =
        (static_cast<uint64_t>(vendorId) << 40U) | 0x04'0000'0000ULL;
    device.identity.nodeVendorOui = vendorId;
    device.identity.rootVendorId = vendorId;
    device.identity.rootModelId = modelId;
    for (const auto& spec : units) {
        Discovery::UnitIdentityEvidence unit{};
        unit.unitDirectoryOffset = spec.offset;
        unit.specifierId = spec.specifierId;
        unit.version = spec.version;
        device.identity.units.push_back(unit);
    }
    return device;
}

[[nodiscard]] Discovery::DeviceRecord DiceDevice(uint32_t vendorId, uint32_t modelId) {
    return MakeDevice(vendorId, modelId,
                      {{.offset = 5,
                        .specifierId = vendorId,
                        .version = kDiceInterfaceVersion}});
}

[[nodiscard]] Discovery::DeviceRecord AvcDevice(uint32_t vendorId, uint32_t modelId) {
    return MakeDevice(vendorId, modelId,
                      {{.offset = 5,
                        .specifierId = kTa1394AvcSpecifier,
                        .version = kTa1394AvcVersion}});
}

// ---------------------------------------------------------------------------
// Every device this driver streams picks the protocol it picked before
// ---------------------------------------------------------------------------

struct Expectation {
    const char* what;
    Discovery::DeviceRecord device;
    ProfileBuilderId builder;
};

[[nodiscard]] std::vector<Expectation> AllStreamedDevices() {
    std::vector<Expectation> cases;
    cases.push_back({"Saffire Pro 14", DiceDevice(kFocusriteVendorId, kSPro14ModelId),
                     ProfileBuilderId::FocusriteSPro14});
    cases.push_back({"Saffire Pro 24", DiceDevice(kFocusriteVendorId, kSPro24ModelId),
                     ProfileBuilderId::FocusriteSPro24});
    cases.push_back({"Saffire Pro 24 DSP",
                     DiceDevice(kFocusriteVendorId, kSPro24DspModelId),
                     ProfileBuilderId::FocusriteSPro24Dsp});
    cases.push_back({"Saffire Pro 40", DiceDevice(kFocusriteVendorId, kSPro40ModelId),
                     ProfileBuilderId::FocusriteSPro40});
    cases.push_back({"Weiss INT202", DiceDevice(kWeissVendorId, kWeissInt202ModelId),
                     ProfileBuilderId::WeissInt202});
    cases.push_back({"Weiss INT203", DiceDevice(kWeissVendorId, kWeissInt203ModelId),
                     ProfileBuilderId::WeissInt203});
    cases.push_back({"Alesis MultiMix",
                     DiceDevice(kAlesisVendorId, kAlesisMultiMixModelId),
                     ProfileBuilderId::AlesisMultiMix});
    cases.push_back({"Midas Venice", DiceDevice(kMidasVendorId, kMidasVeniceModelId),
                     ProfileBuilderId::MidasVeniceF32});
    cases.push_back({"StudioLive 16.0.2",
                     DiceDevice(kPreSonusVendorId, kStudioLive1602ModelId),
                     ProfileBuilderId::PreSonusStudioLive1602});
    cases.push_back({"StudioLive 24.4.2",
                     DiceDevice(kPreSonusVendorId, kStudioLive2442ModelId),
                     ProfileBuilderId::PreSonusStudioLive2442});
    cases.push_back({"Apogee Duet", AvcDevice(kApogeeVendorId, kApogeeDuetModelId),
                     ProfileBuilderId::ApogeeDuet});
    cases.push_back({"TerraTec PHASE 88",
                     AvcDevice(kTerraTecVendorId, kPhase88RackFwModelId),
                     ProfileBuilderId::TerraTecPhase88});
    cases.push_back({"Mackie Onyx-i (Oxford)",
                     AvcDevice(kMackieVendorId, kOnyxIOxfwModelId),
                     ProfileBuilderId::MackieOnyxIOxfw});
    cases.push_back({"Mackie Onyx 400F",
                     MakeDevice(kMackieVendorId, kOnyx400FModelId,
                                {{.offset = 5,
                                  .specifierId = kTa1394AvcSpecifier,
                                  .version = kFireworksVersion}}),
                     ProfileBuilderId::MackieOnyx400F});
    cases.push_back({"MOTU 828mkII",
                     MakeDevice(kMotuVendorId, 0U,
                                {{.offset = 5,
                                  .specifierId = kMotuVendorId,
                                  .version = kMotu828mk2SwVersion}}),
                     ProfileBuilderId::Motu828mk2});
    cases.push_back({"MOTU UltraLite",
                     MakeDevice(kMotuVendorId, 0U,
                                {{.offset = 5,
                                  .specifierId = kMotuVendorId,
                                  .version = kMotuUltraliteSwVersion}}),
                     ProfileBuilderId::MotuUltralite});
    return cases;
}

TEST(DeviceProtocolChoice, EveryStreamedDevicePicksItsProtocol) {
    for (const auto& expected : AllStreamedDevices()) {
        const auto choice = ChooseDeviceProtocol(expected.device);
        ASSERT_TRUE(choice.has_value()) << expected.what << " resolves to no protocol";
        EXPECT_EQ(choice->builder, expected.builder) << expected.what;
    }
}

// MOTU is the one family whose protocol needs a value out of the unit
// directory: the chunk layout is chosen by Unit_Sw_Version, because the root
// model_id is 0 for every MOTU device.
TEST(DeviceProtocolChoice, MotuCarriesItsUnitVersionToTheProtocol) {
    const auto device = MakeDevice(kMotuVendorId, 0U,
                                   {{.offset = 5,
                                     .specifierId = kMotuVendorId,
                                     .version = kMotu828mk2SwVersion}});
    const auto choice = ChooseDeviceProtocol(device);
    ASSERT_TRUE(choice.has_value());
    EXPECT_EQ(choice->unitVersion, kMotu828mk2SwVersion);
}

// ---------------------------------------------------------------------------
// And nothing else does
// ---------------------------------------------------------------------------

TEST(DeviceProtocolChoice, ARecognisedButUnplayableDeviceGetsNoProtocol) {
    // Every one of these shares an OUI with a device we do stream. Before the
    // catalog, FocusriteSaffireProfile::Matches keyed on vendor alone.
    const std::pair<uint32_t, uint32_t> unplayable[] = {
        {kFocusriteVendorId, kSPro40Tcd3070ModelId},
        {kFocusriteVendorId, kLiquidS56ModelId},
        {kFocusriteVendorId, kSPro26ModelId},
        {kWeissVendorId, kWeissDac202ModelId},
        {kWeissVendorId, kWeissMan301ModelId},
        {kPreSonusVendorId, kStudioLive1642ModelId},
        {kPreSonusVendorId, kStudioLive3242ModelId},
        {kMackieVendorId, kOnyxBlackbirdModelId},
    };
    for (const auto& [vendorId, modelId] : unplayable) {
        const auto device = DiceDevice(vendorId, modelId);
        EXPECT_FALSE(ChooseDeviceProtocol(device).has_value())
            << "vendor 0x" << std::hex << vendorId << " model 0x" << modelId;
    }
}

TEST(DeviceProtocolChoice, AnUnverifiedMotuSiblingGetsNoProtocol) {
    for (const uint32_t version :
         {kMotu896hdSwVersion, kMotuTravelerSwVersion, kMotu8preSwVersion}) {
        const auto device = MakeDevice(kMotuVendorId, 0U,
                                       {{.offset = 5,
                                         .specifierId = kMotuVendorId,
                                         .version = version}});
        EXPECT_FALSE(ChooseDeviceProtocol(device).has_value())
            << "version 0x" << std::hex << version;
    }
}

// The generic AV/C classification is not a decision to stream. This branch has
// no generic AV/C backend, and treating the fallback as one would start talking
// to every AV/C device on the bus.
TEST(DeviceProtocolChoice, AnUnknownAvcDeviceGetsNoProtocol) {
    const auto device = AvcDevice(0x00AABB, 0x000042);
    const auto choice = ChooseDeviceProtocol(device);
    if (choice.has_value()) {
        EXPECT_EQ(choice->builder, ProfileBuilderId::GenericAvc)
            << "an unknown AV/C device resolved to a real builder";
    }
}

TEST(DeviceProtocolChoice, TheMAudioSpecialFirmwareGetsNoProtocol) {
    for (const uint32_t model : {kMAudioFireWire1814ModelId,
                                 kMAudioProjectMixModelId,
                                 kMAudioFireWire1814BootloaderModelId}) {
        const auto device = AvcDevice(kMAudioVendorId, model);
        const auto choice = ChooseDeviceProtocol(device);
        EXPECT_FALSE(choice.has_value())
            << "model 0x" << std::hex << model
            << " must be recognised for its command bound only";
    }
}

// A device with no units at all cannot resolve to anything, and must not crash
// trying. Real ROMs on the bus do turn up unit-less.
TEST(DeviceProtocolChoice, ADeviceWithNoUnitsGetsNoProtocol) {
    const auto device = MakeDevice(kFocusriteVendorId, kSPro24DspModelId, {});
    EXPECT_FALSE(ChooseDeviceProtocol(device).has_value());
}

// A non-audio unit listed before the audio one must not stop the walk.
TEST(DeviceProtocolChoice, ASiblingUnitBeforeTheAudioOneIsSkipped) {
    auto device = MakeDevice(kFocusriteVendorId, kSPro24DspModelId,
                             {{.offset = 4, .specifierId = 0x00609E, .version = 0x010483},
                              {.offset = 9,
                               .specifierId = kFocusriteVendorId,
                               .version = kDiceInterfaceVersion}});
    const auto choice = ChooseDeviceProtocol(device);
    ASSERT_TRUE(choice.has_value());
    EXPECT_EQ(choice->builder, ProfileBuilderId::FocusriteSPro24Dsp);
    EXPECT_EQ(choice->unitDirectoryOffset, 9U);
}

// ---------------------------------------------------------------------------
// Identity the ROM does not state
// ---------------------------------------------------------------------------

// Focusrite DICE boards encode the model in GUID bits [27:22], and the old path
// leaned on that: DeviceRegistry::MaybeInferKnownIdentityFromGuid rewrote the
// flat modelId when the ROM did not surface one, so Create() matched on a value
// no ROM had published.
//
// The catalog matches raw evidence, so it must carry that second route
// explicitly or a device whose ROM omits the model silently stops resolving.
// The GUID here is the bench unit's, 0x00130E0402004713 -> field 8 -> Pro 24 DSP.
TEST(DeviceProtocolChoice, AFocusriteBoardResolvesFromItsGuidWhenTheRomOmitsTheModel) {
    Discovery::DeviceRecord device{};
    device.instanceId = Discovery::DeviceInstanceId{3};
    device.identity.observedGuid = 0x00130E0402004713ULL;
    device.identity.nodeVendorOui = kFocusriteVendorId;
    device.identity.rootVendorId = kFocusriteVendorId;
    device.identity.rootModelId = std::nullopt;  // the ROM said nothing
    Discovery::UnitIdentityEvidence unit{};
    unit.unitDirectoryOffset = 5;
    unit.specifierId = kFocusriteVendorId;
    unit.version = kDiceInterfaceVersion;
    device.identity.units.push_back(unit);

    const auto choice = ChooseDeviceProtocol(device);
    ASSERT_TRUE(choice.has_value())
        << "the bench Saffire Pro 24 DSP stopped resolving when its model id is "
           "GUID-encoded rather than ROM-stated";
    EXPECT_EQ(choice->builder, ProfileBuilderId::FocusriteSPro24Dsp);
}

// The TCD3070 Pro 40's GUID model field is 0x13 while its ROM model is 0x0000de
// -- Linux documents the mismatch verbatim (dice.c:385-393). Both routes must
// land on the same definition, and that definition must stay unplayable.
TEST(DeviceProtocolChoice, TheTcd3070IsTheSameDeviceByEitherRoute) {
    Discovery::DeviceRecord byGuid{};
    byGuid.instanceId = Discovery::DeviceInstanceId{4};
    byGuid.identity.observedGuid =
        (static_cast<uint64_t>(kFocusriteVendorId) << 40U) |
        (static_cast<uint64_t>(kFocusriteGuidModelSPro40Tcd3070) << 22U);
    byGuid.identity.nodeVendorOui = kFocusriteVendorId;
    byGuid.identity.rootVendorId = kFocusriteVendorId;
    Discovery::UnitIdentityEvidence unit{};
    unit.unitDirectoryOffset = 5;
    unit.specifierId = kFocusriteVendorId;
    unit.version = kDiceInterfaceVersion;
    byGuid.identity.units.push_back(unit);

    EXPECT_FALSE(ChooseDeviceProtocol(byGuid).has_value());
    EXPECT_FALSE(
        ChooseDeviceProtocol(DiceDevice(kFocusriteVendorId, kSPro40Tcd3070ModelId))
            .has_value());
}

// ---------------------------------------------------------------------------
// Backend routing
// ---------------------------------------------------------------------------

// The question AudioIntegrationMode::kHardcodedNub used to answer. Preserving
// it exactly matters more than tidying it: routing a device to a backend that
// has no profile for it is silence with no error.
TEST(DeviceProtocolChoice, BackendRoutingMatchesWhatTheProfileRegistrySays) {
    using ASFW::Audio::AudioBackendKind;
    using ASFW::Audio::ChooseAudioBackend;

    for (const auto& expected : AllStreamedDevices()) {
        const auto backend = ChooseAudioBackend(expected.device);
        const auto choice = ChooseDeviceProtocol(expected.device);
        ASSERT_TRUE(choice.has_value()) << expected.what;

        switch (choice->builder) {
            case ProfileBuilderId::Motu828mk2:
            case ProfileBuilderId::MotuUltralite:
                EXPECT_EQ(backend, AudioBackendKind::MotuRegister) << expected.what;
                break;
            case ProfileBuilderId::FocusriteSPro14:
            case ProfileBuilderId::FocusriteSPro24:
            case ProfileBuilderId::FocusriteSPro24Dsp:
            case ProfileBuilderId::FocusriteSPro40:
            case ProfileBuilderId::WeissInt202:
            case ProfileBuilderId::WeissInt203:
            case ProfileBuilderId::AlesisMultiMix:
            case ProfileBuilderId::MidasVeniceF32:
            case ProfileBuilderId::PreSonusStudioLive1602:
            case ProfileBuilderId::PreSonusStudioLive2442:
                EXPECT_EQ(backend, AudioBackendKind::Dice) << expected.what;
                break;
            default:
                EXPECT_EQ(backend, AudioBackendKind::Avc) << expected.what;
                break;
        }
    }
}

// A recognised-but-unplayable DICE device routed to the DICE backend would be a
// change, not a cleanup: the old lookup returned kNone for it and it landed on
// AV/C, where it harmlessly does nothing. The DICE backend has no profile for
// it and would have to invent one.
TEST(DeviceProtocolChoice, ARecognisedButUnplayableDeviceStaysOnTheAvcBackend) {
    using ASFW::Audio::AudioBackendKind;
    using ASFW::Audio::ChooseAudioBackend;

    const std::pair<uint32_t, uint32_t> unplayableDice[] = {
        {kFocusriteVendorId, kSPro40Tcd3070ModelId},
        {kFocusriteVendorId, kLiquidS56ModelId},
        {kPreSonusVendorId, kStudioLive3242ModelId},
        {kWeissVendorId, kWeissMan301ModelId},
    };
    for (const auto& [vendorId, modelId] : unplayableDice) {
        EXPECT_EQ(ChooseAudioBackend(DiceDevice(vendorId, modelId)),
                  AudioBackendKind::Avc)
            << "vendor 0x" << std::hex << vendorId << " model 0x" << modelId;
    }

    for (const uint32_t version :
         {kMotu896hdSwVersion, kMotuTravelerSwVersion, kMotu8preSwVersion}) {
        const auto device = MakeDevice(kMotuVendorId, 0U,
                                       {{.offset = 5,
                                         .specifierId = kMotuVendorId,
                                         .version = version}});
        EXPECT_EQ(ChooseAudioBackend(device), AudioBackendKind::Avc)
            << "MOTU version 0x" << std::hex << version;
    }
}

TEST(DeviceProtocolChoice, AnUnknownDeviceRoutesToAvc) {
    using ASFW::Audio::AudioBackendKind;
    using ASFW::Audio::ChooseAudioBackend;
    EXPECT_EQ(ChooseAudioBackend(AvcDevice(0x00AABB, 0x000042)),
              AudioBackendKind::Avc);
    EXPECT_EQ(ChooseAudioBackend(MakeDevice(0x00AABB, 0x000042, {})),
              AudioBackendKind::Avc);
}

// The #115 shape, asserted impossible: a row the catalog calls Supported that
// resolves to no protocol.
TEST(DeviceProtocolChoice, NoSupportedCatalogRowResolvesToNothing) {
    for (const auto& definition : AudioDeviceCatalog::Definitions()) {
        if (definition.support != SupportDisposition::Supported) {
            continue;
        }
        EXPECT_NE(definition.profileBuilder, ProfileBuilderId::None)
            << "definition " << static_cast<uint32_t>(definition.id)
            << " is Supported but names no builder, so Create() would return "
               "nullptr while the nub is published anyway -- this is #115";
    }
}

} // namespace
