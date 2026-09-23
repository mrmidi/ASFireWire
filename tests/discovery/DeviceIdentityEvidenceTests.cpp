// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DeviceIdentityEvidenceTests.cpp
//
// Pins the two properties DeviceRecord::identity exists to provide, both of
// which the flat vendorId/modelId/unitSpecId/unitSwVersion fields cannot express.
// These are not hypothetical shapes: MOTU publishes root model_id 0, and the TC
// Applied Technologies devices publish more than one unit directory.

#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "DeviceProfiles/Audio/ResolvedDevicePolicy.hpp"
#include "Discovery/DeviceRegistry.hpp"
#include "Discovery/DiscoveryTypes.hpp"

#include <gtest/gtest.h>

namespace {

using ASFW::Discovery::CfgKey;
using ASFW::Discovery::ConfigROM;
using ASFW::Discovery::DeviceRegistry;
using ASFW::Discovery::LinkPolicy;
using ASFW::Discovery::RomEntry;
using ASFW::Discovery::UnitDirectory;

constexpr uint64_t kGuid = 0x0001F2'0400112233ULL;

[[nodiscard]] ConfigROM MakeRom() {
    ConfigROM rom{};
    rom.bib.guid = kGuid;
    rom.gen = ASFW::Discovery::Generation{1};
    rom.nodeId = 1;
    return rom;
}

[[nodiscard]] ConfigROM MakeSupportedMotuRom() {
    ConfigROM rom = MakeRom();
    rom.nodeId = 2;
    rom.rootDirMinimal.push_back(RomEntry{.key = CfgKey::VendorId,
                                          .value = ASFW::DeviceProfiles::Audio::kMotuVendorId});
    rom.rootDirMinimal.push_back(RomEntry{.key = CfgKey::ModelId, .value = 0});
    UnitDirectory unit{};
    unit.offsetQuadlets = 4;
    unit.unitSpecId = ASFW::DeviceProfiles::Audio::kMotuVendorId;
    unit.unitSwVersion = ASFW::DeviceProfiles::Audio::kMotu828mk2SwVersion;
    rom.unitDirectories.push_back(unit);
    return rom;
}

// ---------------------------------------------------------------------------
// A zero is not an absence
// ---------------------------------------------------------------------------

TEST(DeviceIdentityEvidence, ModelIdZeroIsDistinguishableFromNoModelIdAtAll) {
    DeviceRegistry registry{};

    // MOTU's shape: the root directory carries a model_id key whose value is 0.
    ConfigROM published = MakeRom();
    published.rootDirMinimal.push_back(RomEntry{.key = CfgKey::VendorId, .value = 0x0001F2});
    published.rootDirMinimal.push_back(RomEntry{.key = CfgKey::ModelId, .value = 0});
    const auto withZero = registry.UpsertFromROM(published, LinkPolicy{});

    ASSERT_TRUE(withZero.identity.rootModelId.has_value());
    EXPECT_EQ(*withZero.identity.rootModelId, 0U);

    // A device that publishes no model_id key at all.
    ConfigROM absent = MakeRom();
    absent.bib.guid = kGuid + 1;
    absent.rootDirMinimal.push_back(RomEntry{.key = CfgKey::VendorId, .value = 0x0001F2});
    const auto withoutKey = registry.UpsertFromROM(absent, LinkPolicy{});

    EXPECT_FALSE(withoutKey.identity.rootModelId.has_value());

    // The flat field collapses both cases to the same value, which is exactly
    // why DeviceProtocolFactory::Create cannot match MOTU on model_id and has
    // to reach into the unit directory instead.
    EXPECT_EQ(withZero.modelId, withoutKey.modelId);
    EXPECT_EQ(withZero.modelId, 0U);
}

TEST(DeviceIdentityEvidence, AbsentVendorIdIsNotReportedAsZero) {
    DeviceRegistry registry{};
    ConfigROM rom = MakeRom();
    // No VendorId entry at all.
    rom.rootDirMinimal.push_back(RomEntry{.key = CfgKey::ModelId, .value = 0x000012});

    const auto record = registry.UpsertFromROM(rom, LinkPolicy{});
    EXPECT_FALSE(record.identity.rootVendorId.has_value());
    EXPECT_EQ(record.RootVendorIdOrZero(), 0U);
    ASSERT_TRUE(record.identity.rootModelId.has_value());
    EXPECT_EQ(record.RootModelIdOrZero(), 0x000012U);
}

// ---------------------------------------------------------------------------
// A device has units, plural
// ---------------------------------------------------------------------------

TEST(DeviceIdentityEvidence, EveryUnitDirectoryIsKeptSeparately) {
    DeviceRegistry registry{};
    ConfigROM rom = MakeRom();

    UnitDirectory audio{};
    audio.offsetQuadlets = 4;
    audio.unitSpecId = 0x00A02D;
    audio.unitSwVersion = 0x010001;
    rom.unitDirectories.push_back(audio);

    UnitDirectory midi{};
    midi.offsetQuadlets = 9;
    midi.unitSpecId = 0x00A02D;
    midi.unitSwVersion = 0x014001;
    rom.unitDirectories.push_back(midi);

    const auto record = registry.UpsertFromROM(rom, LinkPolicy{});

    ASSERT_EQ(record.identity.units.size(), 2U);
    EXPECT_EQ(record.identity.units[0].unitDirectoryOffset, 4U);
    EXPECT_EQ(record.identity.units[1].unitDirectoryOffset, 9U);
    ASSERT_TRUE(record.identity.units[0].version.has_value());
    ASSERT_TRUE(record.identity.units[1].version.has_value());
    EXPECT_EQ(*record.identity.units[0].version, 0x010001U);
    EXPECT_EQ(*record.identity.units[1].version, 0x014001U);
}

// The flat pair is not merely lossy — it can take its two halves from different
// unit directories. This test documents the shim's behaviour so that deleting
// the flat fields later is a visible change rather than a silent one.
TEST(DeviceIdentityEvidence, FlatPairCanTakeItsHalvesFromDifferentUnits) {
    DeviceRegistry registry{};
    ConfigROM rom = MakeRom();

    UnitDirectory first{};
    first.offsetQuadlets = 4;
    first.unitSpecId = 0x00A02D;
    first.unitSwVersion = 0;  // absent
    rom.unitDirectories.push_back(first);

    UnitDirectory second{};
    second.offsetQuadlets = 9;
    second.unitSpecId = 0;  // absent
    second.unitSwVersion = 0x014001;
    rom.unitDirectories.push_back(second);

    const auto record = registry.UpsertFromROM(rom, LinkPolicy{});

    // The shim reports a specId/version pair that no single unit published.
    ASSERT_TRUE(record.unitSpecId.has_value());
    ASSERT_TRUE(record.unitSwVersion.has_value());
    EXPECT_EQ(*record.unitSpecId, 0x00A02DU);
    EXPECT_EQ(*record.unitSwVersion, 0x014001U);

    // The evidence keeps them where they came from.
    ASSERT_EQ(record.identity.units.size(), 2U);
    EXPECT_TRUE(record.identity.units[0].specifierId.has_value());
    EXPECT_FALSE(record.identity.units[0].version.has_value());
    EXPECT_FALSE(record.identity.units[1].specifierId.has_value());
    EXPECT_TRUE(record.identity.units[1].version.has_value());
}

TEST(DeviceIdentityEvidence, FindUnitBySpecifierReturnsTheFirstMatchOrNull) {
    DeviceRegistry registry{};
    ConfigROM rom = MakeRom();

    UnitDirectory sbp2{};
    sbp2.offsetQuadlets = 4;
    sbp2.unitSpecId = 0x00609E;
    rom.unitDirectories.push_back(sbp2);

    UnitDirectory audio{};
    audio.offsetQuadlets = 9;
    audio.unitSpecId = 0x00A02D;
    rom.unitDirectories.push_back(audio);

    const auto record = registry.UpsertFromROM(rom, LinkPolicy{});

    const auto* found = record.FindUnitBySpecifier(0x00A02D);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->unitDirectoryOffset, 9U);
    EXPECT_EQ(record.FindUnitBySpecifier(0xABCDEF), nullptr);
}

