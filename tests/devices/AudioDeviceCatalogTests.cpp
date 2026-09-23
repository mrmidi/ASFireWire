// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioDeviceCatalogTests.cpp
//
// Issue #115 in one sentence: the StudioLive 24.4.2 got a profile and a
// DiceProfileRegistry entry but no DeviceProtocolFactory clause, then published
// a nub, allocated isochronous bandwidth, and failed every StartIO with nothing
// logged. Five independent matchers, no shared table, and a half-add that
// nothing objected to.
//
// The catalog is the one table. These tests hold it to the properties that make
// a half-add impossible: every Supported row resolves to a family, a probe and a
// builder and protocol; no two rows can claim the same device; and a row with no builder is
// never reported as playable.

#include "DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "Discovery/DiscoveryTypes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <ios>
#include <set>
#include <string_view>

namespace {

using namespace ASFW::DeviceProfiles::Audio;
namespace Discovery = ASFW::Discovery;

// ---------------------------------------------------------------------------
// Fixtures built from Config-ROM evidence, never from a profile
// ---------------------------------------------------------------------------

struct UnitSpec {
    uint32_t offset{4};
    std::optional<uint32_t> specifierId{};
    std::optional<uint32_t> version{};
};

[[nodiscard]] Discovery::DeviceRecord MakeDevice(uint64_t guid,
                                                 std::optional<uint32_t> vendorId,
                                                 std::optional<uint32_t> modelId,
                                                 std::vector<UnitSpec> units) {
    Discovery::DeviceRecord device{};
    device.instanceId = Discovery::DeviceInstanceId{1};
    device.identity.observedGuid = guid;
    device.identity.nodeVendorOui = static_cast<uint32_t>(guid >> 40U) & 0xFFFFFFU;
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

constexpr uint32_t kDiceInterfaceVersion = 0x000001;
constexpr uint32_t kTa1394AvcSpecifier = 0x00A02D;
constexpr uint32_t kTa1394AvcVersion = 0x010001;

// ---------------------------------------------------------------------------
// The table itself
// ---------------------------------------------------------------------------

TEST(AudioDeviceCatalog, TheTableIsInternallyConsistent) {
    const auto issues = AudioDeviceCatalog::Validate();
    for (const auto& issue : issues) {
        ADD_FAILURE() << "definition " << static_cast<uint32_t>(issue.first) << " vs "
                      << static_cast<uint32_t>(issue.second) << ": "
                      << (issue.reason != nullptr ? issue.reason : "(no reason)");
    }
    EXPECT_TRUE(issues.empty());
}

// This is the #115 assertion. A row that claims to be Supported and does not
// name family, probe policy, profile builder and protocol implementation is the
// half-add that published a nub and then could not stream.
TEST(AudioDeviceCatalog, EverySupportedRowNamesAFamilyAProbeABuilderAndAProtocol) {
    for (const auto& definition : AudioDeviceCatalog::Definitions()) {
        if (definition.support != SupportDisposition::Supported) {
            continue;
        }
        const auto id = static_cast<uint32_t>(definition.id);
        EXPECT_NE(definition.family, AudioFamilyProviderId::None)
            << "definition " << id << " is Supported with no family provider";
        EXPECT_NE(definition.probePolicy, ProbePolicyId::None)
            << "definition " << id << " is Supported with no probe policy";
        EXPECT_NE(definition.profileBuilder, ProfileBuilderId::None)
            << "definition " << id << " is Supported with no profile builder";
        EXPECT_NE(definition.protocolImplementation, ProtocolImplementationId::None)
            << "definition " << id << " is Supported with no protocol implementation";
    }
}

TEST(AudioDeviceCatalog, DistinctDiceProfilesShareOneProtocolImplementation) {
    const auto spro14 = MakeDevice(0x00130e0000000001ULL, kFocusriteVendorId,
                                   kSPro14ModelId,
                                   {{.offset = 5, .version = kDiceInterfaceVersion}});
    const auto spro24 = MakeDevice(0x00130e0000000002ULL, kFocusriteVendorId,
                                   kSPro24ModelId,
                                   {{.offset = 5, .version = kDiceInterfaceVersion}});
    const auto first = AudioDeviceCatalog::Resolve(spro14);
    const auto second = AudioDeviceCatalog::Resolve(spro24);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_NE(first->profileBuilder, second->profileBuilder);
    EXPECT_EQ(first->protocolImplementation, ProtocolImplementationId::DiceTcat);
    EXPECT_EQ(second->protocolImplementation, ProtocolImplementationId::DiceTcat);
}

TEST(AudioDeviceCatalog, RejectsProtocolThatDisagreesWithFamily) {
    auto definitions = std::vector<AudioDeviceDefinition>(
        AudioDeviceCatalog::Definitions().begin(),
        AudioDeviceCatalog::Definitions().end());
    const auto it = std::ranges::find_if(definitions, [](const auto& definition) {
        return definition.id == DeviceDefinitionId::FocusriteSPro14;
    });
    ASSERT_NE(it, definitions.end());
    it->protocolImplementation = ProtocolImplementationId::MotuV2;

    const auto issues = AudioDeviceCatalog::ValidateDefinitions(definitions);
    EXPECT_TRUE(std::ranges::any_of(issues, [](const auto& issue) {
        return issue.reason != nullptr &&
               std::string_view(issue.reason) ==
                   "supported definition has incompatible family/probe/protocol";
    }));
}

// The converse, and the reason vendor-wide matching had to end: a device we do
// not support must not be able to reach a sibling's geometry. Before the
// catalog, FocusriteSaffireProfile::Matches keyed on vendor alone, so the
// TCD3070 Pro 40, the Liquid 56 and the Pro 26 all matched it and would have
// been handed the Pro 24's 8/16 geometry.
TEST(AudioDeviceCatalog, AnUnsupportedRowResolvesToNoBuilderAtAll) {
    for (const auto& definition : AudioDeviceCatalog::Definitions()) {
        if (definition.support == SupportDisposition::Supported) {
            continue;
        }
        EXPECT_EQ(definition.profileBuilder, ProfileBuilderId::None)
            << "definition " << static_cast<uint32_t>(definition.id)
            << " is not Supported but names a profile builder";
        EXPECT_EQ(definition.protocolImplementation, ProtocolImplementationId::None)
            << "definition " << static_cast<uint32_t>(definition.id)
            << " is not Supported but names a protocol implementation";
    }
}

TEST(AudioDeviceCatalog, NoTwoRowsShareADefinitionId) {
    std::set<DeviceDefinitionId> seen;
    for (const auto& definition : AudioDeviceCatalog::Definitions()) {
        EXPECT_NE(definition.id, DeviceDefinitionId::Unknown);
        EXPECT_TRUE(seen.insert(definition.id).second)
            << "duplicate definition id " << static_cast<uint32_t>(definition.id);
    }
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------

TEST(AudioDeviceCatalog, TheSaffirePro24DspResolvesToItsOwnBuilder) {
    const auto device = MakeDevice(0x00130E'0400000000ULL, kFocusriteVendorId,
                                   kSPro24DspModelId,
                                   {{.offset = 5,
                                     .specifierId = kFocusriteVendorId,
                                     .version = kDiceInterfaceVersion}});
    const auto plan = AudioDeviceCatalog::Resolve(device, device.identity.units[0]);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->support, SupportDisposition::Supported);
    EXPECT_EQ(plan->family, AudioFamilyProviderId::DICE);
    EXPECT_EQ(plan->profileBuilder, ProfileBuilderId::FocusriteSPro24Dsp);
    EXPECT_EQ(plan->unit.device, device.instanceId);
    EXPECT_EQ(plan->unit.unitDirectoryOffset, 5U);
}

// The 24.4.2 is the device #115 was about. It must resolve, and to its own
// builder rather than to the 16.0.2's.
TEST(AudioDeviceCatalog, TheStudioLive2442ResolvesToItsOwnBuilder) {
    const auto device = MakeDevice(0x000A9204049204CBULL, kPreSonusVendorId,
                                   kStudioLive2442ModelId,
                                   {{.offset = 5,
                                     .specifierId = kPreSonusVendorId,
                                     .version = kDiceInterfaceVersion}});
    const auto plan = AudioDeviceCatalog::Resolve(device, device.identity.units[0]);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->support, SupportDisposition::Supported);
    EXPECT_EQ(plan->profileBuilder, ProfileBuilderId::PreSonusStudioLive2442);
    EXPECT_NE(plan->profileBuilder, ProfileBuilderId::PreSonusStudioLive1602);
}

// Linux constrains a DICE unit on the interface version alone in its catch-all
// (dice.c:457-460), and writes Weiss, the PreSonus FireStudio and the TCD3070
// Saffire without a specifier constraint. The Midas Venice is the live reason:
// its vendor kext keys on Unit_Spec_ID 0x10C63F while the root vendor is
// 0x10C73F, so a definition that pinned specifier == vendor would never match it.
TEST(AudioDeviceCatalog, ADiceUnitMatchesOnTheInterfaceVersionNotTheSpecifier) {
    const auto device = MakeDevice(0x10C73F040040C7B6ULL, kMidasVendorId,
                                   kMidasVeniceModelId,
                                   {{.offset = 5,
                                     .specifierId = 0x10C63F,  // not the root vendor
                                     .version = kDiceInterfaceVersion}});
    const auto plan = AudioDeviceCatalog::Resolve(device, device.identity.units[0]);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->profileBuilder, ProfileBuilderId::MidasVeniceF32);
}

