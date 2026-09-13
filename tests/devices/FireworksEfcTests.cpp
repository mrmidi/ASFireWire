// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FireworksEfcTests.cpp - Echo Fireworks EFC codec + transaction runner.
//
// Wire facts pinned here come from Linux sound/firewire/fireworks (header
// layout, addresses, seqnum echo, hwinfo/clock parameter order) and are spelled
// out independently of the production tables so the tables can be caught wrong.

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Protocols/Fireworks/EfcProtocol.hpp"
#include "ASFWDriver/Audio/Protocols/Fireworks/EfcResponseMailbox.hpp"
#include "ASFWDriver/Audio/Protocols/Fireworks/EfcTransport.hpp"
#include "ASFWDriver/Discovery/DeviceRegistry.hpp"

#include "FakeTimerScheduler.hpp"

#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace {

using ASFW::Async::AsyncHandle;
using ASFW::Async::AsyncStatus;
using ASFW::Async::FWAddress;
using ASFW::Async::InterfaceCompletionCallback;
using ASFW::FW::FwSpeed;
using ASFW::FW::Generation;
using ASFW::FW::NodeId;
namespace Efc = ASFW::Audio::Fireworks::Efc;
namespace Mailbox = ASFW::Audio::Fireworks::EfcResponseMailbox;
using ASFW::Audio::Fireworks::EfcTransport;

constexpr uint64_t kCommandAddress = 0xECC000000000ULL;
constexpr uint32_t kCatHwInfo = 0;
constexpr uint32_t kCatTransport = 2;
constexpr uint32_t kCatHwCtl = 3;
constexpr uint32_t kCmdGetCaps = 0;
constexpr uint32_t kCmdSetClock = 0;
constexpr uint32_t kCmdGetClock = 1;
constexpr uint32_t kCmdSetTxMode = 0;
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

// Device-side response frame, laid out the way the firmware writes it.
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

void PutName(std::vector<uint8_t>& out, const char* name) {
    char buf[32] = {};
    std::strncpy(buf, name, sizeof(buf) - 1);
    out.insert(out.end(), buf, buf + sizeof(buf));
}

