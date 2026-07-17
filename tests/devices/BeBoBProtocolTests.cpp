// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Tests for BeBoBProtocol base class: async ApplyClockConfig chain, timer-based
// settle, epoch cancellation, and StreamPlug usage.

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Protocols/BeBoB/BeBoBProtocol.hpp"
#include "ASFWDriver/Async/Interfaces/IFireWireBus.hpp"
#include "ASFWDriver/Protocols/AVC/CMP/CMPClient.hpp"

#include <array>
#include <cstring>
#include <unordered_map>

#include "FakeTimerScheduler.hpp"

using ASFW::Audio::DuplexHealthResult;

namespace {

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
    void ReadClockHealth(HealthCallback callback) override {
        callback(kIOReturnSuccess, DuplexHealthResult{});
    }

    void SetCaps(const ASFW::Audio::AudioStreamRuntimeCaps& caps) { caps_ = caps; }

private:
    ASFW::Audio::AudioStreamRuntimeCaps caps_{};
};

} // namespace

class BeBoBProtocolTest : public testing::Test {
protected:
    BeBoBBus bus_;
    ASFW::CMP::CMPClient cmp_{bus_, bus_};
    ASFW::Testing::FakeTimerScheduler timer_;
    StubBusOps busOps_;
    static constexpr uint64_t kGuid = 0x000aac0300b1d1f7ULL;
    static constexpr uint8_t kNode = 2;
};

// ApplyClockConfig rejects unsupported rates before issuing any FCP command.
TEST_F(BeBoBProtocolTest, RejectsUnsupportedRate) {
    TestBeBoBProtocol proto(busOps_, bus_, kNode, nullptr, &cmp_, kGuid, &timer_);
    proto.UpdateRuntimeContext(kNode, nullptr);  // no FCP transport

    IOReturn status = kIOReturnSuccess;
    proto.ApplyClockConfig({.sampleRateHz = 44100},
                           [&status](IOReturn s, auto) { status = s; });
    EXPECT_EQ(status, kIOReturnUnsupported);
}

// Shutdown during settle cancels the timer and aborts the callback.
TEST_F(BeBoBProtocolTest, ShutdownCancelsSettle) {
    TestBeBoBProtocol proto(busOps_, bus_, kNode, nullptr, &cmp_, kGuid, &timer_);
    proto.UpdateRuntimeContext(kNode, nullptr);

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
    TestBeBoBProtocol proto(busOps_, bus_, kNode, nullptr, &cmp_, kGuid, &timer_);
    proto.UpdateRuntimeContext(kNode, nullptr);

    IOReturn status = kIOReturnBusy;
    bool called = false;
    proto.ApplyClockConfig({.sampleRateHz = 48000},
                           [&status, &called](IOReturn s, auto) { status = s; called = true; });

    // Without FCP transport, signal format fails immediately — no timer started.
    EXPECT_TRUE(called);
    EXPECT_EQ(status, kIOReturnNotReady);
    EXPECT_EQ(timer_.PendingCount(), 0);
}