// MOTU is why DeviceIdentityEvidence exists. The root directory publishes a
// model_id of 0, so a flattened query cannot tell it from a device that
// published no model_id at all, and the model lives in Unit_Sw_Version instead.
TEST(AudioDeviceCatalog, MotuIsMatchedFromTheUnitDirectory) {
    const auto device = MakeDevice(0x0001F2'0400000000ULL, kMotuVendorId,
                                   /*modelId=*/0U,
                                   {{.offset = 5,
                                     .specifierId = kMotuVendorId,
                                     .version = kMotu828mk2SwVersion}});
    const auto plan = AudioDeviceCatalog::Resolve(device, device.identity.units[0]);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->family, AudioFamilyProviderId::MotuRegister);
    EXPECT_EQ(plan->profileBuilder, ProfileBuilderId::Motu828mk2);
}

TEST(AudioDeviceCatalog, AnUnverifiedMotuSiblingIsNamedButNotPlayable) {
    const auto device = MakeDevice(0x0001F2'0400000000ULL, kMotuVendorId,
                                   /*modelId=*/0U,
                                   {{.offset = 5,
                                     .specifierId = kMotuVendorId,
                                     .version = kMotu8preSwVersion}});
    const auto plan = AudioDeviceCatalog::Resolve(device, device.identity.units[0]);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->support, SupportDisposition::RecognizedUnsupported);
    EXPECT_EQ(plan->profileBuilder, ProfileBuilderId::None);
    EXPECT_EQ(plan->modelName, kMotu8preModelName);
}

