// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Tests for BeBoBProtocol base class: async ApplyClockConfig chain, timer-based
// settle, epoch cancellation, and StreamPlug usage.

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Protocols/BeBoB/BeBoBProtocol.hpp"
#include "ASFWDriver/Audio/Protocols/BeBoB/MAudioSpecialProtocol.hpp"
#include "ASFWDriver/Audio/Protocols/BeBoB/MAudioSpecialRouting.hpp"
#include "ASFWDriver/Protocols/BeBoB/VirtualUart/BeBoBVirtualUartCommand.hpp"
#include "ASFWDriver/Async/Interfaces/IFireWireBus.hpp"
#include "ASFWDriver/Discovery/DeviceRegistry.hpp"
#include "ASFWDriver/Protocols/AVC/CMP/CMPClient.hpp"
#include "AvcTestRig.hpp"
#include "../support/MAudioSpecialHappyPathFixture.inc"

#include <array>
#include <cstring>
#include <unordered_map>

#include "FakeTimerScheduler.hpp"

using ASFW::Audio::DuplexHealthResult;

namespace {

TEST(BeBoBVirtualUartCommandTests, AllowsOnlyTheThreeShellOpcodes) {
    namespace VUart = ASFW::Protocols::BeBoB::VirtualUart;
    std::array<uint8_t, 12> envelope{1, 0, 0, 0, 1, 0, 0x07, 0, 0, 0, 0, 0};
    EXPECT_EQ(VUart::VirtualUartOpcodeOf(envelope), 0x07);
    EXPECT_TRUE(VUart::IsPermittedVirtualUartOpcode(VUart::VirtualUartOpcodeOf(envelope)));
    envelope[6] = 0x08;
    EXPECT_TRUE(VUart::IsPermittedVirtualUartOpcode(VUart::VirtualUartOpcodeOf(envelope)));
    envelope[6] = 0x09;
    EXPECT_TRUE(VUart::IsPermittedVirtualUartOpcode(VUart::VirtualUartOpcodeOf(envelope)));
    envelope[6] = 0x0a;
    EXPECT_FALSE(VUart::IsPermittedVirtualUartOpcode(VUart::VirtualUartOpcodeOf(envelope)));
    envelope[6] = 0x11;
    EXPECT_FALSE(VUart::IsPermittedVirtualUartOpcode(VUart::VirtualUartOpcodeOf(envelope)));
}

TEST(BeBoBVirtualUartCommandTests, BlocksEveryOtherBootloaderWindowWrite) {
    namespace VUart = ASFW::Protocols::BeBoB::VirtualUart;
    std::array<uint8_t, 12> envelope{1, 0, 0, 0, 1, 0, 0x09, 0, 1, 0, 0, 0};
    EXPECT_TRUE(VUart::IsPermittedUserClientWrite(0xffff, VUart::kRequestAddressLo, envelope));
    EXPECT_FALSE(VUart::IsPermittedUserClientWrite(0xffff, VUart::kRequestAddressLo,
                                                   std::span<const uint8_t>(envelope).first(8)));
    envelope[6] = 0x0a;
    EXPECT_FALSE(VUart::IsPermittedUserClientWrite(0xffff, VUart::kRequestAddressLo, envelope));
    EXPECT_FALSE(VUart::IsPermittedUserClientWrite(0xffff, 0xc8022000, envelope));
    EXPECT_FALSE(VUart::IsPermittedUserClientWrite(0xffff, VUart::kRequestBufferAddressLo,
                                                   std::array<uint8_t, 1025>{}));
    EXPECT_TRUE(VUart::IsPermittedUserClientWrite(0xffff, VUart::kRequestBufferAddressLo,
                                                  std::array<uint8_t, 4>{1, 2, 3, 4}));
    EXPECT_TRUE(VUart::IsPermittedUserClientWrite(0xffff, 0xffff'0000,
                                                  std::array<uint8_t, 4>{1, 2, 3, 4}));
}

using ASFW::Async::AsyncHandle;
using ASFW::Async::AsyncStatus;
using ASFW::Async::FWAddress;
using ASFW::Async::IFireWireBus;
using ASFW::Async::InterfaceCompletionCallback;
using ASFW::CMP::CMPDevice;
using ASFW::CMP::CMPStatus;
using ASFW::FW::FwSpeed;
using ASFW::FW::Generation;
using ASFW::FW::NodeId;

namespace PCRRegisters = ASFW::CMP::PCRRegisters;

// Stub bus for CMP operations.
class BeBoBBus final : public IFireWireBus {
public:
    AsyncHandle ReadBlock(Generation, NodeId node, FWAddress address, uint32_t,
                          FwSpeed, InterfaceCompletionCallback callback) override {
        const uint32_t value = pcrByNode_[node.value];
        const uint32_t wire = OSSwapHostToBigInt32(value);
        std::array<uint8_t, 4> payload{};
        std::memcpy(payload.data(), &wire, sizeof(wire));
        callback(AsyncStatus::kSuccess, payload);
        return NextHandle();
    }

