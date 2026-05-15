// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// GAP 2: IRMClient allocation/release tests.
// Verifies bandwidth + channel allocation sequence, rollback on partial failure,
// and resource release — the behavior AVCAudioBackend depends on.

#include <gtest/gtest.h>

#include "IRM/IRMClient.hpp"
#include "IRM/IRMTypes.hpp"
#include "Async/Interfaces/IFireWireBusOps.hpp"

#include <cstdint>
#include <cstring>
#include <deque>
#include <span>
#include <vector>

namespace {

// Packs a uint32_t as 4 big-endian bytes (bus wire format).
std::vector<uint8_t> BEBytes(uint32_t v) {
    return {
        static_cast<uint8_t>(v >> 24),
        static_cast<uint8_t>(v >> 16),
        static_cast<uint8_t>(v >> 8),
        static_cast<uint8_t>(v),
    };
}

// Synchronous mock bus. Callbacks are invoked inline in the same call stack.
// Each Read or Lock call pops the next response from the corresponding queue;
// unexpected calls fail the test immediately.
class SyncMockBus : public ASFW::Async::IFireWireBusOps {
public:
    std::deque<std::vector<uint8_t>> readResponses;
    std::deque<std::vector<uint8_t>> lockResponses;

    // Addresses targeted by reads and locks, for sequencing assertions.
    std::vector<uint32_t> readAddresses;
    std::vector<uint32_t> lockAddresses;

