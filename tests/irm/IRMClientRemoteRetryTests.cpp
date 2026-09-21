// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// IRMClientRemoteRetryTests.cpp - IRMClient against a remote IRM node: a read
// or lock the IRM never answers (async timeout) is re-issued within the
// policy's budget, a generation change is never retried, an unanswered
// release lock is never credited twice, and a resource release still frees the
// channel when the bandwidth release fails.
//
// Cross-validated with IOFireWireFamily/IOFWAsyncCommand.cpp:425-461 (timed-out
// commands are re-issued kFWCmdDefaultRetries times), IOFWIsochChannel.cpp:
// 1312-1320 (channel deallocation proceeds after a bandwidth failure) and
// Linux core-iso.c:296-320 (compare-swap loop, 5 tries).

#include <gtest/gtest.h>

#include "ASFWDriver/Async/Interfaces/IFireWireBus.hpp"
#include "ASFWDriver/Bus/IRM/IRMClient.hpp"
#include "ASFWDriver/Bus/IRM/IRMTypes.hpp"

#include <array>
#include <deque>
#include <optional>
#include <span>
#include <unordered_map>

namespace {

using ASFW::Async::AsyncHandle;
using ASFW::Async::AsyncStatus;
using ASFW::Async::FWAddress;
using ASFW::Async::IFireWireBus;
using ASFW::Async::InterfaceCompletionCallback;
using ASFW::FW::FwSpeed;
using ASFW::FW::Generation;
using ASFW::FW::NodeId;
using ASFW::IRM::AllocationStatus;
using ASFW::IRM::ChannelToBitMask;
using ASFW::IRM::ChannelToRegisterAddress;
using ASFW::IRM::IRMClient;
using ASFW::IRM::RetryPolicy;
namespace Regs = ASFW::IRM::IRMRegisters;

constexpr uint8_t kIrmNode = 0;
constexpr uint8_t kLocalNode = 1;
constexpr uint8_t kHeldChannel = 3;
constexpr uint32_t kUnits = 884;   // one AM824 stream of 11 quadlets at S400
constexpr uint32_t kMaxUnits = 4915;

// The remote IRM's register file. Reads and locks can be told, per register,
// to fail the next N requests the way the async layer reports them; a lock
// can also be applied before its response is "lost".
class FakeIrmBus final : public IFireWireBus {
public:
    AsyncHandle ReadBlock(Generation, NodeId, FWAddress address, uint32_t, FwSpeed,
                          InterfaceCompletionCallback callback) override {
        ++reads;
        if (const auto failure = TakeFailure(readFailures, address.addressLo)) {
            callback(*failure, {});
            return Next();
        }
        callback(AsyncStatus::kSuccess, ToWire(registers[address.addressLo]));
        return Next();
    }
    AsyncHandle WriteBlock(Generation, NodeId, FWAddress, std::span<const uint8_t>, FwSpeed,
                           InterfaceCompletionCallback callback) override {
        callback(AsyncStatus::kSuccess, {});
        return Next();
    }
    AsyncHandle Lock(Generation, NodeId, FWAddress address, ASFW::FW::LockOp,
                     std::span<const uint8_t> operand, uint32_t, FwSpeed,
                     InterfaceCompletionCallback callback) override {
        ++locks;
        const uint32_t expected = FromWire(operand.subspan(0, 4));
        const uint32_t desired = FromWire(operand.subspan(4, 4));
        uint32_t& reg = registers[address.addressLo];
        uint32_t observed = reg;
        if (conflictOnce) {
            conflictOnce = false;
            observed ^= 1U;  // another node got there first; nothing applied
            callback(AsyncStatus::kSuccess, ToWire(observed));
            return Next();
        }
        const auto failure = TakeFailure(lockFailures, address.addressLo);
        if ((!failure || applyLockBeforeFailure) && observed == expected) {
            reg = desired;
        }
        if (failure) {
            callback(*failure, {});
            return Next();
        }
        callback(AsyncStatus::kSuccess, ToWire(observed));
        return Next();
    }
    bool Cancel(AsyncHandle) override { return false; }
    FwSpeed GetSpeed(NodeId) const override { return FwSpeed::S400; }
    uint32_t HopCount(NodeId, NodeId) const override { return 0; }
    uint8_t GetGapCount() const override { return 63; }
    Generation GetGeneration() const override { return Generation{1}; }
    NodeId GetLocalNodeID() const override { return NodeId{kLocalNode}; }

