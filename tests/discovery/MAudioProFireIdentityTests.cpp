// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MAudioProFireIdentityTests.cpp
//
// The M-Audio ProFire 2626 is a TCAT DICE unit whose unit directory carries
// version 0x0100C1 instead of the DICE interface version 0x000001. Before it had
// a catalog row, DeviceRegistry classified it Kind=Unknown, audioCandidate=0 and
// no Core Audio device was published. These tests run the unit's real Config ROM
// (read from hardware with the app's Bus Inspector, 2026-11-16; every block CRC
// checks) through the parser and the registry.

#include "ASFWDriver/ConfigROM/ConfigROMParser.hpp"
#include "DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "DeviceProfiles/Audio/DiceIdentity.hpp"
#include "DeviceProfiles/Audio/ResolvedDevicePolicy.hpp"
#include "Discovery/DeviceRegistry.hpp"
#include "Discovery/DiscoveryTypes.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <span>

namespace {

using namespace ASFW::DeviceProfiles::Audio;
using ASFW::Discovery::CfgKey;
using ASFW::Discovery::ConfigROM;
using ASFW::Discovery::ConfigROMParser;
using ASFW::Discovery::DeviceKind;
using ASFW::Discovery::DeviceRegistry;
using ASFW::Discovery::LinkPolicy;
using ASFW::Discovery::RomEntry;
using ASFW::Discovery::UnitDirectory;

constexpr uint32_t Wire(uint32_t be) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return __builtin_bswap32(be);
    }
    return be;
}

// ProFire 2626, GUID 0x000D6C0404000002. Bus Inspector -> Config ROM -> Raw.
constexpr std::array<uint32_t, 34> kProFire2626Rom{
    Wire(0x04046B24), Wire(0x31333934), Wire(0xE0FF8112), Wire(0x000D6C04),  // q0-q3 BIB
    Wire(0x04000002),                                                        // q4  GUID lo
    Wire(0x0006D09D),                                                        // q5  root dir
    Wire(0x03000D6C), Wire(0x8100000A), Wire(0x17000010), Wire(0x8100000D),
    Wire(0x0C0087C0), Wire(0xD1000001),
    Wire(0x000402BB),                                                        // q12 unit dir
    Wire(0x12000D6C), Wire(0x130100C1), Wire(0x17000010), Wire(0x8100000C),
    Wire(0x0004B8E4), Wire(0x00000000), Wire(0x00000000), Wire(0x4D2D4175),  // "M-Audio"
    Wire(0x64696F00),
    Wire(0x00054EBE), Wire(0x00000000), Wire(0x00000000), Wire(0x50726F46),  // "ProFire 2626"
    Wire(0x69726520), Wire(0x32363236),
    Wire(0x00054EBE), Wire(0x00000000), Wire(0x00000000), Wire(0x50726F46),  // unit model name
    Wire(0x69726520), Wire(0x32363236),
};
constexpr uint32_t kRootDirQuadlet = 5;
constexpr uint32_t kUnitDirQuadlet = 12;
constexpr uint64_t kProFireGuid = 0x000D6C0404000002ULL;

[[nodiscard]] std::optional<uint32_t> Find(const std::vector<RomEntry>& entries, CfgKey key) {
    for (const auto& entry : entries) {
        if (entry.key == key) {
            return entry.value;
        }
    }
    return std::nullopt;
}

