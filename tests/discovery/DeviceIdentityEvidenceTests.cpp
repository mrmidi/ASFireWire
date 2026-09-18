// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DeviceIdentityEvidenceTests.cpp
//
// Pins the two properties DeviceRecord::identity exists to provide, both of
// which the flat vendorId/modelId/unitSpecId/unitSwVersion fields cannot express.
// These are not hypothetical shapes: MOTU publishes root model_id 0, and the TC
// Applied Technologies devices publish more than one unit directory.

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

} // namespace
