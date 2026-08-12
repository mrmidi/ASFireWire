#include <gtest/gtest.h>

#include "ASFWDriver/DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "ASFWDriver/DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "ASFWDriver/Discovery/DeviceRegistry.hpp"

#include <array>

namespace {

using namespace ASFW::Discovery;
using namespace ASFW::DeviceProfiles;
using namespace ASFW::DeviceProfiles::Audio;

ConfigROM MakeRom(uint64_t observedGuid, uint32_t generation, uint16_t node,
                  uint32_t rootVendor = 0x001122,
                  uint32_t rootModel = 0x334455,
                  uint32_t unitSpecifier = 0x00A02D,
                  uint32_t unitVersion = 0x010001,
                  uint32_t unitOffset = 6) {
    ConfigROM rom{};
    rom.bib.guid = observedGuid;
    rom.bib.busInfoLength = 4;
    rom.gen = Generation{generation};
    rom.nodeId = node;
    rom.rootDirMinimal = {
        RomEntry{CfgKey::VendorId, rootVendor, 0, 0},
        RomEntry{CfgKey::ModelId, rootModel, 0, 0},
    };
    UnitDirectory unit{};
    unit.offsetQuadlets = unitOffset;
    unit.vendorId = rootVendor + 1U;
    unit.modelId = rootModel + 1U;
    unit.unitSpecId = unitSpecifier;
    unit.unitSwVersion = unitVersion;
    unit.vendorName = "Unit Vendor";
    unit.modelName = "Unit Model";
    rom.unitDirectories.push_back(std::move(unit));
    return rom;
}

const UnitIdentityEvidence& OnlyUnit(const DeviceRecord& record) {
    EXPECT_EQ(record.identity.units.size(), 1U);
    return record.identity.units.front();
}

TEST(DeviceRuntimeIdentityTests, PreservesIndependentRootAndUnitEvidence) {
    DeviceRegistry registry;
    auto rom = MakeRom(0x0011220304050607ULL, 1, 2);
    rom.vendorName = "Root Vendor";
    rom.modelName = "Root Model";

    const auto record = registry.UpsertFromROM(rom, {});

    EXPECT_TRUE(record.instanceId);
    EXPECT_EQ(record.ObservedGuid(), rom.bib.guid);
    EXPECT_EQ(record.identity.rootVendorId, 0x001122U);
    EXPECT_EQ(record.identity.rootModelId, 0x334455U);
    EXPECT_EQ(record.identity.rootVendorName, "Root Vendor");
    EXPECT_EQ(record.identity.rootModelName, "Root Model");
    ASSERT_EQ(record.identity.units.size(), 1U);
    EXPECT_EQ(record.identity.units[0].vendorId, 0x001123U);
    EXPECT_EQ(record.identity.units[0].modelId, 0x334456U);
    EXPECT_EQ(record.identity.units[0].unitDirectoryOffset, 6U);
    EXPECT_EQ(record.identity.units[0].vendorName, "Unit Vendor");
    EXPECT_EQ(record.identity.units[0].modelName, "Unit Model");
}

TEST(DeviceRuntimeIdentityTests, ReconcileQuarantinesEveryDuplicateWithoutFakeGuid) {
    DeviceRegistry registry;
    constexpr uint64_t duplicate = 0x000D6C0404000002ULL;
    auto first = MakeRom(duplicate, 1, 2);
    auto second = MakeRom(duplicate, 1, 3);
    const std::array observations{
        DeviceObservation{&first, {}},
        DeviceObservation{&second, {}},
    };

    const auto records = registry.ReconcileGeneration(Generation{1}, observations);

    ASSERT_EQ(records.size(), 2U);
    EXPECT_NE(records[0].instanceId, records[1].instanceId);
    for (const auto& record : records) {
        EXPECT_EQ(record.ObservedGuid(), duplicate);
        EXPECT_EQ(record.quarantineReason, QuarantineReason::DuplicateObservedGuid);
        EXPECT_FALSE(registry.CurrentRoute(record.instanceId).has_value());
    }
    EXPECT_EQ(registry.FindInstancesByObservedGuid(duplicate).size(), 2U);
}

TEST(DeviceRuntimeIdentityTests, ZeroGuidRemainsDiagnosticAndHasNoRoute) {
    DeviceRegistry registry;
    auto rom = MakeRom(0, 1, 2);
    const std::array observations{DeviceObservation{&rom, {}}};

    const auto records = registry.ReconcileGeneration(Generation{1}, observations);

    ASSERT_EQ(records.size(), 1U);
    EXPECT_TRUE(records[0].instanceId);
    EXPECT_EQ(records[0].ObservedGuid(), 0U);
    EXPECT_EQ(records[0].quarantineReason, QuarantineReason::ZeroObservedGuid);
    EXPECT_FALSE(registry.CurrentRoute(records[0].instanceId).has_value());
}

TEST(DeviceRuntimeIdentityTests, UniqueResetPreservesInstanceButReplugDoesNot) {
    DeviceRegistry registry;
    auto first = MakeRom(0x0011220304050607ULL, 1, 2);
    const auto firstRecord = registry.UpsertFromROM(first, {});
    const auto firstRoute = registry.CurrentRoute(firstRecord.instanceId);
    ASSERT_TRUE(firstRoute.has_value());

    registry.InvalidateLiveMappingsForBusReset();
    auto rebound = MakeRom(first.bib.guid, 2, 7);
    const std::array observations{DeviceObservation{&rebound, {}}};
    const auto reboundRecords = registry.ReconcileGeneration(Generation{2}, observations);
    ASSERT_EQ(reboundRecords.size(), 1U);
    EXPECT_EQ(reboundRecords[0].instanceId, firstRecord.instanceId);
    const auto reboundRoute = registry.CurrentRoute(firstRecord.instanceId);
    ASSERT_TRUE(reboundRoute.has_value());
    EXPECT_NE(firstRoute->routeEpoch, reboundRoute->routeEpoch);
    EXPECT_FALSE(registry.IsCurrent(*firstRoute));

    registry.RetireDevice(firstRecord.instanceId);
    auto replug = MakeRom(first.bib.guid, 3, 4);
    const auto replugRecord = registry.UpsertFromROM(replug, {});
    EXPECT_NE(replugRecord.instanceId, firstRecord.instanceId);
}

TEST(DeviceRuntimeIdentityTests, CollisionGroupsAreRecreatedOnEveryGeneration) {
    DeviceRegistry registry;
    constexpr uint64_t duplicate = 0x000D6C0404000002ULL;
    auto a1 = MakeRom(duplicate, 1, 2);
    auto b1 = MakeRom(duplicate, 1, 3);
    const std::array firstObservations{DeviceObservation{&a1, {}},
                                       DeviceObservation{&b1, {}}};
    const auto first = registry.ReconcileGeneration(Generation{1}, firstObservations);
    ASSERT_EQ(first.size(), 2U);

    registry.InvalidateLiveMappingsForBusReset();
    auto a2 = MakeRom(duplicate, 2, 3);
    auto b2 = MakeRom(duplicate, 2, 2);
    const std::array secondObservations{DeviceObservation{&a2, {}},
                                        DeviceObservation{&b2, {}}};
    const auto second = registry.ReconcileGeneration(Generation{2}, secondObservations);
    ASSERT_EQ(second.size(), 2U);
    EXPECT_NE(second[0].instanceId, first[0].instanceId);
    EXPECT_NE(second[0].instanceId, first[1].instanceId);
    EXPECT_NE(second[1].instanceId, first[0].instanceId);
    EXPECT_NE(second[1].instanceId, first[1].instanceId);
}

TEST(AudioDeviceCatalogTests, HazardousMAudioPersonasFailBeforeGenericAvcFallback) {
    for (const uint32_t model : {0x00010070U, 0x00010071U, 0x00010091U}) {
        DeviceRegistry registry;
        const auto rom = MakeRom(0x000D6C0000000001ULL, 1, 2, 0x000D6C, model);
        const auto record = registry.UpsertFromROM(rom, {});
        const auto result = AudioDeviceCatalog::Resolve(record, OnlyUnit(record));
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), CatalogResolutionError::HazardousIdentity);
        EXPECT_TRUE(AudioDeviceCatalog::MatchAnySafetyRule(record.identity).has_value());
    }
}