// A model_id that was never published must not be read as a model_id of 0, or
// every DICE device on a vendor with a zero-model sibling collides.
TEST(AudioDeviceCatalog, AnAbsentModelIdDoesNotMatchARowThatRequiresZero) {
    const auto device = MakeDevice(0x0001F2'0400000000ULL, kMotuVendorId,
                                   /*modelId=*/std::nullopt,
                                   {{.offset = 5,
                                     .specifierId = kMotuVendorId,
                                     .version = kMotu828mk2SwVersion}});
    const auto plan = AudioDeviceCatalog::Resolve(device, device.identity.units[0]);
    EXPECT_FALSE(plan.has_value());
}

// The end of vendor-wide matching, stated as a test. The TCD3070 Pro 40 shares
// the Focusrite OUI and nothing else; it must not inherit the Pro 24's builder.
TEST(AudioDeviceCatalog, TheTcd3070Pro40DoesNotInheritASiblingsBuilder) {
    const auto device = MakeDevice(0x00130E'0404C00000ULL, kFocusriteVendorId,
                                   kSPro40Tcd3070ModelId,
                                   {{.offset = 5,
                                     .specifierId = kFocusriteVendorId,
                                     .version = kDiceInterfaceVersion}});
    const auto plan = AudioDeviceCatalog::Resolve(device, device.identity.units[0]);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->support, SupportDisposition::RecognizedUnsupported);
    EXPECT_EQ(plan->profileBuilder, ProfileBuilderId::None);
}