    std::unordered_map<uint32_t, uint32_t> registers{
        {Regs::kBandwidthAvailable, kMaxUnits - kUnits},
        {Regs::kChannelsAvailable31_0, 0xFFFFFFFFU & ~ChannelToBitMask(kHeldChannel)},
        {Regs::kChannelsAvailable63_32, 0xFFFFFFFFU},
    };
    std::unordered_map<uint32_t, std::deque<AsyncStatus>> readFailures;
    std::unordered_map<uint32_t, std::deque<AsyncStatus>> lockFailures;
    bool applyLockBeforeFailure{false};  // the IRM applied the lock, only the response was lost
    bool conflictOnce{false};
    size_t reads{0};
    size_t locks{0};

private:
    static std::optional<AsyncStatus> TakeFailure(
        std::unordered_map<uint32_t, std::deque<AsyncStatus>>& failures, uint32_t addressLo) {
        auto it = failures.find(addressLo);
        if (it == failures.end() || it->second.empty()) {
            return std::nullopt;
        }
        const AsyncStatus status = it->second.front();
        it->second.pop_front();
        return status;
    }
    static std::array<uint8_t, 4> ToWire(uint32_t value) {
        return {static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
                static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
    }
    static uint32_t FromWire(std::span<const uint8_t> b) {
        return (uint32_t{b[0]} << 24) | (uint32_t{b[1]} << 16) | (uint32_t{b[2]} << 8) | uint32_t{b[3]};
    }
    AsyncHandle Next() { return AsyncHandle{++next_}; }
    uint32_t next_{0};
};

class IRMClientRemoteRetryTest : public testing::Test {
protected:
    IRMClientRemoteRetryTest() : irm_(bus_) { irm_.SetIRMNode(kIrmNode, Generation{1}); }

    template <typename Op>
    AllocationStatus Run(Op&& op) {
        std::optional<AllocationStatus> result;
        op([&](AllocationStatus status) { result = status; });
        EXPECT_TRUE(result.has_value()) << "operation never completed";
        return result.value_or(AllocationStatus::Failed);
    }
    AllocationStatus ReleaseBandwidth(uint32_t units, RetryPolicy policy = RetryPolicy::Default()) {
        return Run([&](auto cb) { irm_.ReleaseBandwidth(units, cb, policy); });
    }
    AllocationStatus ReleaseChannel(uint8_t channel) {
        return Run([&](auto cb) { irm_.ReleaseChannel(channel, cb); });
    }
    AllocationStatus AllocateChannel(uint8_t channel) {
        return Run([&](auto cb) { irm_.AllocateChannel(channel, cb); });
    }
    AllocationStatus ReleaseResources(uint8_t channel, uint32_t units) {
        return Run([&](auto cb) { irm_.ReleaseResources(channel, units, cb); });
    }

    uint32_t Bandwidth() const { return bus_.registers.at(Regs::kBandwidthAvailable); }
    bool ChannelFree(uint8_t channel) const {
        return (bus_.registers.at(ChannelToRegisterAddress(channel)) & ChannelToBitMask(channel)) != 0;
    }
    static std::deque<AsyncStatus> Timeouts(size_t n) {
        return std::deque<AsyncStatus>(n, AsyncStatus::kTimeout);
    }