TEST(AudioDeviceCatalogTests, RmeShapedSharedModelResolvesByUnitVersion) {
    DeviceRegistry registry;
    auto rom = MakeRom(0x000A350000000001ULL, 1, 2, 0x000A35, 0x101800,
                       0x000A35, 2);
    const auto record = registry.UpsertFromROM(rom, {});

    constexpr std::array definitions{
        AudioDeviceDefinition{
            .id = static_cast<DeviceDefinitionId>(1001),
            .clauses = {IdentityMatchClause{
                            .rootVendorId = MaskedValue32{0x000A35},
                            .rootModelId = MaskedValue32{0x101800},
                            .unitSpecifierId = MaskedValue32{0x000A35},
                            .unitVersion = MaskedValue32{1}},
                        IdentityMatchClause{}},
            .clauseCount = 1,
            .family = AudioFamilyProviderId::OXFW,
            .probePolicy = ProbePolicyId::OxfwAvc,
            .profileBuilder = ProfileBuilderId::ApogeeDuet,
            .support = SupportDisposition::Supported,
            .vendorName = "RME",
            .modelName = "Variant 1"},
        AudioDeviceDefinition{
            .id = static_cast<DeviceDefinitionId>(1002),
            .clauses = {IdentityMatchClause{
                            .rootVendorId = MaskedValue32{0x000A35},
                            .rootModelId = MaskedValue32{0x101800},
                            .unitSpecifierId = MaskedValue32{0x000A35},
                            .unitVersion = MaskedValue32{2}},
                        IdentityMatchClause{}},
            .clauseCount = 1,
            .family = AudioFamilyProviderId::OXFW,
            .probePolicy = ProbePolicyId::OxfwAvc,
            .profileBuilder = ProfileBuilderId::ApogeeDuet,
            .support = SupportDisposition::Supported,
            .vendorName = "RME",
            .modelName = "Variant 2"},
    };

    const auto result = AudioDeviceCatalog::ResolveWithDefinitions(
        record, OnlyUnit(record), definitions, {}, false);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->candidates.size(), 1U);
    EXPECT_EQ(result->candidates[0], static_cast<DeviceDefinitionId>(1002));
}