// A definition constrains the unit it selected, so a non-audio sibling unit on
// the same physical node cannot pick up a root-level audio definition.
TEST(AudioDeviceCatalog, ASiblingUnitOnTheSameNodeDoesNotInheritTheDefinition) {
    const auto device = MakeDevice(0x00130E'0400000000ULL, kFocusriteVendorId,
                                   kSPro24DspModelId,
                                   {{.offset = 4,
                                     .specifierId = 0x00609E,  // SBP-2
                                     .version = 0x010483},
                                    {.offset = 9,
                                     .specifierId = kFocusriteVendorId,
                                     .version = kDiceInterfaceVersion}});

    const auto sbp2 = AudioDeviceCatalog::Resolve(device, device.identity.units[0]);
    EXPECT_FALSE(sbp2.has_value());

    const auto dice = AudioDeviceCatalog::Resolve(device, device.identity.units[1]);
    ASSERT_TRUE(dice.has_value());
    EXPECT_EQ(dice->profileBuilder, ProfileBuilderId::FocusriteSPro24Dsp);
}

TEST(AudioDeviceCatalog, AUnitThatIsNotOnTheDeviceIsRefused) {
    const auto device = MakeDevice(0x00130E'0400000000ULL, kFocusriteVendorId,
                                   kSPro24DspModelId,
                                   {{.offset = 5,
                                     .specifierId = kFocusriteVendorId,
                                     .version = kDiceInterfaceVersion}});
    Discovery::UnitIdentityEvidence stranger{};
    stranger.unitDirectoryOffset = 77;
    stranger.specifierId = kFocusriteVendorId;
    stranger.version = kDiceInterfaceVersion;

    const auto plan = AudioDeviceCatalog::Resolve(device, stranger);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error(), CatalogResolutionError::InvalidUnit);
}

TEST(AudioDeviceCatalog, ADeviceWithNoInstanceIdIsRefused) {
    auto device = MakeDevice(0x00130E'0400000000ULL, kFocusriteVendorId,
                             kSPro24DspModelId,
                             {{.offset = 5,
                               .specifierId = kFocusriteVendorId,
                               .version = kDiceInterfaceVersion}});
    device.instanceId = Discovery::DeviceInstanceId{};

    const auto plan = AudioDeviceCatalog::Resolve(device, device.identity.units[0]);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error(), CatalogResolutionError::InvalidUnit);
}

// ---------------------------------------------------------------------------
// Generic AV/C fallback
// ---------------------------------------------------------------------------

TEST(AudioDeviceCatalog, AnUnknownAvcUnitFallsBackToGenericAvc) {
    const auto device = MakeDevice(0x00AABB'0400000000ULL, 0x00AABB, 0x000042,
                                   {{.offset = 5,
                                     .specifierId = kTa1394AvcSpecifier,
                                     .version = kTa1394AvcVersion}});
    const auto plan = AudioDeviceCatalog::Resolve(device, device.identity.units[0]);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->support, SupportDisposition::GenericFallback);
    EXPECT_EQ(plan->family, AudioFamilyProviderId::GenericAvc);
}

// An unknown DICE unit must not fall through into an FCP probe: DICE uses a
// vendor specifier with interface version 0x000001 and speaks no AV/C at all.
TEST(AudioDeviceCatalog, AnUnknownDiceUnitDoesNotFallBackIntoAnAvcProbe) {
    const auto device = MakeDevice(0x00AABB'0400000000ULL, 0x00AABB, 0x000042,
                                   {{.offset = 5,
                                     .specifierId = 0x00AABB,
                                     .version = kDiceInterfaceVersion}});
    const auto plan = AudioDeviceCatalog::Resolve(device, device.identity.units[0]);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error(), CatalogResolutionError::NoMatch);
}

