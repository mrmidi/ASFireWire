// SPDX-License-Identifier: Apache-2.0
//
// MotuV2Protocol transport-adapter tests.
//
// These pin the exact register traffic the protocol emits. The wire values were
// confirmed against a real 828mkII on 2026-07-26: a clock status read returned
// 0x00000008 (48 kHz, internal), matching the device's own front panel, and writing
// 0x00000000 moved its hardware rate display to 44.1 kHz.
//
// Async address register semantics cross-validated with Linux
// sound/firewire/motu/motu-transaction.c:74-133.

#include <gtest/gtest.h>

#include "Audio/Protocols/MOTU/MotuV2Protocol.hpp"
#include "Discovery/DeviceRegistry.hpp"

#include <optional>
#include <vector>

namespace {

using ASFW::Async::AsyncHandle;
using ASFW::Async::AsyncStatus;
using ASFW::Async::FWAddress;
using ASFW::Audio::Motu::ClockStatus;
using ASFW::Audio::Motu::kAddrBase;
using ASFW::Audio::Motu::kAsyncMessageRegionStart;
using ASFW::Audio::Motu::ClockSourceV2;
using ASFW::Audio::Motu::MotuV2Protocol;
using ASFW::Audio::Motu::Reg;

constexpr uint32_t k828mk2SwVersion = 0x000003U;
constexpr uint16_t kNodeId = 0x0001U;

/// Minimal registry-backed route so the protocol's ProtocolRegisterIO sees a current
/// route. Mirrors the fixture used by the DICE protocol tests; the GUID is arbitrary
/// but carries MOTU's OUI so it reads correctly in any failure output.
struct RouteState {
    ASFW::Discovery::DeviceRegistry registry;
    ASFW::Discovery::DeviceRouteToken route{};