// HWINFO GET_CAPS parameter block (65 quadlets when the 2x/4x counts are present).
std::vector<uint8_t> MakeHwInfoParams(uint32_t tx1x, uint32_t rx1x, bool withMultipliers) {
    std::vector<uint8_t> p;
    PutBE(p, 0x00000001);             // flags: resp addr changeable
    PutBE(p, 0x000FF204); PutBE(p, 0x00003AFC);  // guid
    PutBE(p, 0x00400F);               // type (model)
    PutBE(p, 0x00010000);             // version
    PutName(p, "Loud Technologies");  // 8 quadlets
    PutName(p, "Onyx 400F");          // 8 quadlets
    PutBE(p, (1U << 0) | (1U << 2) | (1U << 3));  // internal, word clock, S/PDIF
    PutBE(p, rx1x);                   // amdtp_rx_pcm_channels (host -> device)
    PutBE(p, tx1x);                   // amdtp_tx_pcm_channels (device -> host)
    PutBE(p, 10); PutBE(p, 10);       // phys_out, phys_in
    PutBE(p, 2);                      // phys_out_grp_count
    p.insert(p.end(), {0, 8, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});  // analog 8, spdif 2
    PutBE(p, 2);                      // phys_in_grp_count
    p.insert(p.end(), {0, 8, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    PutBE(p, 1); PutBE(p, 1);         // midi out / in
    PutBE(p, 192000); PutBE(p, 32000);  // max / min rate  (quadlets 38, 39)
    if (!withMultipliers) return p;   // 40 quadlets: the mandatory prefix
    PutBE(p, 0x04060000);             // dsp
    PutBE(p, 0x04060000);             // arm
    PutBE(p, 10); PutBE(p, 10);       // mixer playback / capture
    PutBE(p, 0x00000001);             // fpga
    PutBE(p, 8); PutBE(p, 8);         // rx 2x, tx 2x
    PutBE(p, 4); PutBE(p, 4);         // rx 4x, tx 4x
    for (int i = 0; i < 16; ++i) PutBE(p, 0);  // reserved
    return p;
}

// ---------------------------------------------------------------------------
// Codec
// ---------------------------------------------------------------------------

TEST(EfcCodec, EncodeCommandLaysOutHeaderThenParams) {
    const uint32_t params[] = {3U, 48000U, 0U};
    const auto frame = Efc::EncodeCommand(0x00010000, Efc::Category::kHwCtl, kCmdSetClock, params);
    ASSERT_EQ(frame.size(), 9U * 4U);
    EXPECT_EQ(GetBE(frame, 0), 9U);           // length in quadlets, header included
    EXPECT_EQ(GetBE(frame, 1), 1U);           // version
    EXPECT_EQ(GetBE(frame, 2), 0x00010000U);  // seqnum
    EXPECT_EQ(GetBE(frame, 3), kCatHwCtl);
    EXPECT_EQ(GetBE(frame, 4), kCmdSetClock);
    EXPECT_EQ(GetBE(frame, 5), 0U);           // status
    EXPECT_EQ(GetBE(frame, 6), 3U);
    EXPECT_EQ(GetBE(frame, 7), 48000U);
    EXPECT_EQ(GetBE(frame, 8), 0U);
}

TEST(EfcCodec, EncodeCommandWithoutParamsIsHeaderOnly) {
    const auto frame = Efc::EncodeCommand(0x00010002, Efc::Category::kHwInfo, kCmdGetCaps, {});
    ASSERT_EQ(frame.size(), 24U);
    EXPECT_EQ(GetBE(frame, 0), 6U);
}

TEST(EfcCodec, DecodeResponseRejectsShortFrames) {
    std::vector<uint8_t> tooShort(20, 0);
    EXPECT_FALSE(Efc::DecodeResponse(tooShort).has_value());
}

TEST(EfcCodec, DecodeResponseParsesHeaderAndKeepsRawParams) {
    const uint32_t params[] = {0U, 44100U, 0U};
    const auto wire = MakeResponse(0x00010001, kCatHwCtl, kCmdGetClock, 0, params);
    const auto r = Efc::DecodeResponse(wire);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->header.lengthQuadlets, 9U);
    EXPECT_EQ(r->header.version, 1U);
    EXPECT_EQ(r->header.seqnum, 0x00010001U);
    EXPECT_EQ(r->header.category, kCatHwCtl);
    EXPECT_EQ(r->header.command, kCmdGetClock);
    EXPECT_EQ(r->header.status, 0U);
    ASSERT_EQ(r->ParamQuadletCount(), 3U);
    EXPECT_EQ(r->Quadlet(1), 44100U);
    EXPECT_EQ(r->Quadlet(7), 0U);  // out of range reads as zero, never past the buffer
}

TEST(EfcCodec, DecodeResponseTrustsTheSmallerOfAdvertisedAndDelivered) {
    const uint32_t params[] = {1U, 2U, 3U};
    auto wire = MakeResponse(0x00010001, kCatHwCtl, kCmdGetClock, 0, params);
    wire.resize(wire.size() - 4);  // device write got truncated
    const auto r = Efc::DecodeResponse(wire);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->ParamQuadletCount(), 2U);

    auto padded = MakeResponse(0x00010001, kCatHwCtl, kCmdGetClock, 0, params);
    padded.resize(padded.size() + 8, 0xAA);  // trailing garbage beyond the advertised length
    const auto r2 = Efc::DecodeResponse(padded);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->ParamQuadletCount(), 3U);
}

TEST(EfcCodec, ResponseMatchesRequiresSeqnumPlusOneAndSameCommand) {
    const auto r = Efc::DecodeResponse(MakeResponse(0x00010001, kCatHwCtl, kCmdGetClock, 0, {}));
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(Efc::ResponseMatches(*r, 0x00010000, Efc::Category::kHwCtl, kCmdGetClock));
    EXPECT_FALSE(Efc::ResponseMatches(*r, 0x00010001, Efc::Category::kHwCtl, kCmdGetClock));
    EXPECT_FALSE(Efc::ResponseMatches(*r, 0x00010000, Efc::Category::kHwInfo, kCmdGetClock));
    EXPECT_FALSE(Efc::ResponseMatches(*r, 0x00010000, Efc::Category::kHwCtl, kCmdSetClock));
}