// The ROM as discovery hands it to the registry, assembled from parser output.
[[nodiscard]] ConfigROM ParsedProFireRom() {
    const std::span<const uint32_t> rom{kProFire2626Rom};
    ConfigROM out{};
    out.gen = ASFW::Discovery::Generation{10};
    out.nodeId = 0;

    const auto bib = ConfigROMParser::ParseBIB(rom.first(kRootDirQuadlet));
    EXPECT_TRUE(bib.has_value());
    if (bib.has_value()) {
        out.bib = bib->bib;
    }

    const auto root = ConfigROMParser::ParseRootDirectory(
        rom.subspan(kRootDirQuadlet), static_cast<uint32_t>(rom.size() - kRootDirQuadlet));
    EXPECT_TRUE(root.has_value());
    if (root.has_value()) {
        out.rootDirMinimal = *root;
    }

    const auto unit = ConfigROMParser::ParseRootDirectory(
        rom.subspan(kUnitDirQuadlet), static_cast<uint32_t>(rom.size() - kUnitDirQuadlet));
    EXPECT_TRUE(unit.has_value());
    if (unit.has_value()) {
        UnitDirectory dir{};
        dir.offsetQuadlets = kUnitDirQuadlet - kRootDirQuadlet;
        dir.unitSpecId = Find(*unit, CfgKey::Unit_Spec_Id).value_or(0);
        dir.unitSwVersion = Find(*unit, CfgKey::Unit_Sw_Version).value_or(0);
        dir.modelId = Find(*unit, CfgKey::ModelId);
        out.unitDirectories.push_back(dir);
    }
    out.vendorName = "M-Audio";
    out.modelName = "ProFire 2626";
    return out;
}

TEST(MAudioProFireIdentity, TheRecordedRomParsesToTheExpectedIdentity) {
    const auto rom = ParsedProFireRom();
    EXPECT_EQ(rom.bib.guid, kProFireGuid);
    EXPECT_EQ(Find(rom.rootDirMinimal, CfgKey::VendorId), std::optional<uint32_t>{kMAudioVendorId});
    EXPECT_EQ(Find(rom.rootDirMinimal, CfgKey::ModelId),
              std::optional<uint32_t>{kMAudioProFire2626ModelId});
    ASSERT_EQ(rom.unitDirectories.size(), 1U);
    EXPECT_EQ(rom.unitDirectories[0].unitSpecId, kMAudioVendorId);
    EXPECT_EQ(rom.unitDirectories[0].unitSwVersion, kMAudioProFireUnitVersion);
    EXPECT_EQ(rom.unitDirectories[0].modelId, std::optional<uint32_t>{kMAudioProFire2626ModelId});
}

// Why the row exists: the GUID follows the DICE layout, but the unit version
// is not the DICE interface version, so the generic rule cannot place it.
TEST(MAudioProFireIdentity, TheGenericDiceRuleRejectsItOnTheVersionAlone) {
    EXPECT_FALSE(IsDiceIdentity(kProFireGuid, kMAudioVendorId, kMAudioProFireUnitVersion,
                                kMAudioProFire2626ModelId));
    EXPECT_TRUE(IsDiceIdentity(kProFireGuid, kMAudioVendorId, kDiceInterfaceVersion,
                               kMAudioProFire2626ModelId));
}

// The registry path that used to log Kind=Unknown, audioCandidate=0.
TEST(MAudioProFireIdentity, TheRegistryMakesItADiceAudioCandidate) {
    DeviceRegistry registry{};
    const auto record = registry.UpsertFromROM(ParsedProFireRom(), LinkPolicy{});

    EXPECT_EQ(record.kind, DeviceKind::VendorSpecificAudio);
    EXPECT_TRUE(record.isAudioCandidate);
    ASSERT_NE(record.audioPolicy, nullptr);
    const auto& plan = record.audioPolicy->plan;
    EXPECT_EQ(plan.support, SupportDisposition::Supported);
    EXPECT_EQ(plan.family, AudioFamilyProviderId::DICE);
    EXPECT_EQ(plan.probePolicy, ProbePolicyId::DiceTcat);
    EXPECT_EQ(plan.profileBuilder, ProfileBuilderId::GenericDice);
    EXPECT_EQ(plan.protocolImplementation, ProtocolImplementationId::DiceTcat);
    EXPECT_EQ(plan.streamTraits.wire.forcedStreamMode, ForcedStreamMode::Blocking);
    ASSERT_EQ(plan.candidates.size(), 1U);
    EXPECT_EQ(plan.candidates[0], DeviceDefinitionId::MAudioProFire2626);
    EXPECT_EQ(plan.vendorName, kMAudioVendorName);
    EXPECT_EQ(plan.modelName, kMAudioProFire2626ModelName);
}

}  // namespace