// ---------------------------------------------------------------------------
// The GUID is evidence
// ---------------------------------------------------------------------------

TEST(DeviceIdentityEvidence, ObservedGuidAndOuiComeFromTheBusInfoBlock) {
    DeviceRegistry registry{};
    const auto record = registry.UpsertFromROM(MakeRom(), LinkPolicy{});

    EXPECT_EQ(record.identity.observedGuid, kGuid);
    EXPECT_EQ(record.ObservedGuid(), kGuid);
    // Top 24 bits of the GUID are the node vendor OUI.
    EXPECT_EQ(record.identity.nodeVendorOui, 0x0001F2U);
}

// ---------------------------------------------------------------------------
// Quarantine and route liveliness lifecycle tests
// ---------------------------------------------------------------------------

TEST(DeviceIdentityEvidence, QuarantinedDeviceDoesNotExposeLiveRoute) {
    DeviceRegistry registry{};
    ConfigROM rom = MakeRom();
    rom.nodeId = 2;
    // Ambiguous curated units: one MOTU 828mk2, one MOTU UltraLite
    rom.rootDirMinimal.push_back(RomEntry{.key = CfgKey::VendorId, .value = ASFW::DeviceProfiles::Audio::kMotuVendorId});
    rom.rootDirMinimal.push_back(RomEntry{.key = CfgKey::ModelId, .value = 0});

    UnitDirectory motu828{};
    motu828.offsetQuadlets = 4;
    motu828.unitSpecId = ASFW::DeviceProfiles::Audio::kMotuVendorId;
    motu828.unitSwVersion = ASFW::DeviceProfiles::Audio::kMotu828mk2SwVersion;
    rom.unitDirectories.push_back(motu828);

    UnitDirectory motuUltralite{};
    motuUltralite.offsetQuadlets = 8;
    motuUltralite.unitSpecId = ASFW::DeviceProfiles::Audio::kMotuVendorId;
    motuUltralite.unitSwVersion = ASFW::DeviceProfiles::Audio::kMotuUltraliteSwVersion;
    rom.unitDirectories.push_back(motuUltralite);

    const auto record = registry.UpsertFromROM(rom, LinkPolicy{});
    EXPECT_EQ(record.state, ASFW::Discovery::LifeState::Quarantined);
    EXPECT_EQ(record.quarantineReason, ASFW::Discovery::QuarantineReason::AmbiguousIdentity);
    EXPECT_FALSE(record.isAudioCandidate);
    EXPECT_FALSE(registry.CurrentRoute(record.guid).has_value());
    EXPECT_TRUE(registry.LiveDevices(record.gen).empty());
    EXPECT_EQ(record.avcCommandFilter, ASFW::Discovery::AvcCommandFilterId::BlockAll);
}