    AsyncHandle WriteBlock(Generation, NodeId, FWAddress, std::span<const uint8_t>,
                           FwSpeed, InterfaceCompletionCallback callback) override {
        callback(AsyncStatus::kSuccess, {});
        return NextHandle();
    }

    AsyncHandle Lock(Generation, NodeId node, FWAddress, ASFW::FW::LockOp,
                     std::span<const uint8_t> operand, uint32_t, FwSpeed,
                     InterfaceCompletionCallback callback) override {
        uint32_t expectedWire = 0;
        uint32_t desiredWire = 0;
        std::memcpy(&expectedWire, operand.data(), sizeof(expectedWire));
        std::memcpy(&desiredWire, operand.data() + sizeof(expectedWire), sizeof(desiredWire));
        const uint32_t expected = OSSwapBigToHostInt32(expectedWire);
        const uint32_t desired = OSSwapBigToHostInt32(desiredWire);
        uint32_t observed = pcrByNode_[node.value];
        if (observed == expected) {
            pcrByNode_[node.value] = desired;
        }
        const uint32_t observedWire = OSSwapHostToBigInt32(observed);
        std::array<uint8_t, 4> payload{};
        std::memcpy(payload.data(), &observedWire, sizeof(observedWire));
        callback(AsyncStatus::kSuccess, payload);
        return NextHandle();
    }

    bool Cancel(AsyncHandle) override { return false; }
    FwSpeed GetSpeed(NodeId) const override { return FwSpeed::S400; }
    uint32_t HopCount(NodeId, NodeId) const override { return 0; }
    uint8_t GetGapCount() const override { return 63; }
    Generation GetGeneration() const override { return Generation{1}; }
    NodeId GetLocalNodeID() const override { return NodeId{0}; }

    std::unordered_map<uint8_t, uint32_t> pcrByNode_{{2, 0x80000000U}};
private:
    AsyncHandle NextHandle() { return AsyncHandle{++nextHandle_}; }
    uint32_t nextHandle_{0};
};

// Minimal IFireWireBusOps stub — BeBoB never uses busOps (casts to void).
class StubBusOps final : public ASFW::Async::IFireWireBusOps {
public:
    AsyncHandle ReadBlock(Generation, NodeId, FWAddress, uint32_t, FwSpeed,
                          InterfaceCompletionCallback cb) override {
        cb(AsyncStatus::kSuccess, {});
        return AsyncHandle{++next_};
    }
    AsyncHandle WriteBlock(Generation, NodeId, FWAddress, std::span<const uint8_t>, FwSpeed,
                           InterfaceCompletionCallback cb) override {
        cb(AsyncStatus::kSuccess, {});
        return AsyncHandle{++next_};
    }
    AsyncHandle Lock(Generation, NodeId, FWAddress, ASFW::FW::LockOp, std::span<const uint8_t>,
                     uint32_t, FwSpeed, InterfaceCompletionCallback cb) override {
        cb(AsyncStatus::kSuccess, {});
        return AsyncHandle{++next_};
    }
    bool Cancel(AsyncHandle) override { return false; }
private:
    uint32_t next_{0};
};

// Concrete BeBoB protocol for testing the base class.
class TestBeBoBProtocol final : public ASFW::Audio::BeBoB::BeBoBProtocol {
public:
    using BeBoBProtocol::BeBoBProtocol;

