// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// GAP 1: CMPClient::ConnectOPCR channel-in-CAS tests.
// Verifies that the IRM-allocated channel number is written into the device's oPCR
// during the CAS operation, per IEC 61883-1 §10.4.2.

#include <gtest/gtest.h>

#include "Protocols/AVC/CMP/CMPClient.hpp"
#include "Async/Interfaces/IFireWireBusOps.hpp"
#include "IRM/IRMTypes.hpp"

#include <cstdint>
#include <cstring>
#include <deque>
#include <span>
#include <vector>

namespace {

// Packs a uint32_t as 4 big-endian bytes (matches bus wire format).
std::vector<uint8_t> BEBytes(uint32_t value) {
    return {
        static_cast<uint8_t>(value >> 24),
        static_cast<uint8_t>(value >> 16),
        static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value),
    };
}

// Synchronous mock bus: calls every callback inline, in the same call stack.
// ReadBlock and Lock drain from response queues; unexpected calls fail the test.
struct SyncMockBus : public ASFW::Async::IFireWireBusOps {
    std::deque<std::vector<uint8_t>> readResponses;
    std::deque<std::vector<uint8_t>> lockResponses;

    // Captured Lock operands ([compare(4B BE)][swap(4B BE)]) for post-call assertions.
    std::vector<std::vector<uint8_t>> lockOperands;

    ASFW::Async::AsyncHandle ReadBlock(ASFW::FW::Generation,
                                       ASFW::FW::NodeId,
                                       ASFW::Async::FWAddress,
                                       uint32_t,
                                       ASFW::FW::FwSpeed,
                                       ASFW::Async::InterfaceCompletionCallback cb) override {
        EXPECT_FALSE(readResponses.empty()) << "Unexpected ReadBlock call";
        if (!readResponses.empty()) {
            auto r = std::move(readResponses.front());
            readResponses.pop_front();
            cb(ASFW::Async::AsyncStatus::kSuccess, std::span(r));
        } else {
            cb(ASFW::Async::AsyncStatus::kHardwareError, {});
        }
        return ASFW::Async::AsyncHandle{0};
    }

    ASFW::Async::AsyncHandle WriteBlock(ASFW::FW::Generation,
                                        ASFW::FW::NodeId,
                                        ASFW::Async::FWAddress,
                                        std::span<const uint8_t>,
                                        ASFW::FW::FwSpeed,
                                        ASFW::Async::InterfaceCompletionCallback cb) override {
        cb(ASFW::Async::AsyncStatus::kSuccess, {});
        return ASFW::Async::AsyncHandle{0};
    }

    ASFW::Async::AsyncHandle Lock(ASFW::FW::Generation,
                                  ASFW::FW::NodeId,
                                  ASFW::Async::FWAddress,
                                  ASFW::FW::LockOp,
                                  std::span<const uint8_t> operand,
                                  uint32_t,
                                  ASFW::FW::FwSpeed,
                                  ASFW::Async::InterfaceCompletionCallback cb) override {
        lockOperands.push_back(std::vector<uint8_t>(operand.begin(), operand.end()));
        EXPECT_FALSE(lockResponses.empty()) << "Unexpected Lock call";
        if (!lockResponses.empty()) {
            auto r = std::move(lockResponses.front());
            lockResponses.pop_front();
            cb(ASFW::Async::AsyncStatus::kSuccess, std::span(r));
        } else {
            cb(ASFW::Async::AsyncStatus::kHardwareError, {});
        }
        return ASFW::Async::AsyncHandle{0};
    }

    bool Cancel(ASFW::Async::AsyncHandle) override { return false; }

    // Decode the "swap" portion of a CAS operand as a host-endian uint32.
    static uint32_t SwapValue(const std::vector<uint8_t>& op) {
        EXPECT_GE(op.size(), 8u);
        uint32_t raw = 0;
        std::memcpy(&raw, op.data() + 4, 4);
        return OSSwapBigToHostInt32(raw);
    }

    // Decode the "compare" portion of a CAS operand as a host-endian uint32.
    static uint32_t CompareValue(const std::vector<uint8_t>& op) {
        EXPECT_GE(op.size(), 8u);
        uint32_t raw = 0;
        std::memcpy(&raw, op.data(), 4);
        return OSSwapBigToHostInt32(raw);
    }
};