// ---------------------------------------------------------------------------
// Coverage this branch must not lose
// ---------------------------------------------------------------------------

// `midi` dropped MOTU, the Mackie Onyx-i/400F and the StudioLive 24.4.2 when it
// introduced the catalog. Forward-porting the catalog must not re-drop them.
TEST(AudioDeviceCatalog, TheDevicesThisBranchStreamsAreAllSupported) {
    constexpr DeviceDefinitionId kMustStream[] = {
        DeviceDefinitionId::FocusriteSPro14,
        DeviceDefinitionId::FocusriteSPro24,
        DeviceDefinitionId::FocusriteSPro24Dsp,
        DeviceDefinitionId::FocusriteSPro40,
        DeviceDefinitionId::WeissInt202,
        DeviceDefinitionId::WeissInt203,
        DeviceDefinitionId::ApogeeDuet,
        DeviceDefinitionId::TerraTecPhase88,
        DeviceDefinitionId::AlesisMultiMix,
        DeviceDefinitionId::MidasVeniceF32,
        DeviceDefinitionId::PreSonusStudioLive1602,
        DeviceDefinitionId::PreSonusStudioLive2442,
        DeviceDefinitionId::Motu828mk2,
        DeviceDefinitionId::MotuUltralite,
        DeviceDefinitionId::MackieOnyxIOxfw,
        DeviceDefinitionId::MackieOnyx400F,
    };
    const auto definitions = AudioDeviceCatalog::Definitions();
    for (const auto id : kMustStream) {
        const auto it = std::ranges::find_if(
            definitions,
            [id](const AudioDeviceDefinition& d) { return d.id == id; });
        ASSERT_NE(it, definitions.end())
            << "definition " << static_cast<uint32_t>(id) << " is missing from the catalog";
        EXPECT_EQ(it->support, SupportDisposition::Supported)
            << "definition " << static_cast<uint32_t>(id) << " lost Supported status";
    }
}