    const char* GetName() const override { return "TestBeBoB"; }
    const char* DeviceName() const override { return "TestBeBoB"; }
    bool GetRuntimeAudioStreamCaps(ASFW::Audio::AudioStreamRuntimeCaps& outCaps) const override {
        outCaps = caps_;
        return true;
    }

    ASFW::Audio::AudioStreamRuntimeCaps DeviceCaps() const override { return caps_; }
    std::vector<uint32_t> SupportedRates() const override { return {48000}; }
    uint32_t SignalFormatInterlockMs() const noexcept override { return 100; }

    void SetCaps(const ASFW::Audio::AudioStreamRuntimeCaps& caps) { caps_ = caps; }
    void SetConnected(bool input, bool output) {
        inputConnected_ = input;
        outputConnected_ = output;
    }

private:
    ASFW::Audio::AudioStreamRuntimeCaps caps_{};
};

} // namespace

class BeBoBProtocolTest : public testing::Test {
protected:
    BeBoBProtocolTest() : cmp_(bus_, bus_, routes_) {
        ASFW::Discovery::ConfigROM rom{};
        rom.bib.guid = kGuid;
        rom.gen = Generation{1};
        rom.nodeId = kNode;
        (void)routes_.UpsertFromROM(rom, ASFW::Discovery::LinkPolicy{});
        route_ = *routes_.CurrentRoute(kGuid);
    }

    BeBoBBus bus_;
    ASFW::Discovery::DeviceRegistry routes_;
    ASFW::CMP::CMPClient cmp_;
    ASFW::Testing::FakeTimerScheduler timer_;
    StubBusOps busOps_;
    static constexpr uint64_t kGuid = 0x000aac0300b1d1f7ULL;
    static constexpr uint8_t kNode = 2;
    ASFW::Discovery::DeviceRouteToken route_{};
};

// ApplyClockConfig rejects unsupported rates before issuing any FCP command.
TEST_F(BeBoBProtocolTest, RejectsUnsupportedRate) {
    TestBeBoBProtocol proto(busOps_, bus_, route_, nullptr, &cmp_, &timer_);
    proto.UpdateRuntimeContext(route_, nullptr);  // no FCP transport

    IOReturn status = kIOReturnSuccess;
    proto.ApplyClockConfig({.sampleRateHz = 44100},
                           [&status](IOReturn s, auto) { status = s; });
    EXPECT_EQ(status, kIOReturnUnsupported);
}

// Shutdown during settle cancels the timer and aborts the callback.
TEST_F(BeBoBProtocolTest, ShutdownCancelsSettle) {
    TestBeBoBProtocol proto(busOps_, bus_, route_, nullptr, &cmp_, &timer_);
    proto.UpdateRuntimeContext(route_, nullptr);

    IOReturn status = kIOReturnBusy;
    bool called = false;
    proto.ApplyClockConfig({.sampleRateHz = 48000},
                           [&status, &called](IOReturn s, auto) { status = s; called = true; });

    // No FCP transport → signal format fails immediately.
    EXPECT_TRUE(called);
    EXPECT_EQ(status, kIOReturnNotReady);
}