    RouteState() {
        ASFW::Discovery::ConfigROM rom{};
        rom.bib.guid = 0x0001F20000000001ULL;
        rom.gen = ASFW::FW::Generation{1};
        rom.nodeId = kNodeId;
        (void)registry.UpsertFromROM(rom, ASFW::Discovery::LinkPolicy{});
        route = *registry.CurrentRoute(rom.bib.guid);
    }
};

/// Records every transaction and replays programmed quadlet values. Completions fire
/// inline, which keeps the tests free of any scheduling assumptions.
class RecordingBus final : public ASFW::Async::IFireWireBusOps,
                           public ASFW::Async::IFireWireBusInfo {
public:
    struct Write {
        uint32_t addressLo{0};
        uint32_t value{0};
    };

    std::vector<Write> writes;
    std::vector<uint32_t> reads;
    std::optional<uint32_t> readValue;
    AsyncStatus readStatus{AsyncStatus::kSuccess};
    AsyncStatus writeStatus{AsyncStatus::kSuccess};
    AsyncStatus failWritesAfterFirst{AsyncStatus::kSuccess};

    AsyncHandle ReadBlock(ASFW::FW::Generation, ASFW::FW::NodeId, FWAddress address, uint32_t,
                          ASFW::FW::FwSpeed,
                          ASFW::Async::InterfaceCompletionCallback callback) override {
        reads.push_back(address.addressLo);
        if (readStatus != AsyncStatus::kSuccess || !readValue.has_value()) {
            callback(readStatus, {});
            return AsyncHandle{1};
        }
        const uint32_t value = *readValue;
        const uint8_t payload[4] = {static_cast<uint8_t>(value >> 24),
                                    static_cast<uint8_t>(value >> 16),
                                    static_cast<uint8_t>(value >> 8),
                                    static_cast<uint8_t>(value)};
        callback(AsyncStatus::kSuccess, std::span<const uint8_t>(payload, 4));
        return AsyncHandle{1};
    }

    AsyncHandle WriteBlock(ASFW::FW::Generation, ASFW::FW::NodeId, FWAddress address,
                           std::span<const uint8_t> data, ASFW::FW::FwSpeed,
                           ASFW::Async::InterfaceCompletionCallback callback) override {
        uint32_t value = 0;
        for (const uint8_t byte : data) {
            value = (value << 8) | byte;
        }
        const bool isFirstWrite = writes.empty();
        writes.push_back(Write{address.addressLo, value});

        const AsyncStatus status =
            (!isFirstWrite && failWritesAfterFirst != AsyncStatus::kSuccess)
                ? failWritesAfterFirst
                : writeStatus;
        callback(status, {});
        return AsyncHandle{1};
    }

    AsyncHandle Lock(ASFW::FW::Generation, ASFW::FW::NodeId, FWAddress, ASFW::FW::LockOp,
                     std::span<const uint8_t>, uint32_t, ASFW::FW::FwSpeed,
                     ASFW::Async::InterfaceCompletionCallback callback) override {
        callback(AsyncStatus::kSuccess, {});
        return AsyncHandle{1};
    }

    bool Cancel(AsyncHandle) override { return false; }

    ASFW::FW::FwSpeed GetSpeed(ASFW::FW::NodeId) const override {
        return ASFW::FW::FwSpeed::S400;
    }
    uint32_t HopCount(ASFW::FW::NodeId, ASFW::FW::NodeId) const override { return 1; }
    ASFW::FW::Generation GetGeneration() const override { return ASFW::FW::Generation{2}; }
    ASFW::FW::NodeId GetLocalNodeID() const override { return ASFW::FW::NodeId{0}; }
};

constexpr uint32_t LowOf(Reg reg) {
    return static_cast<uint32_t>(kAddrBase) + static_cast<uint32_t>(reg);
}

//==============================================================================
// Clock status
//==============================================================================

TEST(MotuV2ProtocolTests, DecodesTheClockStatusObservedOnHardware) {
    RecordingBus bus;
    bus.readValue = 0x00000008U;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<ClockStatus> observed;
    protocol.ReadClockStatus([&](IOReturn status, ClockStatus clock) {
        EXPECT_EQ(status, kIOReturnSuccess);
        observed = clock;
    });

    ASSERT_TRUE(observed.has_value());
    EXPECT_EQ(observed->raw, 0x00000008U);
    EXPECT_EQ(observed->sampleRateHz, 48000U);
    ASSERT_TRUE(observed->source.has_value());
    EXPECT_EQ(*observed->source, ClockSourceV2::Internal);

    ASSERT_EQ(bus.reads.size(), 1U);
    EXPECT_EQ(bus.reads[0], LowOf(Reg::ClockStatusV2));
    EXPECT_EQ(protocol.CachedSampleRateHz(), 48000U);
}

TEST(MotuV2ProtocolTests, ReportsReadFailureAndLeavesCacheUnset) {
    RecordingBus bus;
    bus.readStatus = AsyncStatus::kTimeout;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.ReadClockStatus([&](IOReturn s, ClockStatus) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnTimeout);
    EXPECT_EQ(protocol.CachedSampleRateHz(), 0U);
}

//==============================================================================
// Sample rate write (read-modify-write)
//==============================================================================

TEST(MotuV2ProtocolTests, WritesRateWhilePreservingClockSource) {
    RecordingBus bus;
    // 48 kHz on an external ADAT clock: rate bits change, source bits must not.
    bus.readValue = 0x00000009U;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.SetSampleRate(44100U, [&](IOReturn s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 1U);
    EXPECT_EQ(bus.writes[0].addressLo, LowOf(Reg::ClockStatusV2));
    EXPECT_EQ(bus.writes[0].value, 0x00000001U); // rate index 0, source 1 preserved
    EXPECT_EQ(protocol.CachedSampleRateHz(), 44100U);
}

TEST(MotuV2ProtocolTests, SkipsTheWriteWhenTheRateAlreadyMatches) {
    RecordingBus bus;
    bus.readValue = 0x00000008U;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.SetSampleRate(48000U, [&](IOReturn s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnSuccess);
    EXPECT_TRUE(bus.writes.empty());
}

TEST(MotuV2ProtocolTests, RejectsAnUnsupportedRateWithoutTouchingTheDevice) {
    RecordingBus bus;
    bus.readValue = 0x00000008U;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.SetSampleRate(32000U, [&](IOReturn s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnUnsupported);
    EXPECT_TRUE(bus.writes.empty());
}

//==============================================================================
// Async message address registration / release
//==============================================================================

TEST(MotuV2ProtocolTests, RegistersAsyncAddressAsHiLoPair) {
    RecordingBus bus;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    const uint64_t hostAddress = kAsyncMessageRegionStart + 0x20U;
    std::optional<IOReturn> status;
    protocol.RegisterAsyncMessageAddress(0xffc0U, hostAddress, [&](IOReturn s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 2U);
    EXPECT_EQ(bus.writes[0].addressLo, LowOf(Reg::AsyncAddrHi));
    EXPECT_EQ(bus.writes[0].value, (0xffc0U << 16) | 0x0000ffffU);
    EXPECT_EQ(bus.writes[1].addressLo, LowOf(Reg::AsyncAddrLo));
    EXPECT_EQ(bus.writes[1].value, 0xe0000020U);
    EXPECT_TRUE(protocol.HasRegisteredAsyncAddress());
}

TEST(MotuV2ProtocolTests, RejectsAddressOutsideTheDeviceAcceptedRegion) {
    RecordingBus bus;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.RegisterAsyncMessageAddress(0xffc0U, 0x0000'1000'0000ULL,
                                         [&](IOReturn s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnBadArgument);
    EXPECT_TRUE(bus.writes.empty());
    EXPECT_FALSE(protocol.HasRegisteredAsyncAddress());
}

TEST(MotuV2ProtocolTests, ReleaseZeroesBothHalves) {
    RecordingBus bus;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.ReleaseAsyncMessageAddress([&](IOReturn s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 2U);
    EXPECT_EQ(bus.writes[0].addressLo, LowOf(Reg::AsyncAddrHi));
    EXPECT_EQ(bus.writes[0].value, 0U);
    EXPECT_EQ(bus.writes[1].addressLo, LowOf(Reg::AsyncAddrLo));
    EXPECT_EQ(bus.writes[1].value, 0U);
    EXPECT_FALSE(protocol.HasRegisteredAsyncAddress());
}

TEST(MotuV2ProtocolTests, DoesNotClaimRegistrationWhenTheSecondWriteFails) {
    RecordingBus bus;
    bus.failWritesAfterFirst = AsyncStatus::kTimeout;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.RegisterAsyncMessageAddress(0xffc0U, kAsyncMessageRegionStart,
                                         [&](IOReturn s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnTimeout);
    EXPECT_FALSE(protocol.HasRegisteredAsyncAddress());
}

TEST(MotuV2ProtocolTests, ShutdownReleasesOnlyWhenRegistered) {
    RecordingBus bus;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    // Nothing registered: shutdown must not poke the device.
    EXPECT_EQ(protocol.Shutdown(), kIOReturnSuccess);
    EXPECT_TRUE(bus.writes.empty());

    protocol.RegisterAsyncMessageAddress(0xffc0U, kAsyncMessageRegionStart, nullptr);
    ASSERT_EQ(bus.writes.size(), 2U);
    ASSERT_TRUE(protocol.HasRegisteredAsyncAddress());

    EXPECT_EQ(protocol.Shutdown(), kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 4U);
    EXPECT_EQ(bus.writes[2].value, 0U);
    EXPECT_EQ(bus.writes[3].value, 0U);
    EXPECT_FALSE(protocol.HasRegisteredAsyncAddress());
}

} // namespace
