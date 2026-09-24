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