// Shutdown after the settle timer starts cancels it before it fires.
TEST_F(BeBoBProtocolTest, ShutdownAfterTimerStarts) {
    TestBeBoBProtocol proto(busOps_, bus_, route_, nullptr, &cmp_, &timer_);
    proto.UpdateRuntimeContext(route_, nullptr);

    IOReturn status = kIOReturnBusy;
    bool called = false;
    proto.ApplyClockConfig({.sampleRateHz = 48000},
                           [&status, &called](IOReturn s, auto) { status = s; called = true; });

    // Without FCP transport, signal format fails immediately — no timer started.
    EXPECT_TRUE(called);
    EXPECT_EQ(status, kIOReturnNotReady);
    EXPECT_EQ(timer_.PendingCount(), 0);
}

TEST(BeBoBProtocolInterlockTests, OutputAndInputFormatsAreSeparatedByTheConfiguredDelay) {
    ASFW::Testing::AvcTestRig rig;
    ASSERT_TRUE(rig.IsReady());
    TestBeBoBProtocol proto(rig.Bus(), rig.Bus(), rig.Route(), nullptr, nullptr, &rig.Timers());
    proto.UpdateRuntimeContext(rig.Route(), rig.Transport());

    IOReturn status = kIOReturnBusy;
    bool completed = false;
    proto.ApplyClockConfig({.sampleRateHz = 48000},
        [&status, &completed](IOReturn result, auto) {
            status = result;
            completed = true;
        });

    ASSERT_EQ(rig.Drain(), 1U);
    ASSERT_EQ(rig.Target().CommandCount(), 1U);
    EXPECT_EQ(rig.Target().Commands()[0].data[2], 0x18); // output first
    EXPECT_EQ(rig.Timers().PendingCount(), 1U);

    rig.Timers().Advance(99ULL * 1000ULL * 1000ULL);
    EXPECT_EQ(rig.Target().CommandCount(), 1U);
    rig.Timers().Advance(1ULL * 1000ULL * 1000ULL);
    ASSERT_EQ(rig.Drain(), 1U);
    ASSERT_EQ(rig.Target().CommandCount(), 2U);
    EXPECT_EQ(rig.Target().Commands()[1].data[2], 0x19); // input after interlock
    EXPECT_FALSE(completed);

    rig.Timers().Advance(300ULL * 1000ULL * 1000ULL);
    EXPECT_TRUE(completed);
    EXPECT_EQ(status, kIOReturnSuccess);
}

TEST(BeBoBProtocolInterlockTests, RouteUpdateCancelsInterlockAndCompletesApply) {
    ASFW::Testing::AvcTestRig rig;
    ASSERT_TRUE(rig.IsReady());
    TestBeBoBProtocol proto(rig.Bus(), rig.Bus(), rig.Route(), nullptr, nullptr, &rig.Timers());
    proto.UpdateRuntimeContext(rig.Route(), rig.Transport());

    IOReturn status = kIOReturnSuccess;
    bool completed = false;
    proto.ApplyClockConfig({.sampleRateHz = 48000},
        [&status, &completed](IOReturn result, auto) {
            status = result;
            completed = true;
        });
    ASSERT_EQ(rig.Drain(), 1U);
    ASSERT_EQ(rig.Timers().PendingCount(), 1U);

    proto.UpdateRuntimeContext(rig.Route(), nullptr);
    EXPECT_TRUE(completed);
    EXPECT_EQ(status, kIOReturnAborted);
    EXPECT_EQ(rig.Timers().PendingCount(), 0U);
    EXPECT_EQ(rig.Target().CommandCount(), 1U);
}

TEST(MAudioSpecialRoutingTests, BuildsOnlyTheThreeAudibleOutputRouteQuadlets) {
    const auto write = ASFW::Audio::BeBoB::BuildMAudioSpecialRoutingWrite();
    EXPECT_EQ(write.addressHi, 0xFFC7U);
    EXPECT_EQ(write.addressLo, 0x00700094U);
    EXPECT_EQ(write.bytes.size(), 12U);
    constexpr std::array<uint8_t, 12> expected{
        0x00, 0x00, 0x00, 0x09, // streams 1/2->mixer 1, 3/4->mixer 2
        0x00, 0x02, 0x00, 0x01, // headphone pair 1/2 follow mixer pairs
        0x00, 0x00, 0x00, 0x00, // analog outputs follow mixer
    };
    EXPECT_EQ(write.bytes, expected);
}