// Build a PCR register value with given online, p2p, and channel fields.
constexpr uint32_t MakePCR(bool online, uint8_t p2p, uint8_t channel) {
    return (online ? 0x80000000u : 0u)
         | ((static_cast<uint32_t>(p2p)     & 0x03u) << 24)
         | ((static_cast<uint32_t>(channel) & 0x3Fu) << 16);
}

constexpr uint8_t kChannelShift = 16;
constexpr uint32_t kChannelMask = 0x003F0000u;
constexpr uint8_t kP2PShift     = 24;
constexpr uint32_t kP2PMask     = 0x03000000u;

} // namespace

// ============================================================================
// ConnectOPCR — channel written into oPCR CAS
// ============================================================================

TEST(CMPClientTests, ConnectOPCR_WritesChannelIntoDesiredValue) {
    SyncMockBus bus;
    ASFW::CMP::CMPClient cmp(bus);
    cmp.SetDeviceNode(5, ASFW::IRM::Generation{3});

    constexpr uint8_t kChannel = 7;
    const uint32_t onlinePCR = MakePCR(true, 0, 0);

    bus.readResponses.push_back(BEBytes(onlinePCR));
    // CAS response: return the "old" value == compare value → success.
    bus.lockResponses.push_back(BEBytes(onlinePCR));

    ASFW::CMP::CMPStatus result = ASFW::CMP::CMPStatus::Failed;
    cmp.ConnectOPCR(0, kChannel, [&](ASFW::CMP::CMPStatus s) { result = s; });

    ASSERT_EQ(result, ASFW::CMP::CMPStatus::Success);
    ASSERT_EQ(bus.lockOperands.size(), 1u);

    const uint32_t swapVal = SyncMockBus::SwapValue(bus.lockOperands[0]);
    EXPECT_EQ((swapVal & kChannelMask) >> kChannelShift, static_cast<uint32_t>(kChannel))
        << "Channel bits not written into oPCR CAS desired value";
    EXPECT_EQ((swapVal & kP2PMask) >> kP2PShift, 1u) << "p2p count should be incremented to 1";
}

TEST(CMPClientTests, ConnectOPCR_Channel63_WrittenCorrectly) {
    SyncMockBus bus;
    ASFW::CMP::CMPClient cmp(bus);
    cmp.SetDeviceNode(2, ASFW::IRM::Generation{1});

    const uint32_t onlinePCR = MakePCR(true, 0, 0);
    bus.readResponses.push_back(BEBytes(onlinePCR));
    bus.lockResponses.push_back(BEBytes(onlinePCR));

    ASFW::CMP::CMPStatus result = ASFW::CMP::CMPStatus::Failed;
    cmp.ConnectOPCR(0, 63, [&](ASFW::CMP::CMPStatus s) { result = s; });

    ASSERT_EQ(result, ASFW::CMP::CMPStatus::Success);
    ASSERT_EQ(bus.lockOperands.size(), 1u);
    const uint32_t swapCh = (SyncMockBus::SwapValue(bus.lockOperands[0]) & kChannelMask) >> kChannelShift;
    EXPECT_EQ(swapCh, 63u);
}

TEST(CMPClientTests, ConnectOPCR_CompareValueMatchesReadValue) {
    SyncMockBus bus;
    ASFW::CMP::CMPClient cmp(bus);
    cmp.SetDeviceNode(5, ASFW::IRM::Generation{3});

    // PCR with online=1, p2p=2, ch=10 — existing state preserved in compare.
    const uint32_t existingPCR = MakePCR(true, 2, 10);
    bus.readResponses.push_back(BEBytes(existingPCR));
    bus.lockResponses.push_back(BEBytes(existingPCR));

    ASFW::CMP::CMPStatus result = ASFW::CMP::CMPStatus::Failed;
    cmp.ConnectOPCR(0, 5, [&](ASFW::CMP::CMPStatus s) { result = s; });

    ASSERT_EQ(result, ASFW::CMP::CMPStatus::Success);
    ASSERT_EQ(bus.lockOperands.size(), 1u);
    EXPECT_EQ(SyncMockBus::CompareValue(bus.lockOperands[0]), existingPCR);
}