TEST(AudioDeviceCatalogTests, GuidMatchDoesNotOverwriteConflictingRootEvidence) {
    DeviceRegistry registry;
    constexpr uint64_t guid =
        (static_cast<uint64_t>(kFocusriteVendorId) << 40U) |
        (static_cast<uint64_t>(kSPro24ModelId) << 22U) | 7U;
    auto rom = MakeRom(guid, 1, 2, 0x00ABC0, 0x000077,
                       kFocusriteVendorId, 0x000001);
    const auto record = registry.UpsertFromROM(rom, {});

    const auto result = AudioDeviceCatalog::Resolve(record, OnlyUnit(record));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->candidates.front(), DeviceDefinitionId::FocusriteSPro24);
    EXPECT_EQ(result->provenance.front().clauseIndex, 1U);
    EXPECT_EQ(record.identity.rootVendorId, 0x00ABC0U);
    EXPECT_EQ(record.identity.rootModelId, 0x000077U);
}

TEST(AudioDeviceCatalogTests, GenericFallbackRequiresAvcSpecifierAndVersionPair) {
    DeviceRegistry registry;
    auto diceRom = MakeRom(0x0011220304050607ULL, 1, 2, 0x001122,
                           0x334455, 0x001122, 0x000001);
    const auto dice = registry.UpsertFromROM(diceRom, {});
    const auto diceResult = AudioDeviceCatalog::ResolveWithDefinitions(
        dice, OnlyUnit(dice), {}, {}, true);
    ASSERT_FALSE(diceResult.has_value());
    EXPECT_EQ(diceResult.error(), CatalogResolutionError::NoMatch);

    DeviceRegistry avcRegistry;
    auto avcRom = MakeRom(0x0011220304050608ULL, 1, 3, 0x001122,
                          0x334455, 0x00A02D, 0x010001);
    const auto avc = avcRegistry.UpsertFromROM(avcRom, {});
    const auto avcResult = AudioDeviceCatalog::ResolveWithDefinitions(
        avc, OnlyUnit(avc), {}, {}, true);
    ASSERT_TRUE(avcResult.has_value());
    EXPECT_EQ(avcResult->family, AudioFamilyProviderId::GenericAvc);
}