TEST(MAudioSpecialRoutingTests, AcceptedClockAndFormatsPrecedeRoutingWrite) {
    ASFW::Testing::AvcTestRig rig;
    ASSERT_TRUE(rig.IsReady());
    const auto route = rig.Route();
    ASSERT_TRUE(static_cast<bool>(route));

    ASFW::Audio::BeBoB::MAudioSpecialProtocol protocol(
        rig.Bus(), rig.Bus(), route, nullptr, nullptr, &rig.Timers(), true);
    protocol.UpdateRuntimeContext(route, rig.Transport());

    IOReturn result = kIOReturnBusy;
    bool completed = false;
    protocol.ApplyClockConfig({.sampleRateHz = 48000},
        [&result, &completed](IOReturn status, auto) {
            result = status;
            completed = true;
        });

    // The initial vendor clock command is accepted, then both standard
    // signal-format CONTROL commands complete with their 100 ms interlock.
    ASSERT_EQ(rig.Drain(), 2U);
    ASSERT_EQ(rig.Target().CommandCount(), 2U);
    EXPECT_EQ(rig.Target().Commands()[0].data[2], 0x00); // vendor clock opcode
    EXPECT_EQ(rig.Target().Commands()[1].data[2], 0x18); // output format
    rig.Timers().Advance(100ULL * 1000ULL * 1000ULL);
    ASSERT_EQ(rig.Drain(1), 1U);
    ASSERT_EQ(rig.Target().CommandCount(), 3U);
    EXPECT_EQ(rig.Target().Commands()[2].data[2], 0x19); // input format

    // ConfigureMixer must run only after those accepted FCP exchanges, and it
    // writes just the 12-byte route span at the generation-qualified node.
    ASSERT_EQ(rig.Bus().PendingWriteCount(), 1U);
    const auto& pending = rig.Bus().PendingWriteAt(0);
    const auto expected = ASFW::Audio::BeBoB::BuildMAudioSpecialRoutingWrite();
    EXPECT_EQ(pending.generation, route.generation);
    EXPECT_EQ(pending.nodeId.value, route.nodeId);
    EXPECT_EQ(pending.address.addressHi, expected.addressHi);
    EXPECT_EQ(pending.address.addressLo, expected.addressLo);
    EXPECT_EQ(pending.data,
              std::vector<uint8_t>(expected.bytes.begin(), expected.bytes.end()));
    EXPECT_FALSE(completed);

    const auto handle = pending.handle;
    ASSERT_TRUE(rig.Bus().CompleteWrite(
        handle, ASFW::Async::AsyncStatus::kSuccess));
    rig.Timers().Advance(300ULL * 1000ULL * 1000ULL);
    EXPECT_TRUE(completed);
    EXPECT_EQ(result, kIOReturnSuccess);
}