// ============================================================================
// ConnectOPCR — invalid arguments rejected before any bus op
// ============================================================================

TEST(CMPClientTests, ConnectOPCR_ChannelTooLarge_ImmediateFailed) {
    SyncMockBus bus;
    ASFW::CMP::CMPClient cmp(bus);

    ASFW::CMP::CMPStatus result = ASFW::CMP::CMPStatus::Success;
    cmp.ConnectOPCR(0, 64, [&](ASFW::CMP::CMPStatus s) { result = s; });

    EXPECT_EQ(result, ASFW::CMP::CMPStatus::Failed);
    EXPECT_TRUE(bus.lockOperands.empty()) << "No bus op should be issued for invalid channel";
}

TEST(CMPClientTests, ConnectOPCR_PlugTooLarge_ImmediateFailed) {
    SyncMockBus bus;
    ASFW::CMP::CMPClient cmp(bus);

    ASFW::CMP::CMPStatus result = ASFW::CMP::CMPStatus::Success;
    cmp.ConnectOPCR(31, 0, [&](ASFW::CMP::CMPStatus s) { result = s; });

    EXPECT_EQ(result, ASFW::CMP::CMPStatus::Failed);
    EXPECT_TRUE(bus.lockOperands.empty()) << "No bus op should be issued for invalid plug";
}

// ============================================================================
// ConnectOPCR — PerformConnect guards (offline, p2p maxed)
// ============================================================================

TEST(CMPClientTests, ConnectOPCR_OfflinePlug_FailsWithoutCAS) {
    SyncMockBus bus;
    ASFW::CMP::CMPClient cmp(bus);
    cmp.SetDeviceNode(5, ASFW::IRM::Generation{3});

    bus.readResponses.push_back(BEBytes(MakePCR(false, 0, 0)));

    ASFW::CMP::CMPStatus result = ASFW::CMP::CMPStatus::Success;
    cmp.ConnectOPCR(0, 5, [&](ASFW::CMP::CMPStatus s) { result = s; });

    EXPECT_EQ(result, ASFW::CMP::CMPStatus::Failed);
    EXPECT_TRUE(bus.lockOperands.empty()) << "CAS should not be issued when plug is offline";
}

TEST(CMPClientTests, ConnectOPCR_P2PMaxed_ReturnsNoResources) {
    SyncMockBus bus;
    ASFW::CMP::CMPClient cmp(bus);
    cmp.SetDeviceNode(5, ASFW::IRM::Generation{3});

    bus.readResponses.push_back(BEBytes(MakePCR(true, 3, 0)));

    ASFW::CMP::CMPStatus result = ASFW::CMP::CMPStatus::Success;
    cmp.ConnectOPCR(0, 5, [&](ASFW::CMP::CMPStatus s) { result = s; });

    EXPECT_EQ(result, ASFW::CMP::CMPStatus::NoResources);
    EXPECT_TRUE(bus.lockOperands.empty());
}

// ============================================================================
// ConnectIPCR — channel written into iPCR CAS (symmetric verification)
// ============================================================================

TEST(CMPClientTests, ConnectIPCR_WritesChannelIntoDesiredValue) {
    SyncMockBus bus;
    ASFW::CMP::CMPClient cmp(bus);
    cmp.SetDeviceNode(5, ASFW::IRM::Generation{3});

    constexpr uint8_t kChannel = 1;
    const uint32_t onlinePCR = MakePCR(true, 0, 0);
    bus.readResponses.push_back(BEBytes(onlinePCR));
    bus.lockResponses.push_back(BEBytes(onlinePCR));

    ASFW::CMP::CMPStatus result = ASFW::CMP::CMPStatus::Failed;
    cmp.ConnectIPCR(0, kChannel, [&](ASFW::CMP::CMPStatus s) { result = s; });

    ASSERT_EQ(result, ASFW::CMP::CMPStatus::Success);
    ASSERT_EQ(bus.lockOperands.size(), 1u);
    const uint32_t swapCh = (SyncMockBus::SwapValue(bus.lockOperands[0]) & kChannelMask) >> kChannelShift;
    EXPECT_EQ(swapCh, kChannel);
}