TEST(DeviceIdentityEvidence, ReResolutionClearsQuarantineAndRestoresLiveRoute) {
    DeviceRegistry registry{};
    ConfigROM rejectedRom = MakeRom();
    rejectedRom.nodeId = 2;
    rejectedRom.rootDirMinimal.push_back(RomEntry{.key = CfgKey::VendorId, .value = ASFW::DeviceProfiles::Audio::kMotuVendorId});
    rejectedRom.rootDirMinimal.push_back(RomEntry{.key = CfgKey::ModelId, .value = 0});

    UnitDirectory u1{};
    u1.offsetQuadlets = 4;
    u1.unitSpecId = ASFW::DeviceProfiles::Audio::kMotuVendorId;
    u1.unitSwVersion = ASFW::DeviceProfiles::Audio::kMotu828mk2SwVersion;
    rejectedRom.unitDirectories.push_back(u1);

    UnitDirectory u2{};
    u2.offsetQuadlets = 8;
    u2.unitSpecId = ASFW::DeviceProfiles::Audio::kMotuVendorId;
    u2.unitSwVersion = ASFW::DeviceProfiles::Audio::kMotuUltraliteSwVersion;
    rejectedRom.unitDirectories.push_back(u2);

    const auto rejected = registry.UpsertFromROM(rejectedRom, LinkPolicy{});
    ASSERT_EQ(rejected.state, ASFW::Discovery::LifeState::Quarantined);
    ASSERT_EQ(rejected.quarantineReason, ASFW::Discovery::QuarantineReason::AmbiguousIdentity);
    ASSERT_FALSE(registry.CurrentRoute(rejected.guid).has_value());

    // Re-upsert with clean, single valid unit (same GUID)
    ConfigROM validRom = MakeRom();
    validRom.nodeId = 2;
    validRom.rootDirMinimal.push_back(RomEntry{.key = CfgKey::VendorId, .value = ASFW::DeviceProfiles::Audio::kMotuVendorId});
    validRom.rootDirMinimal.push_back(RomEntry{.key = CfgKey::ModelId, .value = 0});
    UnitDirectory validUnit{};
    validUnit.offsetQuadlets = 4;
    validUnit.unitSpecId = ASFW::DeviceProfiles::Audio::kMotuVendorId;
    validUnit.unitSwVersion = ASFW::DeviceProfiles::Audio::kMotu828mk2SwVersion;
    validRom.unitDirectories.push_back(validUnit);

    const auto valid = registry.UpsertFromROM(validRom, LinkPolicy{});
    EXPECT_EQ(valid.state, ASFW::Discovery::LifeState::Identified);
    EXPECT_EQ(valid.quarantineReason, ASFW::Discovery::QuarantineReason::None);
    EXPECT_TRUE(valid.isAudioCandidate);
    EXPECT_TRUE(registry.CurrentRoute(valid.guid).has_value());
    EXPECT_EQ(registry.LiveDevices(valid.gen).size(), 1U);
}