TEST(AudioDeviceCatalogTests, SaffireShapedSharedModelUsesExactCaseSensitiveDescriptor) {
    DeviceRegistry registry;
    auto rom = MakeRom(0x00130E0000000001ULL, 1, 2, 0x00130E,
                       0x000007, 0x00130E, 0x000001);
    rom.unitDirectories[0].modelName = "Saffire PRO 24";
    const auto record = registry.UpsertFromROM(rom, {});

    const std::array definitions{
        AudioDeviceDefinition{
            .id = static_cast<DeviceDefinitionId>(1101),
            .clauses = {IdentityMatchClause{
                .rootVendorId = MaskedValue32{0x00130E},
                .rootModelId = MaskedValue32{0x000007},
                .unitModelName = "Saffire Pro 24"}},
            .clauseCount = 1,
            .family = AudioFamilyProviderId::DICE,
            .probePolicy = ProbePolicyId::DiceTcat,
            .profileBuilder = ProfileBuilderId::FocusriteSPro24,
            .support = SupportDisposition::Supported},
        AudioDeviceDefinition{
            .id = static_cast<DeviceDefinitionId>(1102),
            .clauses = {IdentityMatchClause{
                .rootVendorId = MaskedValue32{0x00130E},
                .rootModelId = MaskedValue32{0x000007},
                .unitModelName = "Saffire PRO 24"}},
            .clauseCount = 1,
            .family = AudioFamilyProviderId::DICE,
            .probePolicy = ProbePolicyId::DiceTcat,
            .profileBuilder = ProfileBuilderId::FocusriteSPro24,
            .support = SupportDisposition::Supported},
    };

    const auto result = AudioDeviceCatalog::ResolveWithDefinitions(
        record, OnlyUnit(record), definitions, {}, false);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->candidates.size(), 1U);
    EXPECT_EQ(result->candidates.front(), static_cast<DeviceDefinitionId>(1102));
}

TEST(AudioDeviceCatalogTests, IncompatibleOverlapFailsClosed) {
    DeviceRegistry registry;
    const auto rom = MakeRom(0x0011220304050607ULL, 1, 2);
    const auto record = registry.UpsertFromROM(rom, {});
    const auto clause = IdentityMatchClause{
        .rootVendorId = MaskedValue32{0x001122},
        .rootModelId = MaskedValue32{0x334455},
    };
    const std::array definitions{
        AudioDeviceDefinition{.id = static_cast<DeviceDefinitionId>(2001),
                              .clauses = {clause, {}}, .clauseCount = 1,
                              .family = AudioFamilyProviderId::BeBoB,
                              .probePolicy = ProbePolicyId::BeBoBPlug0},
        AudioDeviceDefinition{.id = static_cast<DeviceDefinitionId>(2002),
                              .clauses = {clause, {}}, .clauseCount = 1,
                              .family = AudioFamilyProviderId::DICE,
                              .probePolicy = ProbePolicyId::DiceTcat},
    };

    const auto result = AudioDeviceCatalog::ResolveWithDefinitions(
        record, OnlyUnit(record), definitions, {}, false);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), CatalogResolutionError::AmbiguousIdentity);
}