TEST(MAudioSpecialRoutingTests, Captured1814HappyPathReplaysThroughRealFcpAndCmp) {
    namespace Capture = MAudioSpecialHappyPathFixture;
    using Capture::EventKind;
    ASFW::Testing::AvcTestRig rig;
    ASSERT_TRUE(rig.IsReady());
    ASFW::CMP::CMPClient cmp(rig.Bus(), rig.Bus(), rig.Routes());
    const auto route = rig.Route();
    ASFW::Audio::BeBoB::MAudioSpecialProtocol protocol(
        rig.Bus(), rig.Bus(), route, nullptr, &cmp, &rig.Timers(), true);
    protocol.UpdateRuntimeContext(route, rig.Transport());

    std::vector<const Capture::Event*> capturedCommands;
    const Capture::Event* capturedRoute = nullptr;
    const Capture::Event* capturedTx = nullptr;
    const Capture::Event* capturedRx = nullptr;
    for (const auto& event : Capture::kEvents) {
        if (event.kind == EventKind::Bwrite && event.src == 0xffc0 &&
            event.dst == 0xffc2 && (event.address & 0xffff'ffff'ffffULL) ==
                                       0xffff'f000'0b00ULL) {
            capturedCommands.push_back(&event);
        }
        if (event.kind == EventKind::Bwrite && event.src == 0xffc0 &&
            event.dst == 0xffc2 && (event.address & 0xffff'ffff'ffffULL) ==
                                       0xffc7'0070'0000ULL) {
            capturedRoute = &event;
        }
        if (event.kind == EventKind::IsochPacket && event.dst == 0 &&
            event.size == 232 && !capturedTx) capturedTx = &event;
        if (event.kind == EventKind::IsochPacket && event.dst == 1 &&
            event.size == 8 && !capturedRx) capturedRx = &event;
    }
    ASSERT_EQ(capturedCommands.size(), 5U);
    ASSERT_NE(capturedRoute, nullptr);
    ASSERT_NE(capturedTx, nullptr);
    ASSERT_NE(capturedRx, nullptr);
    const auto capturedReply = [](uint64_t address, bool last) -> uint32_t {
        uint32_t value = 0;
        for (const auto& event : Capture::kEvents) {
            if (event.kind == EventKind::QRresp &&
                (event.address & 0xffff'ffff'ffffULL) == address) {
                value = event.value;
                if (!last) break;
            }
        }
        return value;
    };
    EXPECT_LT(capturedCommands[0]->elapsedCycles, capturedRoute->elapsedCycles);
    EXPECT_LT(capturedRoute->elapsedCycles, capturedTx->elapsedCycles);
    EXPECT_LT(capturedTx->elapsedCycles, capturedCommands[3]->elapsedCycles);
    EXPECT_LT(capturedCommands[3]->elapsedCycles, capturedRx->elapsedCycles);
    EXPECT_LT(capturedCommands[4]->elapsedCycles, capturedRx->elapsedCycles);

    bool clockReady = false;
    IOReturn clockStatus = kIOReturnBusy;
    protocol.ApplyClockConfig({.sampleRateHz = 48000},
        [&](IOReturn status, auto) { clockStatus = status; clockReady = true; });
    ASSERT_EQ(rig.Drain(), 2U); // vendor clock, then output format
    ASSERT_EQ(rig.Target().CommandCount(), 2U);
    rig.Timers().Advance(99'000'000ULL);
    EXPECT_EQ(rig.Target().CommandCount(), 2U);
    rig.Timers().Advance(1'000'000ULL);
    ASSERT_EQ(rig.Drain(1), 1U); // pre-stream input format
    ASSERT_EQ(rig.Bus().PendingWriteCount(), 1U); // route span
    const auto routeWrite = rig.Bus().PendingWriteAt(0);
    ASSERT_EQ(capturedRoute->payloadSize, 160U);
    constexpr size_t kRouteSpanOffset = 0x94;
    const auto expectedRoute = ASFW::Audio::BeBoB::BuildMAudioSpecialRoutingWrite();
    EXPECT_EQ(routeWrite.address.addressHi, expectedRoute.addressHi);
    EXPECT_EQ(routeWrite.address.addressLo, expectedRoute.addressLo);
    EXPECT_TRUE(std::equal(expectedRoute.bytes.begin(), expectedRoute.bytes.end(),
                           capturedRoute->payload + kRouteSpanOffset));
    ASSERT_TRUE(rig.Bus().CompleteWrite(routeWrite.handle, AsyncStatus::kSuccess));
    rig.Timers().Advance(300'000'000ULL);
    ASSERT_TRUE(clockReady);
    ASSERT_EQ(clockStatus, kIOReturnSuccess);

    ASFW::Audio::AudioDuplexChannels channels{};
    channels.hostToDeviceIsoChannel = 0;
    channels.deviceToHostIsoChannel = 1;
    protocol.SetAssignedChannels(channels);
    rig.Bus().MapReadQuadlet(0xffff'f000'0980ULL,
                             capturedReply(0xffff'f000'0980ULL, false));
    rig.Bus().MapReadQuadlet(0xffff'f000'0900ULL,
                             capturedReply(0xffff'f000'0900ULL, false));
    rig.Bus().MapReadQuadlet(0xffff'f000'0984ULL,
                             capturedReply(0xffff'f000'0984ULL, false));
    rig.Bus().MapReadQuadlet(0xffff'f000'0904ULL,
                             capturedReply(0xffff'f000'0904ULL, false));
    bool playbackConnected = false;
    protocol.ProgramRx([&](IOReturn status, auto) {
        EXPECT_EQ(status, kIOReturnSuccess);
        playbackConnected = true;
    });
    ASSERT_TRUE(playbackConnected);
    bool captureConnected = false;
    protocol.ProgramTxAndEnableDuplex([&](IOReturn status, auto) {
        EXPECT_EQ(status, kIOReturnSuccess);
        captureConnected = true;
    });
    ASSERT_TRUE(captureConnected);
    const Capture::Event* capturedOutputCas = nullptr;
    for (const auto& event : Capture::kEvents) {
        if (event.kind == EventKind::LockRq &&
            (event.address & 0xffff'ffff'ffffULL) == 0xffff'f000'0904ULL) {
            capturedOutputCas = &event;
        }
    }
    ASSERT_NE(capturedOutputCas, nullptr);
    EXPECT_EQ(rig.Bus().LastLockOperand(),
              std::vector<uint8_t>(capturedOutputCas->payload,
                                   capturedOutputCas->payload + capturedOutputCas->payloadSize));
    rig.Bus().MapReadQuadlet(0xffff'f000'0984ULL,
                             capturedReply(0xffff'f000'0984ULL, true));
    rig.Bus().MapReadQuadlet(0xffff'f000'0904ULL,
                             capturedReply(0xffff'f000'0904ULL, true));

    // The captured output reply is final; the captured input reply is INTERIM
    // then ACCEPTED. Hold that final response to test the real FCP completion.
    rig.Target().Script(ASFW::Testing::AvcReply::Accepted());
    rig.Target().Script(ASFW::Testing::AvcReply::Interim());
    bool confirmed = false;
    IOReturn confirmStatus = kIOReturnBusy;
    protocol.ConfirmDuplexStart([&](IOReturn status, auto) {
        confirmStatus = status;
        confirmed = true;
    });
    ASSERT_EQ(rig.Drain(1), 1U); // post-start output format
    rig.Timers().Advance(99'000'000ULL);
    EXPECT_EQ(rig.Target().CommandCount(), 4U);
    rig.Timers().Advance(1'000'000ULL);
    ASSERT_EQ(rig.Drain(1), 1U); // post-start input INTERIM
    EXPECT_FALSE(confirmed);
    for (const auto& event : Capture::kEvents) {
        if (event.kind == EventKind::Bwrite && event.src == 0xffc2 &&
            event.dst == 0xffc0 && event.payloadSize == 8 &&
            event.payload[0] == 0x09 && event.payload[2] == 0x19 &&
            event.elapsedCycles > capturedCommands[4]->elapsedCycles) {
            rig.Transport()->OnFCPResponse(2, 1,
                std::vector<uint8_t>(event.payload, event.payload + event.payloadSize));
            break;
        }
    }
    EXPECT_TRUE(confirmed);
    EXPECT_EQ(confirmStatus, kIOReturnSuccess);
    ASSERT_EQ(rig.Target().CommandCount(), capturedCommands.size());
    for (size_t i = 0; i < capturedCommands.size(); ++i) {
        const auto& actual = rig.Target().Commands()[i];
        const auto& expected = *capturedCommands[i];
        ASSERT_EQ(actual.length, expected.payloadSize) << i;
        EXPECT_TRUE(std::equal(actual.data.begin(), actual.data.begin() + actual.length,
                               expected.payload)) << i;
    }
}