TEST(EfcCodec, SeqnumsStayEvenAboveTheUserRangeAndWrap) {
    EXPECT_EQ(Efc::kSeqnumFirst, 0x00010000U);
    EXPECT_EQ(Efc::NextSeqnum(0x00010000), 0x00010002U);
    EXPECT_EQ(Efc::NextSeqnum(0xFFFFFFFC), 0x00010000U);
    EXPECT_EQ(Efc::NextSeqnum(0x00000010), 0x00010000U);  // never inside the hwdep range
}

TEST(EfcCodec, ParseHwInfoReadsCountsNamesAndGroups) {
    const auto params = MakeHwInfoParams(10, 10, true);
    ASSERT_EQ(params.size(), 65U * 4U);
    const auto info = Efc::ParseHwInfo(params);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->guid, 0x000FF20400003AFCULL);
    EXPECT_EQ(info->type, 0x00400FU);
    EXPECT_STREQ(info->vendorName.data(), "Loud Technologies");
    EXPECT_STREQ(info->modelName.data(), "Onyx 400F");
    EXPECT_EQ(info->txPcmChannels[0], 10U);
    EXPECT_EQ(info->rxPcmChannels[0], 10U);
    EXPECT_EQ(info->txPcmChannels[1], 8U);
    EXPECT_EQ(info->txPcmChannels[2], 4U);
    EXPECT_TRUE(info->hasMultiplierCounts);
    EXPECT_EQ(info->physInGroupCount, 2U);
    EXPECT_EQ(info->physInGroups[0].type, 0U);
    EXPECT_EQ(info->physInGroups[0].count, 8U);
    EXPECT_EQ(info->physInGroups[1].type, 1U);
    EXPECT_EQ(info->physInGroups[1].count, 2U);
    EXPECT_EQ(info->minSampleRate, 32000U);
    EXPECT_EQ(info->maxSampleRate, 192000U);
    EXPECT_EQ(info->armVersion, 0x04060000U);
    EXPECT_TRUE(info->SupportsClockSource(Efc::ClockSource::kInternal));
    EXPECT_TRUE(info->SupportsClockSource(Efc::ClockSource::kSpdif));
    EXPECT_FALSE(info->SupportsClockSource(Efc::ClockSource::kAdat1));
}

TEST(EfcCodec, ParseHwInfoAcceptsOldFirmwareWithoutMultiplierCounts) {
    const auto params = MakeHwInfoParams(10, 10, false);
    ASSERT_EQ(params.size(), 40U * 4U);
    const auto info = Efc::ParseHwInfo(params);
    ASSERT_TRUE(info.has_value());
    EXPECT_FALSE(info->hasMultiplierCounts);
    EXPECT_EQ(info->txPcmChannels[1], 0U);

    auto tooShort = params;
    tooShort.resize(tooShort.size() - 4);
    EXPECT_FALSE(Efc::ParseHwInfo(tooShort).has_value());
}

TEST(EfcCodec, ClockRoundTripsAndMultiplierModes) {
    const uint32_t params[] = {3U, 96000U, 0U};
    const auto r = Efc::DecodeResponse(MakeResponse(0x00010001, kCatHwCtl, kCmdGetClock, 0, params));
    ASSERT_TRUE(r.has_value());
    const auto clock = Efc::ParseClock(*r);
    ASSERT_TRUE(clock.has_value());
    EXPECT_EQ(clock->source, 3U);
    EXPECT_EQ(clock->sampleRateHz, 96000U);
    const auto encoded = Efc::EncodeClock(*clock);
    EXPECT_EQ(encoded[0], 3U);
    EXPECT_EQ(encoded[1], 96000U);
    EXPECT_EQ(encoded[2], 0U);

    EXPECT_EQ(Efc::MultiplierModeForRate(44100), 0U);
    EXPECT_EQ(Efc::MultiplierModeForRate(96000), 1U);
    EXPECT_EQ(Efc::MultiplierModeForRate(192000), 2U);
    EXPECT_FALSE(Efc::MultiplierModeForRate(22050).has_value());

    const auto shortClock = Efc::DecodeResponse(MakeResponse(0x1, kCatHwCtl, kCmdGetClock, 0, {}));
    ASSERT_TRUE(shortClock.has_value());
    EXPECT_FALSE(Efc::ParseClock(*shortClock).has_value());
}

