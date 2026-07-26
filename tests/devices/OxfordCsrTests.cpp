// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// OxfordCsrTests.cpp - Oxford FW970/971 CSR reads (FW-137).

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Protocols/Oxford/OxfordCsr.hpp"

#include "AvcTestRig.hpp"

#include <vector>

namespace {

using ASFW::Audio::Oxford::Asic;
using ASFW::Testing::AvcTestRig;
namespace Oxford = ASFW::Audio::Oxford;

// ============================================================================
// Address computation — the arithmetic that silently reads the wrong register
// ============================================================================

TEST(OxfordCsr, FirmwareIdAddressSplitsAcrossTheQuadletBoundary) {
    constexpr auto address = Oxford::FirmwareIdAddress();
    // 0xFFFFF0000000 + 0x50000 = 0xFFFFF0050000
    static_assert(address.addressHi == 0xFFFFU);
    static_assert(address.addressLo == 0xF0050000U);
    EXPECT_EQ(address.addressHi, 0xFFFFU);
    EXPECT_EQ(address.addressLo, 0xF0050000U);
}

TEST(OxfordCsr, HardwareIdAddressSplitsAcrossTheQuadletBoundary) {
    constexpr auto address = Oxford::HardwareIdAddress();
    // 0xFFFFF0000000 + 0x90020 = 0xFFFFF0090020
    static_assert(address.addressHi == 0xFFFFU);
    static_assert(address.addressLo == 0xF0090020U);
    EXPECT_EQ(address.addressHi, 0xFFFFU);
    EXPECT_EQ(address.addressLo, 0xF0090020U);
}

// ============================================================================
// Decode and classification
// ============================================================================

TEST(OxfordCsr, DecodesBigEndianQuadlet) {
    constexpr std::array<uint8_t, 4> fixture{0x39, 0x37, 0x31, 0x00};
    EXPECT_EQ(Oxford::DecodeIdQuadlet(fixture), 0x39373100U);
}

TEST(OxfordCsr, ClassifiesKnownHardwareIds) {
    static_assert(Oxford::ClassifyHardwareId(0x39443841U) == Asic::kFw970);
    static_assert(Oxford::ClassifyHardwareId(0x39373100U) == Asic::kFw971);
    EXPECT_EQ(Oxford::ClassifyHardwareId(Oxford::kHardwareIdFw970), Asic::kFw970);
    EXPECT_EQ(Oxford::ClassifyHardwareId(Oxford::kHardwareIdFw971), Asic::kFw971);
}

TEST(OxfordCsr, UnknownHardwareIdDoesNotDefaultToEitherAsic) {
    // Guessing a generation would select the wrong SYT policy, so an
    // unrecognised ID must stay unknown.
    static_assert(Oxford::ClassifyHardwareId(0U) == Asic::kUnknown);
    EXPECT_EQ(Oxford::ClassifyHardwareId(0x39373101U), Asic::kUnknown);
    EXPECT_EQ(Oxford::ClassifyHardwareId(0xFFFFFFFFU), Asic::kUnknown);
}

// ============================================================================
// Async reads against the shared rig
// ============================================================================

class OxfordCsrReadTests : public ::testing::Test {
protected:
    AvcTestRig rig;

    /// Resolves through the registry on every call, the way the driver does.
    [[nodiscard]] Oxford::RouteProvider LiveRoute() {
        return [this, guid = rig.Route().guid] { return rig.Routes().CurrentRoute(guid); };
    }
    [[nodiscard]] static Oxford::RouteProvider NoRoute() {
        return [] { return std::optional<ASFW::Discovery::DeviceRouteToken>{}; };
    }