// No definition may be built from the pending-capture sentinel: it is not a
// valid 24-bit model id, so the row could never match anything.
TEST(AudioDeviceCatalog, NoRowIsBuiltFromAPendingCaptureSentinel) {
    for (const auto& definition : AudioDeviceCatalog::Definitions()) {
        for (uint8_t i = 0; i < definition.clauseCount; ++i) {
            const auto& model = definition.clauses[i].rootModelId;
            if (model.has_value()) {
                EXPECT_NE(model->value, kMackieModelIdPendingCapture)
                    << "definition " << static_cast<uint32_t>(definition.id)
                    << " matches on the pending-capture sentinel";
                EXPECT_LE(model->value, 0xFFFFFFU)
                    << "definition " << static_cast<uint32_t>(definition.id)
                    << " matches on a model id wider than 24 bits";
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The AV/C command bound
// ---------------------------------------------------------------------------

// M-Audio's special firmware hangs on AV/C it does not implement
// (AVC_DEVICE_HAZARDS.md H1). Being *unrecognised* is the unsafe state: an
// unmatched AV/C unit is opened with generic UNIT_INFO/SUBUNIT_INFO, two of the
// four shapes on record as freeze-capable. Recognising it is what bounds it.
TEST(AudioDeviceCatalog, TheMAudioSpecialFirmwareCarriesAFilteredCommandSet) {
    for (const uint32_t model : {kMAudioFireWire1814ModelId, kMAudioProjectMixModelId}) {
        const auto device = MakeDevice(0x000D6C'0400000000ULL, kMAudioVendorId, model,
                                       {{.offset = 5,
                                         .specifierId = kTa1394AvcSpecifier,
                                         .version = kTa1394AvcVersion}});
        // Recognised, but not playable here: this branch has no
        // MAudioSpecialProtocol, so no builder may be named.
        const auto plan = AudioDeviceCatalog::Resolve(device.identity);
        ASSERT_TRUE(plan.has_value());
        EXPECT_EQ(AudioDeviceCatalog::CommandFilterFor(*plan),
                  Discovery::AvcCommandFilterId::MAudioSpecialBeBoB)
            << "model 0x" << std::hex << model;
        EXPECT_EQ(plan->probePolicy, ProbePolicyId::BeBoBFilteredCommandSet);
        EXPECT_EQ(plan->profileBuilder, ProfileBuilderId::None);
        EXPECT_NE(plan->support, SupportDisposition::GenericFallback)
            << "model 0x" << std::hex << model
            << " fell through to generic AV/C, which is the freeze path";
    }
}

// The bootloader persona is not an audio endpoint and never becomes one. It
// exists to carry its cue policy and to keep the identity off the generic path.
TEST(AudioDeviceCatalog, TheMAudioBootloaderPersonaIsNeverAnAudioEndpoint) {
    const auto device = MakeDevice(0x000D6C'0400000000ULL, kMAudioVendorId,
                                   kMAudioFireWire1814BootloaderModelId,
                                   {{.offset = 5,
                                     .specifierId = kTa1394AvcSpecifier,
                                     .version = kTa1394AvcVersion}});
    const auto plan = AudioDeviceCatalog::Resolve(device.identity);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->family, AudioFamilyProviderId::None);
    EXPECT_EQ(plan->probePolicy, ProbePolicyId::NoAutomaticTraffic);
    EXPECT_EQ(plan->profileBuilder, ProfileBuilderId::None);
    EXPECT_EQ(plan->bootloaderCue, BootloaderCuePolicy::BeBoBStartFirmware);
    EXPECT_EQ(AudioDeviceCatalog::CommandFilterFor(*plan),
              Discovery::AvcCommandFilterId::BlockAll);
}

// Every ordinary device stays unrestricted. A non-empty allowlist is a bound on
// what the driver may send, so applying one by accident would silently break a
// working device.
TEST(AudioDeviceCatalog, AnOrdinaryDeviceIsNotCommandFiltered) {
    const auto duet = MakeDevice(0x0003DB'0400000000ULL, kApogeeVendorId,
                                 kApogeeDuetModelId,
                                 {{.offset = 5,
                                   .specifierId = kTa1394AvcSpecifier,
                                   .version = kTa1394AvcVersion}});
    const auto duetPlan = AudioDeviceCatalog::Resolve(duet.identity);
    ASSERT_TRUE(duetPlan.has_value());
    EXPECT_EQ(AudioDeviceCatalog::CommandFilterFor(*duetPlan),
              Discovery::AvcCommandFilterId::Unrestricted);

    const auto unknown = MakeDevice(0x00AABB'0400000000ULL, 0x00AABB, 0x000042,
                                    {{.offset = 5,
                                      .specifierId = kTa1394AvcSpecifier,
                                      .version = kTa1394AvcVersion}});
    const auto unknownPlan = AudioDeviceCatalog::Resolve(unknown.identity);
    ASSERT_TRUE(unknownPlan.has_value());
    EXPECT_EQ(AudioDeviceCatalog::CommandFilterFor(*unknownPlan),
              Discovery::AvcCommandFilterId::Unrestricted);
}

// No safety rule is defined on this branch, and nothing must be quarantined by
// accident: a safety rule stops the audio session, the family adapter and FCP
// transport construction alike.
TEST(AudioDeviceCatalog, NothingIsQuarantinedOnThisBranch) {
    EXPECT_TRUE(AudioDeviceCatalog::SafetyRules().empty());
    const auto device = MakeDevice(0x00130E'0400000000ULL, kFocusriteVendorId,
                                   kSPro24DspModelId,
                                   {{.offset = 5,
                                     .specifierId = kFocusriteVendorId,
                                     .version = kDiceInterfaceVersion}});
    EXPECT_FALSE(AudioDeviceCatalog::MatchAnySafetyRule(device.identity).has_value());
    const auto plan = AudioDeviceCatalog::Resolve(device.identity);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(AudioDeviceCatalog::CommandFilterFor(*plan),
              Discovery::AvcCommandFilterId::Unrestricted);
}

// ---------------------------------------------------------------------------
// Stage 1: Resolution Invariants and Multi-Unit Aggregation
// ---------------------------------------------------------------------------

TEST(AudioDeviceCatalog, DeviceLevelResolvePrefersCuratedOverGenericFallback) {
    // Device with two units: unit 0 is generic 1394TA AV/C, unit 1 is SPro24Dsp DICE
    const auto device = MakeDevice(0x00130E'0400000000ULL, kFocusriteVendorId,
                                   kSPro24DspModelId,
                                   {{.offset = 4,
                                     .specifierId = kTa1394AvcSpecifier,
                                     .version = kTa1394AvcVersion},
                                    {.offset = 8,
                                     .specifierId = kFocusriteVendorId,
                                     .version = kDiceInterfaceVersion}});

    const auto plan = AudioDeviceCatalog::Resolve(device.identity);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->family, AudioFamilyProviderId::DICE);
    EXPECT_EQ(plan->profileBuilder, ProfileBuilderId::FocusriteSPro24Dsp);
    EXPECT_EQ(plan->support, SupportDisposition::Supported);
}

TEST(AudioDeviceCatalog, DeviceLevelResolveFailsOnConflictingCuratedUnits) {
    // Hypothetical device with two conflicting curated units
    const auto device = MakeDevice(0x00130E'0400000000ULL, kFocusriteVendorId,
                                   kSPro24DspModelId,
                                   {{.offset = 4,
                                     .specifierId = kFocusriteVendorId,
                                     .version = kDiceInterfaceVersion},
                                    {.offset = 8,
                                     .specifierId = kTa1394AvcSpecifier,
                                     .version = kTa1394AvcVersion}});

    // Device with two conflicting curated units: unit 0 is 828mk2, unit 1 is UltraLite
    Discovery::DeviceIdentityEvidence devEvidence{};
    devEvidence.observedGuid = 0x0001F2'0400000000ULL;
    devEvidence.nodeVendorOui = kMotuVendorId;
    devEvidence.rootVendorId = kMotuVendorId;
    devEvidence.rootModelId = 0U;
    devEvidence.units.push_back(Discovery::UnitIdentityEvidence{
        .unitDirectoryOffset = 4,
        .specifierId = kMotuVendorId,
        .version = kMotu828mk2SwVersion,
    });
    devEvidence.units.push_back(Discovery::UnitIdentityEvidence{
        .unitDirectoryOffset = 8,
        .specifierId = kMotuVendorId,
        .version = kMotuUltraliteSwVersion,
    });

    const auto res = AudioDeviceCatalog::Resolve(devEvidence);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), CatalogResolutionError::AmbiguousIdentity);
}