    ASFW::Async::AsyncHandle ReadBlock(ASFW::FW::Generation,
                                       ASFW::FW::NodeId,
                                       ASFW::Async::FWAddress addr,
                                       uint32_t,
                                       ASFW::FW::FwSpeed,
                                       ASFW::Async::InterfaceCompletionCallback cb) override {
        readAddresses.push_back(addr.addressLo);
        EXPECT_FALSE(readResponses.empty()) << "Unexpected ReadBlock at 0x" << std::hex << addr.addressLo;
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
                                  ASFW::Async::FWAddress addr,
                                  ASFW::FW::LockOp,
                                  std::span<const uint8_t>,
                                  uint32_t,
                                  ASFW::FW::FwSpeed,
                                  ASFW::Async::InterfaceCompletionCallback cb) override {
        lockAddresses.push_back(addr.addressLo);
        EXPECT_FALSE(lockResponses.empty()) << "Unexpected Lock at 0x" << std::hex << addr.addressLo;
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
};

// Build a CAS "success" response: returns the expected (old) value as big-endian,
// which IRMClient interprets as "CAS succeeded" (old == expected).
std::vector<uint8_t> CASSuccess(uint32_t expectedValue) {
    return BEBytes(expectedValue);
}

// Build a CAS "contention" response: returns a DIFFERENT value,
// which IRMClient interprets as "CAS failed" (old != expected) and fires NoResources
// after retries are exhausted.
std::vector<uint8_t> CASContention(uint32_t differentValue) {
    return BEBytes(differentValue);
}

constexpr uint32_t kBandwidthAvailable = ASFW::IRM::IRMRegisters::kBandwidthAvailable;
constexpr uint32_t kChannelsAvailable  = ASFW::IRM::IRMRegisters::kChannelsAvailable31_0;
constexpr uint32_t kInitialBandwidth   = ASFW::IRM::kMaxBandwidthUnitsS400; // 4915
constexpr uint32_t kAllChannelsFree    = ASFW::IRM::kChannelsAvailableInitial; // 0xFFFFFFFF

// Bit mask for channel 0 in CHANNELS_AVAILABLE31_0 (bit 31).
constexpr uint32_t kCh0Bit = 1u << 31;

} // namespace

// ============================================================================
// AllocateResources — happy path: bandwidth then channel, both succeed
// ============================================================================

TEST(IRMClientTests, AllocateResources_BothSucceed_CallbackSuccess) {
    SyncMockBus bus;
    ASFW::IRM::IRMClient irm(bus);
    irm.SetIRMNode(0x3F, ASFW::IRM::Generation{1});

    constexpr uint32_t kBW = 100;

    // Bandwidth allocation: read → CAS success
    bus.readResponses.push_back(BEBytes(kInitialBandwidth));
    bus.lockResponses.push_back(CASSuccess(kInitialBandwidth));

    // Channel 0 allocation: read → CAS success
    bus.readResponses.push_back(BEBytes(kAllChannelsFree));
    bus.lockResponses.push_back(CASSuccess(kAllChannelsFree));

    ASFW::IRM::AllocationStatus result = ASFW::IRM::AllocationStatus::Failed;
    irm.AllocateResources(0, kBW, [&](ASFW::IRM::AllocationStatus s) { result = s; });

    EXPECT_EQ(result, ASFW::IRM::AllocationStatus::Success);
    EXPECT_TRUE(bus.readResponses.empty());
    EXPECT_TRUE(bus.lockResponses.empty());
}

// ============================================================================
// AllocateResources — bandwidth failure: no channel allocation attempted
// ============================================================================

TEST(IRMClientTests, AllocateResources_BandwidthFails_NoChannelOps) {
    SyncMockBus bus;
    ASFW::IRM::IRMClient irm(bus);
    irm.SetIRMNode(0x3F, ASFW::IRM::Generation{1});

    constexpr uint32_t kBW = 5000; // more than kInitialBandwidth → NoResources

    bus.readResponses.push_back(BEBytes(kInitialBandwidth)); // 4915 < 5000 → insufficient

    ASFW::IRM::AllocationStatus result = ASFW::IRM::AllocationStatus::Success;
    irm.AllocateResources(0, kBW, [&](ASFW::IRM::AllocationStatus s) { result = s; });

    EXPECT_EQ(result, ASFW::IRM::AllocationStatus::NoResources);

    // No channel read or lock should have been issued.
    EXPECT_EQ(bus.readAddresses.size(), 1u) << "Only bandwidth read expected";
    EXPECT_TRUE(bus.lockAddresses.empty()) << "No Lock when bandwidth insufficient";
}

// ============================================================================
// AllocateResources — channel failure: bandwidth is rolled back
// ============================================================================

TEST(IRMClientTests, AllocateResources_ChannelFails_BandwidthRolledBack) {
    SyncMockBus bus;
    ASFW::IRM::IRMClient irm(bus);
    irm.SetIRMNode(0x3F, ASFW::IRM::Generation{1});

    constexpr uint32_t kBW = 100;
    const uint32_t afterAlloc = kInitialBandwidth - kBW; // 4815

    // Bandwidth allocation succeeds.
    bus.readResponses.push_back(BEBytes(kInitialBandwidth));
    bus.lockResponses.push_back(CASSuccess(kInitialBandwidth));

    // Channel 0 allocation fails: bit already clear (channel taken).
    const uint32_t channelsTaken = kAllChannelsFree & ~kCh0Bit; // ch0 not available
    bus.readResponses.push_back(BEBytes(channelsTaken));
    // No lock needed: IRMClient detects unavailability from the read value.

    // Bandwidth rollback (ReleaseBandwidth): read current, CAS to restore.
    bus.readResponses.push_back(BEBytes(afterAlloc));
    bus.lockResponses.push_back(CASSuccess(afterAlloc));

    ASFW::IRM::AllocationStatus result = ASFW::IRM::AllocationStatus::Success;
    irm.AllocateResources(0, kBW, [&](ASFW::IRM::AllocationStatus s) { result = s; });

    EXPECT_NE(result, ASFW::IRM::AllocationStatus::Success)
        << "Should fail when channel is unavailable";

    // Exactly 3 reads: bandwidth, channel, rollback-bandwidth.
    EXPECT_EQ(bus.readAddresses.size(), 3u);
    EXPECT_EQ(bus.readAddresses[0], kBandwidthAvailable);
    EXPECT_EQ(bus.readAddresses[1], kChannelsAvailable);
    EXPECT_EQ(bus.readAddresses[2], kBandwidthAvailable) << "Rollback reads bandwidth register";

    // Exactly 2 lock operations: allocate-bandwidth, rollback-bandwidth.
    EXPECT_EQ(bus.lockAddresses.size(), 2u);
    EXPECT_EQ(bus.lockAddresses[0], kBandwidthAvailable);
    EXPECT_EQ(bus.lockAddresses[1], kBandwidthAvailable) << "Rollback CAS on bandwidth register";

    EXPECT_TRUE(bus.readResponses.empty()) << "All queued responses consumed";
    EXPECT_TRUE(bus.lockResponses.empty());
}

// ============================================================================
// AllocateResources — no IRM node configured
// ============================================================================

TEST(IRMClientTests, AllocateResources_NoIRMNode_CallbackNotFound) {
    SyncMockBus bus;
    ASFW::IRM::IRMClient irm(bus); // irmNodeId_ defaults to 0xFF → not set

    ASFW::IRM::AllocationStatus result = ASFW::IRM::AllocationStatus::Success;
    irm.AllocateResources(0, 100, [&](ASFW::IRM::AllocationStatus s) { result = s; });

    EXPECT_EQ(result, ASFW::IRM::AllocationStatus::NotFound);
    EXPECT_TRUE(bus.readAddresses.empty()) << "No bus ops when IRM node not configured";
}

// ============================================================================
// ReleaseResources — issues both channel and bandwidth release CAS
// ============================================================================

TEST(IRMClientTests, ReleaseResources_IssuesBothReleaseCAS) {
    SyncMockBus bus;
    ASFW::IRM::IRMClient irm(bus);
    irm.SetIRMNode(0x3F, ASFW::IRM::Generation{1});

    constexpr uint32_t kBW = 100;
    const uint32_t afterAlloc = kInitialBandwidth - kBW;

    // ReleaseChannel(0): read → CAS (set bit back to 1)
    const uint32_t channelTaken = kAllChannelsFree & ~kCh0Bit;
    bus.readResponses.push_back(BEBytes(channelTaken));
    bus.lockResponses.push_back(CASSuccess(channelTaken));

    // ReleaseBandwidth(100): read → CAS (add units back)
    bus.readResponses.push_back(BEBytes(afterAlloc));
    bus.lockResponses.push_back(CASSuccess(afterAlloc));

    ASFW::IRM::AllocationStatus result = ASFW::IRM::AllocationStatus::Failed;
    irm.ReleaseResources(0, kBW, [&](ASFW::IRM::AllocationStatus s) { result = s; });

    EXPECT_EQ(result, ASFW::IRM::AllocationStatus::Success);
    EXPECT_EQ(bus.readAddresses.size(), 2u);
    EXPECT_EQ(bus.lockAddresses.size(), 2u);

    EXPECT_TRUE(bus.readResponses.empty());
    EXPECT_TRUE(bus.lockResponses.empty());
}

// ============================================================================
// AllocateChannel — invalid channel (≥64) rejected immediately
// ============================================================================

TEST(IRMClientTests, AllocateChannel_InvalidChannel_ImmediateFailed) {
    SyncMockBus bus;
    ASFW::IRM::IRMClient irm(bus);
    irm.SetIRMNode(0x3F, ASFW::IRM::Generation{1});

    ASFW::IRM::AllocationStatus result = ASFW::IRM::AllocationStatus::Success;
    irm.AllocateChannel(64, [&](ASFW::IRM::AllocationStatus s) { result = s; });

    EXPECT_EQ(result, ASFW::IRM::AllocationStatus::Failed);
    EXPECT_TRUE(bus.readAddresses.empty());
}

// ============================================================================
// AllocateBandwidth — zero units succeeds immediately (no bus ops)
// ============================================================================

TEST(IRMClientTests, AllocateBandwidth_ZeroUnits_SuccessWithNoBusOps) {
    SyncMockBus bus;
    ASFW::IRM::IRMClient irm(bus);
    irm.SetIRMNode(0x3F, ASFW::IRM::Generation{1});

    ASFW::IRM::AllocationStatus result = ASFW::IRM::AllocationStatus::Failed;
    irm.AllocateBandwidth(0, [&](ASFW::IRM::AllocationStatus s) { result = s; });

    EXPECT_EQ(result, ASFW::IRM::AllocationStatus::Success);
    EXPECT_TRUE(bus.readAddresses.empty());
}