TEST_F(BeBoBProtocolTest, ReadClockHealthReportsNominalRateAndDisconnectedState) {
    TestBeBoBProtocol proto(busOps_, bus_, route_, nullptr, &cmp_, &timer_);
    proto.UpdateRuntimeContext(route_, nullptr);
    proto.SetCaps({.hostInputPcmChannels = 8, .hostOutputPcmChannels = 8, .sampleRateHz = 48000});

    bool called = false;
    DuplexHealthResult healthResult{};
    proto.ReadDuplexHealth([&](IOReturn status, DuplexHealthResult result) {
        called = true;
        EXPECT_EQ(status, kIOReturnSuccess);
        healthResult = result;
    });

    EXPECT_TRUE(called);
    EXPECT_EQ(healthResult.generation, Generation{1});
    EXPECT_EQ(healthResult.nominalRateHz, 48000U);
    EXPECT_TRUE(healthResult.clockReferenceHealthy);
    EXPECT_FALSE(healthResult.sourceLocked);
    EXPECT_EQ(healthResult.runtimeCaps.hostInputPcmChannels, 8U);
    EXPECT_EQ(healthResult.runtimeCaps.hostOutputPcmChannels, 8U);
}

TEST_F(BeBoBProtocolTest, ReadClockHealthReportsSourceLockedWhenBothConnected) {
    TestBeBoBProtocol proto(busOps_, bus_, route_, nullptr, &cmp_, &timer_);
    proto.UpdateRuntimeContext(route_, nullptr);
    proto.SetCaps({.hostInputPcmChannels = 4, .hostOutputPcmChannels = 4, .sampleRateHz = 96000});
    proto.SetConnected(true, true);

    bool called = false;
    DuplexHealthResult healthResult{};
    proto.ReadDuplexHealth([&](IOReturn status, DuplexHealthResult result) {
        called = true;
        EXPECT_EQ(status, kIOReturnSuccess);
        healthResult = result;
    });

    EXPECT_TRUE(called);
    EXPECT_EQ(healthResult.nominalRateHz, 96000U);
    EXPECT_TRUE(healthResult.clockReferenceHealthy);
    EXPECT_TRUE(healthResult.sourceLocked);
    EXPECT_EQ(healthResult.runtimeCaps.hostInputPcmChannels, 4U);
    EXPECT_EQ(healthResult.runtimeCaps.hostOutputPcmChannels, 4U);
}