TEST(AudioDeviceCatalog, ResolutionCarriesBuilderAndStreamTraits) {
    const auto duet = MakeDevice(0x0003DB'0400000000ULL, kApogeeVendorId,
                                 kApogeeDuetModelId,
                                 {{.offset = 5,
                                   .specifierId = kTa1394AvcSpecifier,
                                   .version = kTa1394AvcVersion}});

    const auto plan = AudioDeviceCatalog::Resolve(duet.identity);
    ASSERT_TRUE(plan.has_value());

    EXPECT_EQ(plan->profileBuilder, ProfileBuilderId::ApogeeDuet);
    EXPECT_EQ(plan->streamTraits.startRatePinHz, 48000U);
    EXPECT_EQ(plan->streamTraits.startShape, StreamStartShape::ApogeeInterleaved);
}

TEST(AudioDeviceCatalog, StartRatePinHzIsAccurateForOnyxAndDuet) {
    const auto onyx = MakeDevice(0x000FF2'0400000000ULL, kMackieVendorId,
                                 kOnyxIOxfwModelId,
                                 {{.offset = 5,
                                   .specifierId = kTa1394AvcSpecifier,
                                   .version = kTa1394AvcVersion}});
    const auto onyxPlan = AudioDeviceCatalog::Resolve(onyx.identity);
    ASSERT_TRUE(onyxPlan.has_value());
    EXPECT_EQ(onyxPlan->streamTraits.startRatePinHz, 44100U);

    const auto spro = MakeDevice(0x00130E'0400000000ULL, kFocusriteVendorId,
                                 kSPro24DspModelId,
                                 {{.offset = 5,
                                   .specifierId = kFocusriteVendorId,
                                   .version = kDiceInterfaceVersion}});
    const auto sproPlan = AudioDeviceCatalog::Resolve(spro.identity);
    ASSERT_TRUE(sproPlan.has_value());
    EXPECT_EQ(sproPlan->streamTraits.startRatePinHz, 0U);
}