TEST(EfcCodec, StatusNamesCoverTheLinuxTable) {
    EXPECT_STREQ(Efc::StatusName(0), "ok");
    EXPECT_STREQ(Efc::StatusName(8), "bad rate");
    EXPECT_STREQ(Efc::StatusName(0x80000000), "incomplete");
    EXPECT_STREQ(Efc::StatusName(99), "unknown");
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

class RecordingBusOps final : public ASFW::Async::IFireWireBusOps {
public:
    struct Write {
        uint8_t node{0};
        uint64_t address{0};
        std::vector<uint8_t> data{};
    };

    AsyncHandle ReadBlock(Generation, NodeId, FWAddress, uint32_t, FwSpeed,
                          InterfaceCompletionCallback cb) override {
        cb(AsyncStatus::kSuccess, {});
        return AsyncHandle{++next_};
    }
    AsyncHandle WriteBlock(Generation, NodeId node, FWAddress address, std::span<const uint8_t> data,
                           FwSpeed, InterfaceCompletionCallback cb) override {
        writes.push_back(Write{.node = node.value,
                               .address = ASFW::FW::Pack(address) & 0xFFFFFFFFFFFFULL,
                               .data = std::vector<uint8_t>(data.begin(), data.end())});
        if (deferCompletions) {
            deferred.push_back(std::move(cb));
        } else {
            cb(nextStatus, {});
        }
        return AsyncHandle{++next_};
    }
    AsyncHandle Lock(Generation, NodeId, FWAddress, ASFW::FW::LockOp, std::span<const uint8_t>,
                     uint32_t, FwSpeed, InterfaceCompletionCallback cb) override {
        cb(AsyncStatus::kSuccess, {});
        return AsyncHandle{++next_};
    }
    bool Cancel(AsyncHandle) override { return false; }

    std::vector<Write> writes;
    AsyncStatus nextStatus{AsyncStatus::kSuccess};
    bool deferCompletions{false};
    std::vector<InterfaceCompletionCallback> deferred;

private:
    uint32_t next_{0};
};

class StubBusInfo final : public ASFW::Async::IFireWireBusInfo {
public:
    FwSpeed GetSpeed(NodeId) const override { return FwSpeed::S400; }
    uint32_t HopCount(NodeId, NodeId) const override { return 0; }
    Generation GetGeneration() const override { return Generation{7}; }
    NodeId GetLocalNodeID() const override { return NodeId{0}; }
};

class EfcTransportTest : public testing::Test {
protected:
    EfcTransportTest() {
        ASFW::Discovery::ConfigROM rom{};
        rom.bib.guid = kGuid;
        rom.gen = Generation{1};
        rom.nodeId = kNode;
        (void)routes_.UpsertFromROM(rom, ASFW::Discovery::LinkPolicy{});
        route_ = *routes_.CurrentRoute(kGuid);
    }

    // Feed a device response for the in-flight command through the mailbox,
    // exactly as LocalRequestWiring would.
    bool Respond(EfcTransport& t, uint32_t category, uint32_t command, uint32_t status,
                 std::span<const uint32_t> params, uint16_t sourceID = kNode) {
        const auto wire = MakeResponse(t.InflightSeqnum() + 1, category, command, status, params);
        return Mailbox::Publish(sourceID, wire);
    }

    static constexpr uint64_t kGuid = 0x000FF20400003AFCULL;
    static constexpr uint8_t kNode = 2;
    RecordingBusOps bus_;
    StubBusInfo info_;
    ASFW::Testing::FakeTimerScheduler timer_;
    ASFW::Discovery::DeviceRegistry routes_;
    ASFW::Discovery::DeviceRouteToken route_{};
};

TEST_F(EfcTransportTest, RegistersWithTheMailboxForItsLifetime) {
    const size_t before = Mailbox::ObserverCount();
    {
        EfcTransport t(bus_, info_, &timer_);
        EXPECT_TRUE(t.IsRegistered());
        EXPECT_EQ(Mailbox::ObserverCount(), before + 1);
    }
    EXPECT_EQ(Mailbox::ObserverCount(), before);
}

TEST_F(EfcTransportTest, SubmitWritesTheCommandFrameToTheDeviceCommandRegister) {
    EfcTransport t(bus_, info_, &timer_);
    t.SetRoute(route_);
    const uint32_t params[] = {1U};
    t.Submit(Efc::Category::kTransport, kCmdSetTxMode, params, [](IOReturn, const Efc::Response&) {});

    ASSERT_EQ(bus_.writes.size(), 1U);
    EXPECT_EQ(bus_.writes[0].node, kNode);
    EXPECT_EQ(bus_.writes[0].address, kCommandAddress);
    ASSERT_EQ(bus_.writes[0].data.size(), 7U * 4U);
    EXPECT_EQ(GetBE(bus_.writes[0].data, 2), Efc::kSeqnumFirst);
    EXPECT_EQ(GetBE(bus_.writes[0].data, 3), kCatTransport);
    EXPECT_EQ(GetBE(bus_.writes[0].data, 6), 1U);
    EXPECT_TRUE(t.HasInflight());
    EXPECT_EQ(timer_.PendingCount(), 1U);  // response timeout armed after the write completed
}

TEST_F(EfcTransportTest, MatchingResponseCompletesWithParamsAndDisarmsTimeout) {
    EfcTransport t(bus_, info_, &timer_);
    t.SetRoute(route_);
    IOReturn status = kIOReturnNotReady;
    uint32_t rate = 0;
    t.Submit(Efc::Category::kHwCtl, kCmdGetClock, {}, [&](IOReturn s, const Efc::Response& r) {
        status = s;
        rate = r.Quadlet(1);
    });

    // A frame for a different transaction is not ours.
    const uint32_t other[] = {0U, 1U, 2U};
    EXPECT_FALSE(Mailbox::Publish(kNode, MakeResponse(0x00ABCDEF, kCatHwCtl, kCmdGetClock, 0, other)));
    EXPECT_EQ(status, kIOReturnNotReady);

    const uint32_t clock[] = {0U, 44100U, 0U};
    EXPECT_TRUE(Respond(t, kCatHwCtl, kCmdGetClock, 0, clock));
    EXPECT_EQ(status, kIOReturnSuccess);
    EXPECT_EQ(rate, 44100U);
    EXPECT_FALSE(t.HasInflight());
    EXPECT_EQ(timer_.PendingCount(), 0U);
}

TEST_F(EfcTransportTest, IgnoresResponsesFromOtherNodes) {
    EfcTransport t(bus_, info_, &timer_);
    t.SetRoute(route_);
    bool called = false;
    t.Submit(Efc::Category::kHwCtl, kCmdGetClock, {}, [&](IOReturn, const Efc::Response&) { called = true; });
    const uint32_t clock[] = {0U, 44100U, 0U};
    EXPECT_FALSE(Respond(t, kCatHwCtl, kCmdGetClock, 0, clock, /*sourceID=*/kNode + 1));
    EXPECT_FALSE(called);
    EXPECT_TRUE(t.HasInflight());
}

TEST_F(EfcTransportTest, DeviceErrorStatusSurfacesAsErrorWithTheResponse) {
    EfcTransport t(bus_, info_, &timer_);
    t.SetRoute(route_);
    IOReturn status = kIOReturnSuccess;
    uint32_t efcStatus = 0;
    t.Submit(Efc::Category::kHwCtl, kCmdSetClock, {}, [&](IOReturn s, const Efc::Response& r) {
        status = s;
        efcStatus = r.header.status;
    });
    EXPECT_TRUE(Respond(t, kCatHwCtl, kCmdSetClock, /*status=*/8, {}));
    EXPECT_EQ(status, kIOReturnError);
    EXPECT_EQ(efcStatus, 8U);  // bad rate
}

TEST_F(EfcTransportTest, CommandMismatchInAnOtherwiseMatchingSeqnumIsRejected) {
    EfcTransport t(bus_, info_, &timer_);
    t.SetRoute(route_);
    IOReturn status = kIOReturnSuccess;
    t.Submit(Efc::Category::kHwCtl, kCmdGetClock, {}, [&](IOReturn s, const Efc::Response&) { status = s; });
    EXPECT_TRUE(Respond(t, kCatHwInfo, kCmdGetCaps, 0, {}));
    EXPECT_EQ(status, kIOReturnBadArgument);
}

TEST_F(EfcTransportTest, TimeoutResendsThreeTimesThenFails) {
    EfcTransport t(bus_, info_, &timer_);
    t.SetRoute(route_);
    IOReturn status = kIOReturnSuccess;
    t.Submit(Efc::Category::kHwInfo, kCmdGetCaps, {}, [&](IOReturn s, const Efc::Response&) { status = s; });
    ASSERT_EQ(bus_.writes.size(), 1U);

    timer_.Advance(125 * kMs);
    EXPECT_EQ(bus_.writes.size(), 2U);
    EXPECT_EQ(t.InflightAttempts(), 2U);
    EXPECT_EQ(GetBE(bus_.writes[1].data, 2), GetBE(bus_.writes[0].data, 2));  // same seqnum on retry

    timer_.Advance(125 * kMs);
    EXPECT_EQ(bus_.writes.size(), 3U);
    EXPECT_EQ(status, kIOReturnSuccess);  // still pending

    timer_.Advance(125 * kMs);
    EXPECT_EQ(bus_.writes.size(), 3U);
    EXPECT_EQ(status, kIOReturnTimeout);
    EXPECT_FALSE(t.HasInflight());
}

TEST_F(EfcTransportTest, LateResponseAfterRetryStillCompletes) {
    EfcTransport t(bus_, info_, &timer_);
    t.SetRoute(route_);
    IOReturn status = kIOReturnNotReady;
    t.Submit(Efc::Category::kHwInfo, kCmdGetCaps, {}, [&](IOReturn s, const Efc::Response&) { status = s; });
    timer_.Advance(125 * kMs);  // one retry out
    const auto params = MakeHwInfoParams(10, 10, true);
    std::vector<uint32_t> quads(params.size() / 4);
    for (size_t i = 0; i < quads.size(); ++i) quads[i] = GetBE(params, i);
    EXPECT_TRUE(Respond(t, kCatHwInfo, kCmdGetCaps, 0, quads));
    EXPECT_EQ(status, kIOReturnSuccess);
    EXPECT_EQ(timer_.PendingCount(), 0U);
}

TEST_F(EfcTransportTest, WriteTimeoutRetriesAndBusResetFailsFast) {
    EfcTransport t(bus_, info_, &timer_);
    t.SetRoute(route_);
    bus_.nextStatus = AsyncStatus::kTimeout;
    IOReturn status = kIOReturnSuccess;
    t.Submit(Efc::Category::kHwInfo, kCmdGetCaps, {}, [&](IOReturn s, const Efc::Response&) { status = s; });
    EXPECT_EQ(bus_.writes.size(), 3U);
    EXPECT_EQ(status, kIOReturnTimeout);

    bus_.writes.clear();
    bus_.nextStatus = AsyncStatus::kStaleGeneration;
    t.Submit(Efc::Category::kHwInfo, kCmdGetCaps, {}, [&](IOReturn s, const Efc::Response&) { status = s; });
    EXPECT_EQ(bus_.writes.size(), 1U);
    EXPECT_EQ(status, kIOReturnNotResponding);
}

TEST_F(EfcTransportTest, QueueIsSerialAndPreservesOrder) {
    EfcTransport t(bus_, info_, &timer_);
    t.SetRoute(route_);
    std::vector<int> order;
    t.Submit(Efc::Category::kTransport, kCmdSetTxMode, {}, [&](IOReturn, const Efc::Response&) { order.push_back(1); });
    t.Submit(Efc::Category::kHwCtl, kCmdGetClock, {}, [&](IOReturn, const Efc::Response&) { order.push_back(2); });
    EXPECT_EQ(bus_.writes.size(), 1U);  // second command waits
    EXPECT_EQ(t.QueuedCount(), 1U);

    EXPECT_TRUE(Respond(t, kCatTransport, kCmdSetTxMode, 0, {}));
    EXPECT_EQ(bus_.writes.size(), 2U);
    EXPECT_EQ(GetBE(bus_.writes[1].data, 2), Efc::kSeqnumFirst + 2);
    const uint32_t clock[] = {0U, 48000U, 0U};
    EXPECT_TRUE(Respond(t, kCatHwCtl, kCmdGetClock, 0, clock));
    ASSERT_EQ(order.size(), 2U);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
}

TEST_F(EfcTransportTest, CompletionMaySubmitTheNextCommand) {
    EfcTransport t(bus_, info_, &timer_);
    t.SetRoute(route_);
    IOReturn second = kIOReturnNotReady;
    t.Submit(Efc::Category::kTransport, kCmdSetTxMode, {}, [&](IOReturn, const Efc::Response&) {
        t.Submit(Efc::Category::kHwCtl, kCmdGetClock, {}, [&](IOReturn s, const Efc::Response&) { second = s; });
    });
    EXPECT_TRUE(Respond(t, kCatTransport, kCmdSetTxMode, 0, {}));
    EXPECT_EQ(bus_.writes.size(), 2U);
    EXPECT_TRUE(t.HasInflight());
    const uint32_t clock[] = {0U, 48000U, 0U};
    EXPECT_TRUE(Respond(t, kCatHwCtl, kCmdGetClock, 0, clock));
    EXPECT_EQ(second, kIOReturnSuccess);
}

TEST_F(EfcTransportTest, CancelAllAbortsInflightAndQueued) {
    EfcTransport t(bus_, info_, &timer_);
    t.SetRoute(route_);
    std::vector<IOReturn> statuses;
    t.Submit(Efc::Category::kTransport, kCmdSetTxMode, {}, [&](IOReturn s, const Efc::Response&) { statuses.push_back(s); });
    t.Submit(Efc::Category::kHwCtl, kCmdGetClock, {}, [&](IOReturn s, const Efc::Response&) { statuses.push_back(s); });
    t.CancelAll(kIOReturnAborted);
    ASSERT_EQ(statuses.size(), 2U);
    EXPECT_EQ(statuses[0], kIOReturnAborted);
    EXPECT_EQ(statuses[1], kIOReturnAborted);
    EXPECT_FALSE(t.HasInflight());
    EXPECT_EQ(t.QueuedCount(), 0U);
    EXPECT_EQ(timer_.PendingCount(), 0U);
}

TEST_F(EfcTransportTest, WithoutARouteCommandsFailNotReady) {
    EfcTransport t(bus_, info_, &timer_);
    IOReturn status = kIOReturnSuccess;
    t.Submit(Efc::Category::kHwInfo, kCmdGetCaps, {}, [&](IOReturn s, const Efc::Response&) { status = s; });
    EXPECT_EQ(status, kIOReturnNotReady);
    EXPECT_TRUE(bus_.writes.empty());
}

TEST(EfcMailbox, WindowCoversTheWholeResponseRegion) {
    EXPECT_TRUE(Mailbox::MatchesDestOffset(0xECC080000000ULL));
    EXPECT_TRUE(Mailbox::MatchesDestOffset(0xECC0800001FFULL));
    EXPECT_FALSE(Mailbox::MatchesDestOffset(0xECC080000200ULL));
    EXPECT_FALSE(Mailbox::MatchesDestOffset(0xECC000000000ULL));  // the command register is the device's
    EXPECT_FALSE(Mailbox::Publish(2, {}));  // nobody registered / nothing to claim
}

} // namespace
