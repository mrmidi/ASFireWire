// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FireworksProtocolTests.cpp - FireworksProtocol on the BeBoB base: HWINFO
// geometry gate, EFC-driven clock apply (SET_TX_MODE -> GET_CLOCK -> SET_CLOCK
// -> settle), cancellation on shutdown.

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Protocols/Fireworks/EfcResponseMailbox.hpp"
#include "ASFWDriver/Audio/Protocols/Fireworks/FireworksProtocol.hpp"
#include "ASFWDriver/Async/Interfaces/IFireWireBus.hpp"
#include "ASFWDriver/Bus/IRM/IRMClient.hpp"
#include "ASFWDriver/Discovery/DeviceRegistry.hpp"
#include "ASFWDriver/Protocols/AVC/CMP/CMPClient.hpp"

#include "FakeTimerScheduler.hpp"

#include <array>
#include <cstring>
#include <span>
#include <unordered_map>
#include <vector>

namespace {

using ASFW::Async::AsyncHandle;
using ASFW::Async::AsyncStatus;
using ASFW::Async::FWAddress;
using ASFW::Async::IFireWireBus;
using ASFW::Async::InterfaceCompletionCallback;
using ASFW::FW::FwSpeed;
using ASFW::FW::Generation;
using ASFW::FW::NodeId;
namespace Efc = ASFW::Audio::Fireworks::Efc;
namespace Mailbox = ASFW::Audio::Fireworks::EfcResponseMailbox;
using ASFW::Audio::Fireworks::FireworksProtocol;
using ASFW::Audio::Fireworks::kOnyx400FGeometry;
using GeometryCheck = FireworksProtocol::GeometryCheck;

constexpr uint32_t kCatHwInfo = 0;
constexpr uint32_t kCatTransport = 2;
constexpr uint32_t kCatHwCtl = 3;
constexpr uint32_t kCmdGetCaps = 0;
constexpr uint32_t kCmdSetTxMode = 0;
constexpr uint32_t kCmdSetClock = 0;
constexpr uint32_t kCmdGetClock = 1;
constexpr uint64_t kMs = 1000ULL * 1000ULL;

void PutBE(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

uint32_t GetBE(const std::vector<uint8_t>& in, size_t quadlet) {
    const size_t o = quadlet * 4;
    return (uint32_t{in[o]} << 24) | (uint32_t{in[o + 1]} << 16) | (uint32_t{in[o + 2]} << 8) |
           uint32_t{in[o + 3]};
}

std::vector<uint8_t> MakeResponse(uint32_t seqnum, uint32_t category, uint32_t command,
                                  uint32_t status, std::span<const uint32_t> params) {
    std::vector<uint8_t> frame;
    PutBE(frame, 6 + static_cast<uint32_t>(params.size()));
    PutBE(frame, 1);
    PutBE(frame, seqnum);
    PutBE(frame, category);
    PutBE(frame, command);
    PutBE(frame, status);
    for (const uint32_t p : params) PutBE(frame, p);
    return frame;
}

// Minimal HWINFO block (40 mandatory quadlets) with the given 1x channel counts.
std::vector<uint32_t> MakeHwInfoQuadlets(uint32_t capture, uint32_t playback) {
    std::vector<uint32_t> q(40, 0);
    q[3] = 0x00400F;
    q[21] = 1U;        // internal clock only
    q[22] = playback;  // amdtp_rx_pcm_channels (host -> device)
    q[23] = capture;   // amdtp_tx_pcm_channels (device -> host)
    q[38] = 96000;
    q[39] = 44100;
    return q;
}

// CMP/IRM/bus-info side (PCR registers), same shape as BeBoBProtocolTests.
class FakeBus final : public IFireWireBus {
public:
    AsyncHandle ReadBlock(Generation, NodeId node, FWAddress, uint32_t, FwSpeed,
                          InterfaceCompletionCallback callback) override {
        const uint32_t value = pcrByNode_[node.value];
        std::array<uint8_t, 4> payload{static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
                                       static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
        callback(AsyncStatus::kSuccess, payload);
        return Next();
    }
    AsyncHandle WriteBlock(Generation, NodeId, FWAddress, std::span<const uint8_t>, FwSpeed,
                           InterfaceCompletionCallback callback) override {
        callback(AsyncStatus::kSuccess, {});
        return Next();
    }
    AsyncHandle Lock(Generation, NodeId, FWAddress, ASFW::FW::LockOp, std::span<const uint8_t> operand,
                     uint32_t, FwSpeed, InterfaceCompletionCallback callback) override {
        std::array<uint8_t, 4> payload{};
        if (operand.size() >= 4) std::memcpy(payload.data(), operand.data(), 4);
        callback(AsyncStatus::kSuccess, payload);
        return Next();
    }
    bool Cancel(AsyncHandle) override { return false; }
    FwSpeed GetSpeed(NodeId) const override { return FwSpeed::S400; }
    uint32_t HopCount(NodeId, NodeId) const override { return 0; }
    uint8_t GetGapCount() const override { return 63; }
    Generation GetGeneration() const override { return Generation{1}; }
    NodeId GetLocalNodeID() const override { return NodeId{0}; }

    std::unordered_map<uint8_t, uint32_t> pcrByNode_{{2, 0x80000000U}};

private:
    AsyncHandle Next() { return AsyncHandle{++next_}; }
    uint32_t next_{0};
};

// EFC side: records every command frame the protocol sends.
class RecordingBusOps final : public ASFW::Async::IFireWireBusOps {
public:
    struct Write {
        uint64_t address{0};
        std::vector<uint8_t> data{};
        uint32_t Seqnum() const { return GetBE(data, 2); }
        uint32_t Category() const { return GetBE(data, 3); }
        uint32_t Command() const { return GetBE(data, 4); }
        uint32_t Param(size_t i) const { return GetBE(data, 6 + i); }
    };
    AsyncHandle ReadBlock(Generation, NodeId, FWAddress, uint32_t, FwSpeed,
                          InterfaceCompletionCallback cb) override {
        cb(AsyncStatus::kSuccess, {});
        return AsyncHandle{++next_};
    }
    AsyncHandle WriteBlock(Generation, NodeId, FWAddress address, std::span<const uint8_t> data,
                           FwSpeed, InterfaceCompletionCallback cb) override {
        writes.push_back(Write{.address = ASFW::FW::Pack(address) & 0xFFFFFFFFFFFFULL,
                               .data = std::vector<uint8_t>(data.begin(), data.end())});
        cb(AsyncStatus::kSuccess, {});
        return AsyncHandle{++next_};
    }
    AsyncHandle Lock(Generation, NodeId, FWAddress, ASFW::FW::LockOp, std::span<const uint8_t>,
                     uint32_t, FwSpeed, InterfaceCompletionCallback cb) override {
        cb(AsyncStatus::kSuccess, {});
        return AsyncHandle{++next_};
    }
    bool Cancel(AsyncHandle) override { return false; }

    std::vector<Write> writes;

private:
    uint32_t next_{0};
};

class FireworksProtocolTest : public testing::Test {
protected:
    FireworksProtocolTest() : irm_(bus_), cmp_(bus_, bus_, routes_) {
        ASFW::Discovery::ConfigROM rom{};
        rom.bib.guid = kGuid;
        rom.gen = Generation{1};
        rom.nodeId = kNode;
        (void)routes_.UpsertFromROM(rom, ASFW::Discovery::LinkPolicy{});
        route_ = *routes_.CurrentRoute(kGuid);
    }

    const RecordingBusOps::Write& LastWrite() const { return efcBus_.writes.back(); }

    // Answer the command currently in flight (the newest write) as the device would.
    bool Respond(FireworksProtocol& proto, uint32_t category, uint32_t command, uint32_t status,
                 std::span<const uint32_t> params) {
        const uint32_t seq = proto.Transport().InflightSeqnum() + 1;
        return Mailbox::Publish(kNode, MakeResponse(seq, category, command, status, params));
    }

    void RespondHwInfo(FireworksProtocol& proto, uint32_t capture, uint32_t playback) {
        ASSERT_EQ(LastWrite().Category(), kCatHwInfo);
        ASSERT_EQ(LastWrite().Command(), kCmdGetCaps);
        EXPECT_TRUE(Respond(proto, kCatHwInfo, kCmdGetCaps, 0, MakeHwInfoQuadlets(capture, playback)));
    }

    void RespondTxMode(FireworksProtocol& proto) {
        ASSERT_EQ(LastWrite().Category(), kCatTransport);
        ASSERT_EQ(LastWrite().Command(), kCmdSetTxMode);
        EXPECT_EQ(LastWrite().Param(0), 1U);  // IEC 61883 mode
        EXPECT_TRUE(Respond(proto, kCatTransport, kCmdSetTxMode, 0, {}));
    }

    void RespondGetClock(FireworksProtocol& proto, uint32_t source, uint32_t rate) {
        ASSERT_EQ(LastWrite().Category(), kCatHwCtl);
        ASSERT_EQ(LastWrite().Command(), kCmdGetClock);
        const uint32_t clock[] = {source, rate, 0U};
        EXPECT_TRUE(Respond(proto, kCatHwCtl, kCmdGetClock, 0, clock));
    }

    static constexpr uint64_t kGuid = 0x000FF20400003AFCULL;
    static constexpr uint8_t kNode = 2;
    FakeBus bus_;
    ASFW::Discovery::DeviceRegistry routes_;
    ASFW::IRM::IRMClient irm_;
    ASFW::CMP::CMPClient cmp_;
    ASFW::Testing::FakeTimerScheduler timer_;
    RecordingBusOps efcBus_;
    ASFW::Discovery::DeviceRouteToken route_{};
};

TEST_F(FireworksProtocolTest, PublishesStaticGeometryAsRuntimeCaps) {
    FireworksProtocol proto(efcBus_, bus_, route_, &irm_, &cmp_, &timer_, kOnyx400FGeometry);
    ASFW::Audio::AudioStreamRuntimeCaps caps{};
    ASSERT_TRUE(proto.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(caps.hostInputPcmChannels, 10U);
    EXPECT_EQ(caps.hostOutputPcmChannels, 10U);
    EXPECT_EQ(caps.deviceToHostAm824Slots, 10U);
    EXPECT_EQ(caps.sampleRateHz, 44100U);
    EXPECT_EQ(caps.deviceToHostStreamCount, 1U);
    EXPECT_STREQ(proto.GetName(), "Mackie Onyx 400F");
    EXPECT_EQ(proto.GeometryStatus(), GeometryCheck::kPending);
}

TEST_F(FireworksProtocolTest, InitializeProbesHwInfoAndAcceptsMatchingGeometry) {
    FireworksProtocol proto(efcBus_, bus_, route_, &irm_, &cmp_, &timer_, kOnyx400FGeometry);
    ASSERT_EQ(proto.Initialize(), kIOReturnSuccess);
    ASSERT_EQ(efcBus_.writes.size(), 1U);
    EXPECT_EQ(LastWrite().address, 0xECC000000000ULL);
    RespondHwInfo(proto, /*capture=*/10, /*playback=*/10);
    ASSERT_TRUE(proto.HardwareInfo().has_value());
    EXPECT_EQ(proto.HardwareInfo()->txPcmChannels[0], 10U);
    EXPECT_EQ(proto.GeometryStatus(), GeometryCheck::kMatched);
}

TEST_F(FireworksProtocolTest, GeometryMismatchRefusesToPrepareDuplex) {
    FireworksProtocol proto(efcBus_, bus_, route_, &irm_, &cmp_, &timer_, kOnyx400FGeometry);
    ASSERT_EQ(proto.Initialize(), kIOReturnSuccess);
    RespondHwInfo(proto, /*capture=*/8, /*playback=*/8);
    EXPECT_EQ(proto.GeometryStatus(), GeometryCheck::kMismatch);

    IOReturn status = kIOReturnSuccess;
    proto.PrepareDuplex({}, {.sampleRateHz = 44100}, [&](IOReturn s, auto) { status = s; });
    EXPECT_EQ(status, kIOReturnUnsupported);
    EXPECT_EQ(efcBus_.writes.size(), 1U);  // no further EFC traffic

    proto.ApplyClockConfig({.sampleRateHz = 44100}, [&](IOReturn s, auto) { status = s; });
    EXPECT_EQ(status, kIOReturnUnsupported);
}

TEST_F(FireworksProtocolTest, PrepareDuplexProbesHwInfoFirstWhenInitializeDidNotRun) {
    FireworksProtocol proto(efcBus_, bus_, route_, &irm_, &cmp_, &timer_, kOnyx400FGeometry);
    bool called = false;
    proto.PrepareDuplex({}, {.sampleRateHz = 44100}, [&](IOReturn, auto) { called = true; });
    ASSERT_EQ(efcBus_.writes.size(), 1U);
    EXPECT_EQ(LastWrite().Category(), kCatHwInfo);
    EXPECT_FALSE(called);
    RespondHwInfo(proto, 10, 10);
    EXPECT_EQ(proto.GeometryStatus(), GeometryCheck::kMatched);
    // The base then continues into the clock apply: transport mode goes out next.
    ASSERT_EQ(efcBus_.writes.size(), 2U);
    EXPECT_EQ(LastWrite().Category(), kCatTransport);
}

TEST_F(FireworksProtocolTest, RejectsUnsupportedRateWithoutTouchingTheBus) {
    FireworksProtocol proto(efcBus_, bus_, route_, &irm_, &cmp_, &timer_, kOnyx400FGeometry);
    IOReturn status = kIOReturnSuccess;
    proto.ApplyClockConfig({.sampleRateHz = 96000}, [&](IOReturn s, auto) { status = s; });
    EXPECT_EQ(status, kIOReturnUnsupported);
    EXPECT_TRUE(efcBus_.writes.empty());
}

TEST_F(FireworksProtocolTest, ClockApplySkipsSetClockWhenDeviceAlreadyAtRate) {
    FireworksProtocol proto(efcBus_, bus_, route_, &irm_, &cmp_, &timer_, kOnyx400FGeometry);
    ASSERT_EQ(proto.Initialize(), kIOReturnSuccess);
    RespondHwInfo(proto, 10, 10);

    IOReturn status = kIOReturnNotReady;
    uint32_t appliedRate = 0;
    proto.ApplyClockConfig({.sampleRateHz = 44100}, [&](IOReturn s, ASFW::Audio::ClockApplyResult r) {
        status = s;
        appliedRate = r.appliedClock.sampleRateHz;
    });
    ASSERT_EQ(efcBus_.writes.size(), 2U);
    RespondTxMode(proto);
    EXPECT_TRUE(proto.TransportModeSet());
    ASSERT_EQ(efcBus_.writes.size(), 3U);
    RespondGetClock(proto, /*source=*/0, /*rate=*/44100);

    EXPECT_EQ(status, kIOReturnSuccess);
    EXPECT_EQ(appliedRate, 44100U);
    EXPECT_EQ(efcBus_.writes.size(), 3U);  // no SET_CLOCK
    EXPECT_EQ(timer_.PendingCount(), 0U);  // watchdog disarmed
    ASSERT_TRUE(proto.LastClock().has_value());
    EXPECT_EQ(proto.LastClock()->sampleRateHz, 44100U);
}

TEST_F(FireworksProtocolTest, ClockApplyWritesSetClockThenSettles) {
    FireworksProtocol proto(efcBus_, bus_, route_, &irm_, &cmp_, &timer_, kOnyx400FGeometry);
    ASSERT_EQ(proto.Initialize(), kIOReturnSuccess);
    RespondHwInfo(proto, 10, 10);

    IOReturn status = kIOReturnNotReady;
    proto.ApplyClockConfig({.sampleRateHz = 44100}, [&](IOReturn s, auto) { status = s; });
    RespondTxMode(proto);
    RespondGetClock(proto, /*source=*/3, /*rate=*/48000);  // S/PDIF, wrong rate

    ASSERT_EQ(efcBus_.writes.size(), 4U);
    EXPECT_EQ(LastWrite().Category(), kCatHwCtl);
    EXPECT_EQ(LastWrite().Command(), kCmdSetClock);
    EXPECT_EQ(LastWrite().Param(0), 3U);       // keeps the current source
    EXPECT_EQ(LastWrite().Param(1), 44100U);   // new rate
    EXPECT_EQ(LastWrite().Param(2), 0U);
    EXPECT_TRUE(Respond(proto, kCatHwCtl, kCmdSetClock, 0, {}));
    EXPECT_EQ(status, kIOReturnNotReady);      // settling
    EXPECT_EQ(timer_.PendingCount(), 1U);

    timer_.Advance(150 * kMs);
    EXPECT_EQ(status, kIOReturnSuccess);
    EXPECT_EQ(timer_.PendingCount(), 0U);
    ASSERT_TRUE(proto.LastClock().has_value());
    EXPECT_EQ(proto.LastClock()->sampleRateHz, 44100U);
    EXPECT_EQ(proto.LastClock()->source, 3U);
}

TEST_F(FireworksProtocolTest, TransportModeIsSetOncePerSession) {
    FireworksProtocol proto(efcBus_, bus_, route_, &irm_, &cmp_, &timer_, kOnyx400FGeometry);
    ASSERT_EQ(proto.Initialize(), kIOReturnSuccess);
    RespondHwInfo(proto, 10, 10);
    proto.ApplyClockConfig({.sampleRateHz = 44100}, [](IOReturn, auto) {});
    RespondTxMode(proto);
    RespondGetClock(proto, 0, 44100);

    proto.ApplyClockConfig({.sampleRateHz = 44100}, [](IOReturn, auto) {});
    ASSERT_EQ(efcBus_.writes.size(), 4U);
    EXPECT_EQ(LastWrite().Category(), kCatHwCtl);  // straight to GET_CLOCK
    EXPECT_EQ(LastWrite().Command(), kCmdGetClock);
}

TEST_F(FireworksProtocolTest, DeviceRejectingSetClockFailsTheApply) {
    FireworksProtocol proto(efcBus_, bus_, route_, &irm_, &cmp_, &timer_, kOnyx400FGeometry);
    ASSERT_EQ(proto.Initialize(), kIOReturnSuccess);
    RespondHwInfo(proto, 10, 10);
    IOReturn status = kIOReturnNotReady;
    proto.ApplyClockConfig({.sampleRateHz = 44100}, [&](IOReturn s, auto) { status = s; });
    RespondTxMode(proto);
    RespondGetClock(proto, 0, 48000);
    EXPECT_TRUE(Respond(proto, kCatHwCtl, kCmdSetClock, /*status=*/8, {}));  // bad rate
    EXPECT_EQ(status, kIOReturnError);
    EXPECT_EQ(timer_.PendingCount(), 0U);
}

TEST_F(FireworksProtocolTest, ShutdownAbortsClockApplyInFlight) {
    FireworksProtocol proto(efcBus_, bus_, route_, &irm_, &cmp_, &timer_, kOnyx400FGeometry);
    ASSERT_EQ(proto.Initialize(), kIOReturnSuccess);
    RespondHwInfo(proto, 10, 10);
    IOReturn status = kIOReturnNotReady;
    proto.ApplyClockConfig({.sampleRateHz = 44100}, [&](IOReturn s, auto) { status = s; });
    RespondTxMode(proto);
    ASSERT_TRUE(proto.Transport().HasInflight());  // GET_CLOCK pending

    (void)proto.Shutdown();
    EXPECT_EQ(status, kIOReturnAborted);
    EXPECT_FALSE(proto.Transport().HasInflight());
    EXPECT_EQ(timer_.PendingCount(), 0U);
    EXPECT_FALSE(proto.TransportModeSet());  // next session re-arms IEC 61883 mode
}

TEST_F(FireworksProtocolTest, ClockApplyWatchdogFiresWhenDeviceNeverAnswers) {
    FireworksProtocol proto(efcBus_, bus_, route_, &irm_, &cmp_, &timer_, kOnyx400FGeometry);
    ASSERT_EQ(proto.Initialize(), kIOReturnSuccess);
    RespondHwInfo(proto, 10, 10);
    IOReturn status = kIOReturnNotReady;
    proto.ApplyClockConfig({.sampleRateHz = 44100}, [&](IOReturn s, auto) { status = s; });
    // SET_TX_MODE retries 3 x 125 ms and fails first (375 ms); the apply reports it.
    timer_.Advance(400 * kMs);
    EXPECT_EQ(status, kIOReturnTimeout);
    EXPECT_EQ(timer_.PendingCount(), 0U);
}

TEST_F(FireworksProtocolTest, HealthReadsTheClockAndReportsLock) {
    FireworksProtocol proto(efcBus_, bus_, route_, &irm_, &cmp_, &timer_, kOnyx400FGeometry);
    IOReturn status = kIOReturnNotReady;
    ASFW::Audio::DuplexHealthResult health{};
    proto.ReadDuplexHealth([&](IOReturn s, ASFW::Audio::DuplexHealthResult h) {
        status = s;
        health = h;
    });
    RespondGetClock(proto, 0, 44100);
    EXPECT_EQ(status, kIOReturnSuccess);
    EXPECT_EQ(health.nominalRateHz, 44100U);
    EXPECT_TRUE(health.clockReferenceHealthy);
    EXPECT_FALSE(health.sourceLocked);  // no CMP connections yet
    EXPECT_EQ(health.runtimeCaps.hostInputPcmChannels, 10U);
}

} // namespace
