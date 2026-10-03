// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Protocols/MOTU/MOTU828Mk3RegisterIO.hpp"
#include "ASFWDriver/Common/WireFormat.hpp"
#include "Discovery/DeviceRegistry.hpp"
#include "tests/mocks/FakeFireWireBus.hpp"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace {

using ASFW::Async::AsyncHandle;
using ASFW::Async::AsyncStatus;
using ASFW::Async::FWAddress;
using ASFW::Async::InterfaceCompletionCallback;
using ASFW::Audio::MOTU::MOTU828Mk3RegisterIO;
using ASFW::FW::FwSpeed;
using ASFW::FW::Generation;
using ASFW::FW::NodeId;

class RecordingBus final : public ASFW::Async::Fakes::FakeFireWireBus {
public:
    struct Write final {
        uint8_t nodeId{0};
        uint16_t addressHi{0};
        uint32_t addressLo{0};
        std::vector<uint8_t> payload;
    };

    AsyncHandle WriteBlock(Generation generation,
                           NodeId nodeId,
                           FWAddress address,
                           std::span<const uint8_t> payload,
                           FwSpeed speed,
                           InterfaceCompletionCallback callback) override {
        (void)generation;
        (void)speed;
        writes.push_back(Write{
            .nodeId = nodeId.value,
            .addressHi = address.addressHi,
            .addressLo = address.addressLo,
            .payload = std::vector<uint8_t>(payload.begin(), payload.end()),
        });

        const AsyncStatus status = nextStatus < statuses.size()
            ? statuses[nextStatus++]
            : AsyncStatus::kSuccess;
        callback(status, {});
        return AsyncHandle{++nextHandle};
    }

    std::vector<AsyncStatus> statuses;
    std::vector<Write> writes;

private:
    size_t nextStatus{0};
    uint32_t nextHandle{0};
};

uint32_t Quadlet(const RecordingBus::Write& write, size_t index = 0) {
    const auto payload = std::span<const uint8_t>(write.payload);
    return ASFW::FW::ReadBE32(
        payload.subspan(sizeof(uint32_t) * index, sizeof(uint32_t)).data());
}


// The register IO now addresses the device through a route token instead of a
// bare node id, so every test needs a registry whose current route matches the
// bus generation -- a stale route short-circuits each transaction with
// kStaleGeneration before it ever reaches the fake bus.
struct RouteState {
    ASFW::Discovery::DeviceRegistry registry;
    ASFW::Discovery::DeviceRouteToken route{};

    // The generation must match the fake bus: the route token carries it into
    // every transaction, and a mismatch is rejected as stale before the bus is
    // ever asked.
    explicit RouteState(uint32_t generation = 7) {
        ASFW::Discovery::ConfigROM rom{};
        rom.bib.guid = 0x0001F2FF00000001ULL;
        rom.gen = Generation{generation};
        rom.nodeId = 0x02;
        (void)registry.UpsertFromROM(rom, ASFW::Discovery::LinkPolicy{});
        route = *registry.CurrentRoute(rom.bib.guid);
    }
};

} // namespace

TEST(Motu828Mk3RegisterIOTests, SubmitsCapturedPrepareTransactionsInOrder) {
    RecordingBus bus;
    bus.SetGeneration(Generation{7});
    bus.SetSpeed(NodeId{2}, FwSpeed::S400);
    RouteState routes;
    MOTU828Mk3RegisterIO io(bus, bus, routes.registry, routes.route);

    IOReturn result = kIOReturnNotReady;
    io.RunPrepareSequence([&result](IOReturn status) { result = status; });

    ASSERT_EQ(result, kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 5u);
    EXPECT_EQ(bus.writes[0].nodeId, 2u);
    EXPECT_EQ(bus.writes[0].addressHi, 0xffffu);
    EXPECT_EQ(bus.writes[0].addressLo, 0xf0000b38u);
    ASSERT_EQ(bus.writes[0].payload.size(), 8u);
    EXPECT_EQ(Quadlet(bus.writes[0]), 0xffc20002u);
    EXPECT_EQ(Quadlet(bus.writes[0], 1), 0x00000000u);

    EXPECT_EQ(bus.writes[1].addressLo, 0xf0000b08u);
    EXPECT_EQ(Quadlet(bus.writes[1]), 0xffffffffu);
    EXPECT_EQ(bus.writes[2].addressLo, 0xf0000b04u);
    EXPECT_EQ(Quadlet(bus.writes[2]), 0xffc10001u);
    EXPECT_EQ(bus.writes[3].addressLo, 0xf0000b08u);
    EXPECT_EQ(Quadlet(bus.writes[3]), 0x00000000u);
    EXPECT_EQ(bus.writes[4].addressLo, 0xf0000b10u);
    EXPECT_EQ(Quadlet(bus.writes[4]), 0x00000002u);
}

TEST(Motu828Mk3RegisterIOTests, ContinuesAfterBestEffortWriteFailure) {
    RecordingBus bus;
    bus.statuses = {
        AsyncStatus::kTimeout,
        AsyncStatus::kHardwareError,
        AsyncStatus::kSuccess,
        AsyncStatus::kBusyRetryExhausted,
        AsyncStatus::kSuccess,
    };
    RouteState routes;
    MOTU828Mk3RegisterIO io(bus, bus, routes.registry, routes.route);

    IOReturn result = kIOReturnNotReady;
    io.RunPrepareSequence([&result](IOReturn status) { result = status; });

    EXPECT_EQ(result, kIOReturnSuccess);
    EXPECT_EQ(bus.writes.size(), 5u);
}

TEST(Motu828Mk3RegisterIOTests, ReportsPacketFormatWriteFailure) {
    RecordingBus bus;
    bus.statuses = {
        AsyncStatus::kSuccess,
        AsyncStatus::kSuccess,
        AsyncStatus::kSuccess,
        AsyncStatus::kSuccess,
        AsyncStatus::kHardwareError,
    };
    RouteState routes;
    MOTU828Mk3RegisterIO io(bus, bus, routes.registry, routes.route);

    IOReturn result = kIOReturnSuccess;
    io.RunPrepareSequence([&result](IOReturn status) { result = status; });

    EXPECT_EQ(result, kIOReturnError);
    EXPECT_EQ(bus.writes.size(), 5u);
}