TEST(AudioDeviceCatalogTests, ExplicitEquivalenceClassProducesCommonPlan) {
    DeviceRegistry registry;
    const auto rom = MakeRom(0x0011220304050607ULL, 1, 2);
    const auto record = registry.UpsertFromROM(rom, {});
    const auto clause = IdentityMatchClause{
        .rootVendorId = MaskedValue32{0x001122},
        .rootModelId = MaskedValue32{0x334455},
    };
    const std::array definitions{
        AudioDeviceDefinition{.id = static_cast<DeviceDefinitionId>(3001),
                              .equivalenceClassId = 42,
                              .clauses = {clause, {}}, .clauseCount = 1,
                              .family = AudioFamilyProviderId::DICE,
                              .probePolicy = ProbePolicyId::DiceTcat,
                              .profileBuilder = ProfileBuilderId::FocusriteSPro14,
                              .commonEquivalenceProfileBuilder = ProfileBuilderId::AlesisMultiMix},
        AudioDeviceDefinition{.id = static_cast<DeviceDefinitionId>(3002),
                              .equivalenceClassId = 42,
                              .clauses = {clause, {}}, .clauseCount = 1,
                              .family = AudioFamilyProviderId::DICE,
                              .probePolicy = ProbePolicyId::DiceTcat,
                              .profileBuilder = ProfileBuilderId::FocusriteSPro24,
                              .commonEquivalenceProfileBuilder = ProfileBuilderId::AlesisMultiMix},
    };

    const auto result = AudioDeviceCatalog::ResolveWithDefinitions(
        record, OnlyUnit(record), definitions, {}, false);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->candidates.size(), 2U);
    EXPECT_EQ(result->equivalenceClassId, 42U);
    EXPECT_EQ(result->profileBuilder, ProfileBuilderId::AlesisMultiMix);
}

TEST(AudioDeviceCatalogTests, ProductionCatalogPassesStructuralValidation) {
    EXPECT_TRUE(AudioDeviceCatalog::Validate().empty());
}

TEST(AudioDeviceCatalogTests, ValidationRejectsInvalidMasksMissingProvidersAndUnsafeOverlap) {
    const auto broadClause = IdentityMatchClause{
        .rootVendorId = MaskedValue32{0x001100, 0x00FFFF00},
    };
    const auto narrowClause = IdentityMatchClause{
        .rootVendorId = MaskedValue32{0x001122},
        .rootModelId = MaskedValue32{0x334455},
    };
    const std::array definitions{
        AudioDeviceDefinition{
            .id = static_cast<DeviceDefinitionId>(4001),
            .clauses = {broadClause},
            .clauseCount = 1,
            .support = SupportDisposition::Supported},
        AudioDeviceDefinition{
            .id = static_cast<DeviceDefinitionId>(4002),
            .clauses = {narrowClause},
            .clauseCount = 1,
            .family = AudioFamilyProviderId::DICE,
            .probePolicy = ProbePolicyId::DiceTcat,
            .profileBuilder = ProfileBuilderId::FocusriteSPro24,
            .support = SupportDisposition::Supported},
        AudioDeviceDefinition{
            .id = static_cast<DeviceDefinitionId>(4003),
            .clauses = {IdentityMatchClause{
                .rootVendorId = MaskedValue32{0x001122, 0}}},
            .clauseCount = 1,
            .family = AudioFamilyProviderId::DICE,
            .probePolicy = ProbePolicyId::DiceTcat,
            .profileBuilder = ProfileBuilderId::FocusriteSPro24,
            .support = SupportDisposition::Supported},
    };

    const auto issues = AudioDeviceCatalog::ValidateDefinitions(definitions);
    EXPECT_GE(issues.size(), 3U);
}

} // namespace
