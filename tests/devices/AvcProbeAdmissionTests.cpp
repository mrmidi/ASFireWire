// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcProbeAdmissionTests.cpp - From Config ROM unit evidence to the AV/C
// traffic a unit gets: the real catalog resolves the plan, DecideAvcProbe
// turns it into the decision AVCDiscovery acts on.

#include <gtest/gtest.h>

#include "ASFWDriver/DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "ASFWDriver/DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "ASFWDriver/Audio/Protocols/AVC/AvcProbeAdmission.hpp"

#include <optional>
#include <vector>

namespace {

using namespace ASFW::DeviceProfiles::Audio;
using ASFW::Protocols::AVC::AvcProbeDecision;
using ASFW::Protocols::AVC::DecideAvcProbe;

constexpr uint32_t kTa1394Specifier = 0x00A02D;
constexpr uint32_t kAvcVersion = 0x010001;
// BeBoB units also carry 0x014001 (Linux bebob.c:352-354).
constexpr uint32_t kBeBoBAltAvcVersion = 0x014001;
// Echo Fireworks keeps the 1394 TA specifier with a non-AV/C version.
constexpr uint32_t kFireworksVersion = 0x010000;
constexpr uint32_t kUnknownVendor = 0x00ABCD;

ASFW::Discovery::DeviceRecord MakeDevice(uint64_t guid, std::optional<uint32_t> vendorId,
                                         std::optional<uint32_t> modelId, uint32_t specifierId,
                                         uint32_t version) {
    ASFW::Discovery::DeviceRecord device{};
    device.instanceId = ASFW::Discovery::DeviceInstanceId{1};
    device.identity.observedGuid = guid;
    device.identity.nodeVendorOui = static_cast<uint32_t>(guid >> 40U) & 0xFFFFFFU;
    device.identity.rootVendorId = vendorId;
    device.identity.rootModelId = modelId;
    ASFW::Discovery::UnitIdentityEvidence unit{};
    unit.unitDirectoryOffset = 5;
    unit.specifierId = specifierId;
    unit.version = version;
    device.identity.units.push_back(unit);
    return device;
}

// What AVCDiscovery decides for the device's single unit directory.
AvcProbeDecision Decide(const ASFW::Discovery::DeviceRecord& device) {
    const auto& unit = device.identity.units.front();
    const auto plan = AudioDeviceCatalog::Resolve(device, unit);
    return DecideAvcProbe(unit.specifierId.value_or(0), plan ? &*plan : nullptr);
}

TEST(AvcProbeAdmission, UnknownStandardAvcUnitGetsGenericDiscovery) {
    // Config ROM says 1394 TA + AV/C, and no catalog row knows the device.
    const auto device = MakeDevice(0x00ABCD0000000001ULL, kUnknownVendor, 0x000001,
                                   kTa1394Specifier, kAvcVersion);
    EXPECT_EQ(Decide(device), AvcProbeDecision::GenericDiscovery);
}

TEST(AvcProbeAdmission, NonTa1394UnitIsNeverAvc) {
    // A vendor specifier (as on DICE) never reaches AV/C, whatever the plan.
    const auto device = MakeDevice(0x00ABCD0000000002ULL, kUnknownVendor, 0x000002,
                                   kUnknownVendor, 0x000001);
    EXPECT_EQ(Decide(device), AvcProbeDecision::NotAvcUnit);
}

TEST(AvcProbeAdmission, UnknownTa1394UnitWithOtherVersionGetsNoTraffic) {
    // Linux BeBoB would match this (it ignores the version); we stay off the
    // wire until a catalog row says otherwise.
    const auto bebobVersion = MakeDevice(0x00ABCD0000000003ULL, kUnknownVendor, 0x000003,
                                         kTa1394Specifier, kBeBoBAltAvcVersion);
    EXPECT_EQ(Decide(bebobVersion), AvcProbeDecision::NoPolicy);
    const auto fireworksVersion = MakeDevice(0x00ABCD0000000004ULL, kUnknownVendor, 0x000004,
                                             kTa1394Specifier, kFireworksVersion);
    EXPECT_EQ(Decide(fireworksVersion), AvcProbeDecision::NoPolicy);
}

TEST(AvcProbeAdmission, NamedDevicesKeepTheirBringUp) {
    EXPECT_EQ(Decide(MakeDevice(0x0003DB0A0000D112ULL, kApogeeVendorId, kApogeeDuetModelId,
                                kTa1394Specifier, kAvcVersion)),
              AvcProbeDecision::GenericDiscovery);
    EXPECT_EQ(Decide(MakeDevice(0x000AAC0300B1D1F7ULL, kTerraTecVendorId, kPhase88RackFwModelId,
                                kTa1394Specifier, kAvcVersion)),
              AvcProbeDecision::GenericDiscovery);
    EXPECT_EQ(Decide(MakeDevice(0x000D6C0400DA3D9AULL, kMAudioVendorId, kMAudioFireWire1814ModelId,
                                kTa1394Specifier, kAvcVersion)),
              AvcProbeDecision::ProfileOwned);
    EXPECT_EQ(Decide(MakeDevice(0x000FF20400000001ULL, kMackieVendorId, kOnyx400FModelId,
                                kTa1394Specifier, kFireworksVersion)),
              AvcProbeDecision::FireworksEfc);
}

TEST(AvcProbeAdmission, BootloaderPersonaGetsNoAvcTraffic) {
    const auto device = MakeDevice(0x000D6C0400DA3D9BULL, kMAudioVendorId,
                                   kMAudioFireWire1814BootloaderModelId, kTa1394Specifier,
                                   kAvcVersion);
    EXPECT_NE(Decide(device), AvcProbeDecision::GenericDiscovery);
}

TEST(AvcProbeAdmission, IdentifiedChipsAddTheirExtensionInventory) {
    using ASFW::Protocols::AVC::AvcExtensionInventory;
    using ASFW::Protocols::AVC::ExtensionInventoryFor;
    const auto inventory = [](const ASFW::Discovery::DeviceRecord& device) {
        const auto plan = AudioDeviceCatalog::Resolve(device, device.identity.units.front());
        return plan ? ExtensionInventoryFor(*plan) : AvcExtensionInventory::kNone;
    };
    EXPECT_EQ(inventory(MakeDevice(0x000AAC0300B1D1F7ULL, kTerraTecVendorId, kPhase88RackFwModelId,
                                   kTa1394Specifier, kAvcVersion)),
              AvcExtensionInventory::kBridgeCo);
    EXPECT_EQ(inventory(MakeDevice(0x0003DB0A0000D112ULL, kApogeeVendorId, kApogeeDuetModelId,
                                   kTa1394Specifier, kAvcVersion)),
              AvcExtensionInventory::kNone) << "Oxford units need nothing beyond generic discovery";
    EXPECT_EQ(inventory(MakeDevice(0x00ABCD0000000001ULL, kUnknownVendor, 0x000001,
                                   kTa1394Specifier, kAvcVersion)),
              AvcExtensionInventory::kNone);
}

TEST(AvcProbeAdmission, NoPlanMeansNoTraffic) {
    EXPECT_EQ(DecideAvcProbe(kTa1394Specifier, nullptr), AvcProbeDecision::NoPolicy);
}

} // namespace