TEST(AudioDeviceCatalog, CommandFilterForNeverFallsBackToUnrestrictedOnHazardOrAmbiguity) {
    // Ambiguous identity between two MOTU units -> BlockAll
    Discovery::DeviceIdentityEvidence ambiguousMotu{};
    ambiguousMotu.rootVendorId = kMotuVendorId;
    ambiguousMotu.rootModelId = 0U;
    ambiguousMotu.units.push_back(Discovery::UnitIdentityEvidence{
        .unitDirectoryOffset = 4,
        .specifierId = kMotuVendorId,
        .version = kMotu828mk2SwVersion,
    });
    ambiguousMotu.units.push_back(Discovery::UnitIdentityEvidence{
        .unitDirectoryOffset = 8,
        .specifierId = kMotuVendorId,
        .version = kMotuUltraliteSwVersion,
    });
    const auto resolution = AudioDeviceCatalog::Resolve(ambiguousMotu);
    ASSERT_FALSE(resolution.has_value());
    EXPECT_EQ(AudioDeviceCatalog::CommandFilterFor(resolution.error()),
              Discovery::AvcCommandFilterId::BlockAll);
}

TEST(AudioDeviceCatalog, StaticAudioEndpointPlanCarriesUnitVersion) {
    const auto device = MakeDevice(0x00130E'0400000000ULL, kFocusriteVendorId,
                                   kSPro24DspModelId,
                                   {{.offset = 5,
                                     .specifierId = kFocusriteVendorId,
                                     .version = kDiceInterfaceVersion}});
    const auto plan = AudioDeviceCatalog::Resolve(device.identity);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->unitVersion, kDiceInterfaceVersion);
}

TEST(AudioDeviceCatalog, Preserves64BitDeviceInstanceIdAboveUint32Max) {
    constexpr uint64_t kLargeInstanceId = 0x1'0000'0005ULL;
    static_assert(kLargeInstanceId > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()));

    auto device = MakeDevice(0x00130E'0400000000ULL, kFocusriteVendorId,
                             kSPro24DspModelId,
                             {{.offset = 5,
                               .specifierId = kFocusriteVendorId,
                               .version = kDiceInterfaceVersion}});
    device.instanceId = Discovery::DeviceInstanceId{kLargeInstanceId};

    // 1. DeviceRecord resolution
    const auto planFromRecord = AudioDeviceCatalog::Resolve(device);
    ASSERT_TRUE(planFromRecord.has_value());
    EXPECT_EQ(planFromRecord->unit.device.value, kLargeInstanceId);

    // 2. Per-unit resolution with DeviceRecord
    const auto planPerUnitRecord = AudioDeviceCatalog::Resolve(device, device.identity.units.front());
    ASSERT_TRUE(planPerUnitRecord.has_value());
    EXPECT_EQ(planPerUnitRecord->unit.device.value, kLargeInstanceId);

    // 3. Per-unit resolution with DeviceIdentityEvidence and explicit DeviceInstanceId
    const auto planPerUnitEvidence = AudioDeviceCatalog::Resolve(
        device.identity, device.identity.units.front(), Discovery::DeviceInstanceId{kLargeInstanceId});
    ASSERT_TRUE(planPerUnitEvidence.has_value());
    EXPECT_EQ(planPerUnitEvidence->unit.device.value, kLargeInstanceId);
}

} // namespace