    void MapId(const ASFW::Async::FWAddress& address, uint32_t value) {
        rig.Bus().MapReadQuadlet(
            (static_cast<uint64_t>(address.addressHi) << 32U) | address.addressLo, value);
    }
};

TEST_F(OxfordCsrReadTests, ReadsFirmwareIdFromItsOwnRegister) {
    MapId(Oxford::FirmwareIdAddress(), 0x01020304U);
    MapId(Oxford::HardwareIdAddress(), 0x39373100U);

    IOReturn status = kIOReturnNotReady;
    uint32_t id = 0;
    Oxford::ReadFirmwareId(rig.Bus(), LiveRoute(),
                           [&](IOReturn s, uint32_t value) {
                               status = s;
                               id = value;
                           });

    EXPECT_EQ(status, kIOReturnSuccess);
    EXPECT_EQ(id, 0x01020304U);
}

TEST_F(OxfordCsrReadTests, ReadsHardwareIdAndClassifiesTheDuetAsFw971) {
    MapId(Oxford::FirmwareIdAddress(), 0x01020304U);
    MapId(Oxford::HardwareIdAddress(), Oxford::kHardwareIdFw971);

    IOReturn status = kIOReturnNotReady;
    uint32_t id = 0;
    Oxford::ReadHardwareId(rig.Bus(), LiveRoute(),
                           [&](IOReturn s, uint32_t value) {
                               status = s;
                               id = value;
                           });

    EXPECT_EQ(status, kIOReturnSuccess);
    // The Duet is an OXFW971 (linux .../oxfw/oxfw.c:377).
    EXPECT_EQ(Oxford::ClassifyHardwareId(id), Asic::kFw971);
}

TEST_F(OxfordCsrReadTests, DeviceWithNoLiveRouteIsRefusedBeforeAnyBusTraffic) {
    IOReturn status = kIOReturnSuccess;
    Oxford::ReadHardwareId(rig.Bus(), NoRoute(),
                           [&](IOReturn s, uint32_t) { status = s; });

    EXPECT_EQ(status, kIOReturnNotReady);
    EXPECT_EQ(rig.Bus().ReadCount(), 0U) << "a dead route must not reach the bus";
}

TEST_F(OxfordCsrReadTests, UnwiredProviderIsRefusedBeforeAnyBusTraffic) {
    // Distinct from "device has no live route": this one is a programming
    // error, and collapsing the two is what made FW-142 undiagnosable.
    IOReturn status = kIOReturnSuccess;
    Oxford::ReadHardwareId(rig.Bus(), Oxford::RouteProvider{},
                           [&](IOReturn s, uint32_t) { status = s; });

    EXPECT_EQ(status, kIOReturnNotReady);
    EXPECT_EQ(rig.Bus().ReadCount(), 0U);
}

TEST_F(OxfordCsrReadTests, RouteReboundBeforeTheReadIsResolvedFreshNotFromASnapshot) {
    // FW-142 regression. The device is rebound in the registry — same node, same
    // generation, new routeEpoch — between the moment a caller would have
    // captured a route and the moment the read is issued.
    //
    // The old API took (route, isRouteCurrent) and ApogeeDuetProtocol passed a
    // token snapshotted at construction, so this exact sequence failed every CSR
    // read with "route not current" against a device that was present and
    // answering vendor commands on the identical node and generation.
    MapId(Oxford::HardwareIdAddress(), Oxford::kHardwareIdFw971);

    const auto snapshot = rig.Route();
    ASSERT_TRUE(static_cast<bool>(snapshot));

    ASFW::Discovery::ConfigROM rom{};
    rom.bib.guid = snapshot.guid;
    rom.gen = snapshot.generation;
    rom.nodeId = static_cast<uint8_t>(snapshot.nodeId);
    rig.Routes().InvalidateLiveMappingsForBusReset();
    rig.Routes().UpsertFromROM(rom, ASFW::Discovery::LinkPolicy{});

    const auto rebound = rig.Route();
    ASSERT_NE(rebound.routeEpoch, snapshot.routeEpoch) << "test must actually rebind the route";
    ASSERT_EQ(rebound.nodeId, snapshot.nodeId);
    ASSERT_EQ(rebound.generation, snapshot.generation);
    EXPECT_FALSE(rig.Routes().IsCurrent(snapshot))
        << "the snapshot must be stale — otherwise this test proves nothing";

    IOReturn status = kIOReturnNotReady;
    uint32_t id = 0;
    Oxford::ReadHardwareId(rig.Bus(), LiveRoute(), [&](IOReturn s, uint32_t value) {
        status = s;
        id = value;
    });

    EXPECT_EQ(status, kIOReturnSuccess);
    EXPECT_EQ(Oxford::ClassifyHardwareId(id), Asic::kFw971);
}

TEST_F(OxfordCsrReadTests, ReadFailurePropagatesAsAnErrorRatherThanAZeroId) {
    // Nothing is mapped, so the bus answers kTimeout. Reporting 0 as a
    // successful read would be indistinguishable from an odd device ID that
    // legitimately classifies as unknown.
    IOReturn status = kIOReturnSuccess;
    uint32_t id = 0xDEADBEEFU;
    Oxford::ReadFirmwareId(rig.Bus(), LiveRoute(),
                           [&](IOReturn s, uint32_t value) {
                               status = s;
                               id = value;
                           });

    EXPECT_EQ(status, kIOReturnError);
    EXPECT_EQ(id, 0U);
    EXPECT_EQ(rig.Bus().ReadCount(), 1U);
}

TEST_F(OxfordCsrReadTests, ShortPayloadIsRejected) {
    rig.Bus().MapRead((static_cast<uint64_t>(Oxford::FirmwareIdAddress().addressHi) << 32U) |
                          Oxford::FirmwareIdAddress().addressLo,
                      std::vector<uint8_t>{0x39, 0x37});

    IOReturn status = kIOReturnSuccess;
    Oxford::ReadFirmwareId(rig.Bus(), LiveRoute(),
                           [&](IOReturn s, uint32_t) { status = s; });

    EXPECT_EQ(status, kIOReturnError);
}

TEST_F(OxfordCsrReadTests, RouteInvalidatedDuringTheReadDiscardsTheAnswer) {
    MapId(Oxford::HardwareIdAddress(), Oxford::kHardwareIdFw971);

    // A bus reset between issue and completion means the quadlet may have come
    // from whatever now occupies that node.
    IOReturn status = kIOReturnSuccess;
    uint32_t id = 0xDEADBEEFU;
    int resolves = 0;
    const auto live = rig.Route();
    Oxford::ReadHardwareId(
        rig.Bus(),
        [&resolves, live]() -> std::optional<ASFW::Discovery::DeviceRouteToken> {
            // Live at issue, retired by the time the completion re-resolves.
            return resolves++ == 0 ? std::optional{live} : std::nullopt;
        },
        [&](IOReturn s, uint32_t value) {
            status = s;
            id = value;
        });

    EXPECT_EQ(status, kIOReturnError);
    EXPECT_EQ(id, 0U);
}

} // namespace