TEST(CMPClientTests, ConnectIPCR_ChannelTooLarge_ImmediateFailed) {
    SyncMockBus bus;
    ASFW::CMP::CMPClient cmp(bus);

    ASFW::CMP::CMPStatus result = ASFW::CMP::CMPStatus::Success;
    cmp.ConnectIPCR(0, 64, [&](ASFW::CMP::CMPStatus s) { result = s; });

    EXPECT_EQ(result, ASFW::CMP::CMPStatus::Failed);
    EXPECT_TRUE(bus.lockOperands.empty());
}

// ============================================================================
// GAP 3 — ReadOPCR read-back after ConnectOPCR
// ============================================================================

TEST(CMPClientTests, ReadOPCR_ReturnsChannelFromDevice) {
    SyncMockBus bus;
    ASFW::CMP::CMPClient cmp(bus);
    cmp.SetDeviceNode(5, ASFW::IRM::Generation{3});

    constexpr uint8_t kExpectedChannel = 7;
    bus.readResponses.push_back(BEBytes(MakePCR(true, 1, kExpectedChannel)));

    bool callbackCalled = false;
    bool readOk = false;
    uint32_t opcrValue = 0;
    cmp.ReadOPCR(0, [&](bool success, uint32_t value) {
        callbackCalled = true;
        readOk = success;
        opcrValue = value;
    });

    ASSERT_TRUE(callbackCalled);
    ASSERT_TRUE(readOk);
    EXPECT_EQ(ASFW::CMP::PCRBits::GetChannel(opcrValue), kExpectedChannel);
    EXPECT_EQ(ASFW::CMP::PCRBits::GetP2P(opcrValue), 1u);
    EXPECT_TRUE(ASFW::CMP::PCRBits::IsOnline(opcrValue));
}

TEST(CMPClientTests, ReadOPCR_BusFailure_CallbackSuccessFalse) {
    struct FailBus : public ASFW::Async::IFireWireBusOps {
        ASFW::Async::AsyncHandle ReadBlock(ASFW::FW::Generation, ASFW::FW::NodeId,
                                           ASFW::Async::FWAddress, uint32_t,
                                           ASFW::FW::FwSpeed,
                                           ASFW::Async::InterfaceCompletionCallback cb) override {
            cb(ASFW::Async::AsyncStatus::kHardwareError, {});
            return ASFW::Async::AsyncHandle{0};
        }
        ASFW::Async::AsyncHandle WriteBlock(ASFW::FW::Generation, ASFW::FW::NodeId,
                                             ASFW::Async::FWAddress, std::span<const uint8_t>,
                                             ASFW::FW::FwSpeed,
                                             ASFW::Async::InterfaceCompletionCallback cb) override {
            cb(ASFW::Async::AsyncStatus::kSuccess, {}); return ASFW::Async::AsyncHandle{0};
        }
        ASFW::Async::AsyncHandle Lock(ASFW::FW::Generation, ASFW::FW::NodeId,
                                      ASFW::Async::FWAddress, ASFW::FW::LockOp,
                                      std::span<const uint8_t>, uint32_t, ASFW::FW::FwSpeed,
                                      ASFW::Async::InterfaceCompletionCallback cb) override {
            cb(ASFW::Async::AsyncStatus::kSuccess, {}); return ASFW::Async::AsyncHandle{0};
        }
        bool Cancel(ASFW::Async::AsyncHandle) override { return false; }
    } failBus;

    ASFW::CMP::CMPClient cmp(failBus);
    cmp.SetDeviceNode(1, ASFW::IRM::Generation{1});

    bool callbackCalled = false;
    bool readOk = true;
    cmp.ReadOPCR(0, [&](bool success, uint32_t) {
        callbackCalled = true;
        readOk = success;
    });

    ASSERT_TRUE(callbackCalled);
    EXPECT_FALSE(readOk);
}

TEST(CMPClientTests, ReadOPCR_InvalidPlug_ImmediateFailure) {
    SyncMockBus bus;
    ASFW::CMP::CMPClient cmp(bus);

    bool callbackCalled = false;
    bool readOk = true;
    cmp.ReadOPCR(31, [&](bool success, uint32_t) {
        callbackCalled = true;
        readOk = success;
    });

    ASSERT_TRUE(callbackCalled);
    EXPECT_FALSE(readOk);
    EXPECT_TRUE(bus.readResponses.empty()) << "No bus op should be issued for invalid plug";
}