TEST(DeviceIdentityEvidence, ValidToQuarantinedReResolutionDropsLiveRoute) {
    DeviceRegistry registry{};
    ConfigROM validRom = MakeRom();
    validRom.nodeId = 2;
    validRom.rootDirMinimal.push_back(RomEntry{.key = CfgKey::VendorId, .value = ASFW::DeviceProfiles::Audio::kMotuVendorId});
    validRom.rootDirMinimal.push_back(RomEntry{.key = CfgKey::ModelId, .value = 0});
    UnitDirectory validUnit{};
    validUnit.offsetQuadlets = 4;
    validUnit.unitSpecId = ASFW::DeviceProfiles::Audio::kMotuVendorId;
    validUnit.unitSwVersion = ASFW::DeviceProfiles::Audio::kMotu828mk2SwVersion;
    validRom.unitDirectories.push_back(validUnit);

    const auto valid = registry.UpsertFromROM(validRom, LinkPolicy{});
    ASSERT_EQ(valid.state, ASFW::Discovery::LifeState::Identified);
    ASSERT_EQ(valid.quarantineReason, ASFW::Discovery::QuarantineReason::None);
    ASSERT_TRUE(registry.CurrentRoute(valid.guid).has_value());

    // Now subsequent scan returns conflicting/ambiguous evidence
    ConfigROM rejectedRom = MakeRom();
    rejectedRom.nodeId = 2;
    rejectedRom.rootDirMinimal.push_back(RomEntry{.key = CfgKey::VendorId, .value = ASFW::DeviceProfiles::Audio::kMotuVendorId});
    rejectedRom.rootDirMinimal.push_back(RomEntry{.key = CfgKey::ModelId, .value = 0});
    UnitDirectory u1{};
    u1.offsetQuadlets = 4;
    u1.unitSpecId = ASFW::DeviceProfiles::Audio::kMotuVendorId;
    u1.unitSwVersion = ASFW::DeviceProfiles::Audio::kMotu828mk2SwVersion;
    rejectedRom.unitDirectories.push_back(u1);

    UnitDirectory u2{};
    u2.offsetQuadlets = 8;
    u2.unitSpecId = ASFW::DeviceProfiles::Audio::kMotuVendorId;
    u2.unitSwVersion = ASFW::DeviceProfiles::Audio::kMotuUltraliteSwVersion;
    rejectedRom.unitDirectories.push_back(u2);

    const auto rejected = registry.UpsertFromROM(rejectedRom, LinkPolicy{});
    EXPECT_EQ(rejected.state, ASFW::Discovery::LifeState::Quarantined);
    EXPECT_EQ(rejected.quarantineReason, ASFW::Discovery::QuarantineReason::AmbiguousIdentity);
    EXPECT_FALSE(rejected.isAudioCandidate);
    EXPECT_FALSE(registry.CurrentRoute(rejected.guid).has_value());
    EXPECT_TRUE(registry.LiveDevices(rejected.gen).empty());
}

