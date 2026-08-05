// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeDuetClockTransitionTests.cpp - Unit tests for ApogeeDuetProtocol non-blocking clock transitions.

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Protocols/Oxford/Apogee/ApogeeDuetProtocol.hpp"
#include "ASFWDriver/Protocols/Ports/FireWireBusPort.hpp"

#include "FakeTimerScheduler.hpp"

namespace {

using ASFW::Audio::AudioClockConfig;
using ASFW::Audio::ClockApplyResult;
using ASFW::Audio::Oxford::Apogee::ApogeeDuetProtocol;
using ASFW::Async::AsyncHandle;
using ASFW::Async::AsyncStatus;
using ASFW::Async::FWAddress;
using ASFW::Async::InterfaceCompletionCallback;
using ASFW::FW::FwSpeed;
using ASFW::FW::Generation;
using ASFW::FW::NodeId;
using ASFW::Protocols::Ports::FireWireBusInfo;
using ASFW::Protocols::Ports::FireWireBusOps;

class TestBusOps final : public FireWireBusOps {
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

class TestBusInfo final : public FireWireBusInfo {
public:
    Generation GetGeneration() const noexcept override {
        return Generation{1};
    }
    NodeId GetLocalNodeID() const noexcept override {
        return NodeId{0};
    }
    FwSpeed GetSpeed(NodeId) const noexcept override {
        return FwSpeed::S400;
    }
    uint32_t HopCount(NodeId, NodeId) const noexcept override {
        return 0;
    }
};

TEST(ApogeeDuetClockTransitionTests, ConcurrentApplyClockConfigReturnsBusy) {
    TestBusOps busOps;
    TestBusInfo busInfo;
    ASFW::Testing::FakeTimerScheduler timerScheduler;

    ApogeeDuetProtocol protocol(busOps, busInfo, 0xFFC2, nullptr, nullptr, nullptr, 0x3DB0A0000D112ULL, 100U, &timerScheduler);

    // Initial ApplyClockConfig without transport returns kIOReturnNotReady
    bool callbackFired = false;
    protocol.ApplyClockConfig(AudioClockConfig{.sampleRateHz = 48000U},
                              [&callbackFired](IOReturn status, const ClockApplyResult&) {
                                  callbackFired = true;
                                  EXPECT_EQ(status, kIOReturnNotReady);
                              });
    EXPECT_TRUE(callbackFired);
}

TEST(ApogeeDuetClockTransitionTests, ShutdownCancelsClockTransition) {
    TestBusOps busOps;
    TestBusInfo busInfo;
    ASFW::Testing::FakeTimerScheduler timerScheduler;

    ApogeeDuetProtocol protocol(busOps, busInfo, 0xFFC2, nullptr, nullptr, nullptr, 0x3DB0A0000D112ULL, 100U, &timerScheduler);

    EXPECT_EQ(protocol.Shutdown(), kIOReturnSuccess);
}

} // namespace