    FakeIrmBus bus_;
    IRMClient irm_;
};

TEST_F(IRMClientRemoteRetryTest, ReleaseBandwidthReissuesAReadTheIrmNeverAnswered) {
    bus_.readFailures[Regs::kBandwidthAvailable] = Timeouts(1);
    EXPECT_EQ(ReleaseBandwidth(kUnits), AllocationStatus::Success);
    EXPECT_EQ(bus_.reads, 2U);
    EXPECT_EQ(bus_.locks, 1U);
    EXPECT_EQ(Bandwidth(), kMaxUnits);
}

TEST_F(IRMClientRemoteRetryTest, ReleaseBandwidthReissuesALockTheIrmNeverAnswered) {
    bus_.lockFailures[Regs::kBandwidthAvailable] = Timeouts(1);  // request dropped, nothing applied
    EXPECT_EQ(ReleaseBandwidth(kUnits), AllocationStatus::Success);
    EXPECT_EQ(bus_.reads, 2U);  // a fresh read precedes the second lock
    EXPECT_EQ(bus_.locks, 2U);
    EXPECT_EQ(Bandwidth(), kMaxUnits);
}

TEST_F(IRMClientRemoteRetryTest, ReleaseBandwidthNeverCreditsTwiceWhenOnlyTheResponseWasLost) {
    bus_.registers[Regs::kBandwidthAvailable] = 3000U;  // low enough that a double credit fits
    bus_.applyLockBeforeFailure = true;
    bus_.lockFailures[Regs::kBandwidthAvailable] = Timeouts(1);
    EXPECT_EQ(ReleaseBandwidth(kUnits), AllocationStatus::Success);
    EXPECT_EQ(bus_.locks, 1U);
    EXPECT_EQ(Bandwidth(), 3000U + kUnits);
}

TEST_F(IRMClientRemoteRetryTest, ReleaseBandwidthGivesUpAfterTheTimeoutBudget) {
    bus_.readFailures[Regs::kBandwidthAvailable] = Timeouts(8);
    EXPECT_EQ(ReleaseBandwidth(kUnits), AllocationStatus::Timeout);
    EXPECT_EQ(bus_.reads, 4U);  // one attempt plus the default three re-issues
    EXPECT_EQ(bus_.locks, 0U);
    EXPECT_EQ(Bandwidth(), kMaxUnits - kUnits);
}

TEST_F(IRMClientRemoteRetryTest, RetryPolicyNoneMakesASingleAttempt) {
    bus_.readFailures[Regs::kBandwidthAvailable] = Timeouts(1);
    EXPECT_EQ(ReleaseBandwidth(kUnits, RetryPolicy::None()), AllocationStatus::Timeout);
    EXPECT_EQ(bus_.reads, 1U);
}

TEST_F(IRMClientRemoteRetryTest, AGenerationChangeIsNeverRetried) {
    bus_.readFailures[Regs::kBandwidthAvailable] = {AsyncStatus::kStaleGeneration};
    EXPECT_EQ(ReleaseBandwidth(kUnits), AllocationStatus::GenerationMismatch);
    EXPECT_EQ(bus_.reads, 1U);
    EXPECT_EQ(bus_.locks, 0U);
}

TEST_F(IRMClientRemoteRetryTest, ContentionRetriesAreUnchanged) {
    bus_.conflictOnce = true;
    EXPECT_EQ(ReleaseBandwidth(kUnits), AllocationStatus::Success);
    EXPECT_EQ(bus_.reads, 2U);
    EXPECT_EQ(bus_.locks, 2U);
    EXPECT_EQ(Bandwidth(), kMaxUnits);
}

TEST_F(IRMClientRemoteRetryTest, ReleaseResourcesStillFreesTheChannelWhenBandwidthReleaseFails) {
    bus_.readFailures[Regs::kBandwidthAvailable] = Timeouts(8);
    EXPECT_EQ(ReleaseResources(kHeldChannel, kUnits), AllocationStatus::Timeout);  // first failure
    EXPECT_TRUE(ChannelFree(kHeldChannel));
    EXPECT_EQ(Bandwidth(), kMaxUnits - kUnits);  // the leak stays visible to the caller
}

TEST_F(IRMClientRemoteRetryTest, ReleaseResourcesStopsAtAGenerationChange) {
    bus_.readFailures[Regs::kBandwidthAvailable] = {AsyncStatus::kStaleGeneration};
    EXPECT_EQ(ReleaseResources(kHeldChannel, kUnits), AllocationStatus::GenerationMismatch);
    EXPECT_FALSE(ChannelFree(kHeldChannel));  // the reset freed it; nothing is sent
    EXPECT_EQ(bus_.reads, 1U);
}

TEST_F(IRMClientRemoteRetryTest, ReleaseResourcesReportsTheChannelResultWhenBandwidthSucceeded) {
    bus_.readFailures[Regs::kChannelsAvailable31_0] = Timeouts(8);
    EXPECT_EQ(ReleaseResources(kHeldChannel, kUnits), AllocationStatus::Timeout);
    EXPECT_EQ(Bandwidth(), kMaxUnits);
    EXPECT_FALSE(ChannelFree(kHeldChannel));
}

TEST_F(IRMClientRemoteRetryTest, AllocateChannelReissuesAReadTheIrmNeverAnswered) {
    bus_.readFailures[Regs::kChannelsAvailable31_0] = Timeouts(1);
    EXPECT_EQ(AllocateChannel(5), AllocationStatus::Success);
    EXPECT_FALSE(ChannelFree(5));
    EXPECT_EQ(bus_.reads, 2U);
    EXPECT_EQ(bus_.locks, 1U);
}

TEST_F(IRMClientRemoteRetryTest, AllocateChannelWhoseLockWasAppliedButUnansweredReportsNoResources) {
    bus_.applyLockBeforeFailure = true;
    bus_.lockFailures[Regs::kChannelsAvailable31_0] = Timeouts(1);
    EXPECT_EQ(AllocateChannel(5), AllocationStatus::ChannelBusy);  // conservative: reads as taken
    EXPECT_FALSE(ChannelFree(5));
}

TEST_F(IRMClientRemoteRetryTest, ChannelReleaseWhoseResponseWasLostIsIdempotent) {
    bus_.applyLockBeforeFailure = true;
    bus_.lockFailures[Regs::kChannelsAvailable31_0] = Timeouts(1);
    EXPECT_EQ(ReleaseChannel(kHeldChannel), AllocationStatus::Success);
    EXPECT_TRUE(ChannelFree(kHeldChannel));
    EXPECT_EQ(bus_.locks, 2U);  // the second lock is a no-op compare-swap
}

} // namespace