TEST(DeviceIdentityEvidence, ResolvedPolicyIsBoundToTheCurrentRoute) {
    DeviceRegistry registry{};
    const auto record = registry.UpsertFromROM(MakeSupportedMotuRom(), LinkPolicy{});
    const auto* policy = ASFW::DeviceProfiles::Audio::CurrentAudioPolicy(record);
    ASSERT_NE(policy, nullptr);
    EXPECT_EQ(policy->plan.unit.device, record.instanceId);
    EXPECT_EQ(policy->route.guid, record.guid);
    EXPECT_EQ(policy->route.generation, record.gen);
    EXPECT_EQ(policy->route.nodeId, record.nodeId);
    EXPECT_TRUE(registry.IsCurrent(policy->route));

    auto altered = record;
    altered.nodeId = 3;
    EXPECT_EQ(ASFW::DeviceProfiles::Audio::CurrentAudioPolicy(altered), nullptr);
}

TEST(DeviceIdentityEvidence, BusResetInvalidatesPolicyUntilRomRebind) {
    DeviceRegistry registry{};
    const auto first = registry.UpsertFromROM(MakeSupportedMotuRom(), LinkPolicy{});
    const auto* firstPolicy = ASFW::DeviceProfiles::Audio::CurrentAudioPolicy(first);
    ASSERT_NE(firstPolicy, nullptr);
    const auto oldRoute = firstPolicy->route;

    registry.InvalidateLiveMappingsForBusReset();
    const auto invalidated = registry.SnapshotByGuid(first.guid);
    ASSERT_TRUE(invalidated.has_value());
    EXPECT_EQ(ASFW::DeviceProfiles::Audio::CurrentAudioPolicy(*invalidated), nullptr);
    EXPECT_EQ(invalidated->avcCommandFilter, ASFW::Discovery::AvcCommandFilterId::BlockAll);
    EXPECT_FALSE(registry.IsCurrent(oldRoute));

    auto nextRom = MakeSupportedMotuRom();
    nextRom.gen = ASFW::Discovery::Generation{2};
    nextRom.nodeId = 3;
    const auto rebound = registry.UpsertFromROM(nextRom, LinkPolicy{});
    const auto* reboundPolicy = ASFW::DeviceProfiles::Audio::CurrentAudioPolicy(rebound);
    ASSERT_NE(reboundPolicy, nullptr);
    EXPECT_NE(reboundPolicy->route, oldRoute);
    EXPECT_TRUE(registry.IsCurrent(reboundPolicy->route));
    EXPECT_FALSE(registry.IsCurrent(oldRoute));
}

TEST(DeviceIdentityEvidence, DeviceLossClearsPolicyAndRejectsOldRoute) {
    DeviceRegistry registry{};
    const auto first = registry.UpsertFromROM(MakeSupportedMotuRom(), LinkPolicy{});
    const auto* policy = ASFW::DeviceProfiles::Audio::CurrentAudioPolicy(first);
    ASSERT_NE(policy, nullptr);
    const auto route = policy->route;

    registry.MarkLost(first.gen, static_cast<uint8_t>(first.nodeId));
    const auto lost = registry.SnapshotByGuid(first.guid);
    ASSERT_TRUE(lost.has_value());
    EXPECT_EQ(ASFW::DeviceProfiles::Audio::CurrentAudioPolicy(*lost), nullptr);
    EXPECT_EQ(lost->avcCommandFilter, ASFW::Discovery::AvcCommandFilterId::BlockAll);
    EXPECT_FALSE(registry.IsCurrent(route));
}

} // namespace