// FamilyDriver steps start the same callback chains and wait for them.
TEST_F(BeBoBProtocolTest, FamilyDriverStepsAnswerThroughTheSameChains) {
    TestBeBoBProtocol proto(busOps_, bus_, route_, nullptr, &cmp_, &timer_);
    proto.UpdateRuntimeContext(route_, nullptr);  // no FCP transport
    proto.SetCaps({.hostInputPcmChannels = 8, .hostOutputPcmChannels = 8, .sampleRateHz = 48000});
    ASFW::Audio::FamilyDriver& family = *proto.AsFamilyDriver();

    EXPECT_EQ(family.LoadGeometry(), kIOReturnSuccess);
    const auto health = family.ReadHealth(1000);
    ASSERT_TRUE(health.has_value());
    EXPECT_EQ(health->nominalRateHz, 48000U);
    EXPECT_FALSE(health->sourceLocked);

    const auto unsupported = family.ApplyClockIdle({.sampleRateHz = 44100});
    ASSERT_FALSE(unsupported.has_value());
    EXPECT_EQ(unsupported.error(), kIOReturnUnsupported);
    const auto noTransport = family.ApplyClockIdle({.sampleRateHz = 48000});
    ASSERT_FALSE(noTransport.has_value());
    EXPECT_EQ(noTransport.error(), kIOReturnNotReady);
}

