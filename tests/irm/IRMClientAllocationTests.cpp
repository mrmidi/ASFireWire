// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// IRMClientAllocationTests.cpp - Tests for IEEE 1394 IRM CSR resource allocation,
// Apple channel-then-bandwidth ordering, channel rollback on bandwidth deficit,
// local CSR fast path, and bus generation validation.
//
// Cross-validated with Apple IOFireWireFamily/IOFWIsochChannel.cpp (allocates
// channel first, then bandwidth; on bandwidth deficit, releases channel) and
// IEEE Std 1394-2008 clause 8.3.2.3.8 (BANDWIDTH_AVAILABLE) and 8.3.2.3.9
// (CHANNELS_AVAILABLE).

#include <gtest/gtest.h>

#include "Async/Interfaces/IFireWireBus.hpp"
#include "Bus/IRM/IRMCSRConstants.hpp"
#include "Bus/IRM/IRMClient.hpp"
#include "Bus/IRM/IRMTypes.hpp"
#include "Common/WireFormat.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace {

using ::ASFW::Async::AsyncHandle;
using ::ASFW::Async::AsyncStatus;
using ::ASFW::Async::FWAddress;
using ::ASFW::Async::IFireWireBus;
using ::ASFW::Async::InterfaceCompletionCallback;
using ::ASFW::FW::FwSpeed;
using ::ASFW::FW::Generation;
using ::ASFW::FW::LockOp;
using ::ASFW::FW::NodeId;
using ::ASFW::IRM::AllocationStatus;
using ::ASFW::IRM::IRMClient;
using ::ASFW::IRM::ResourceSnapshot;

enum class OpKind {
    Read,
    Write,
    Lock,
};

struct RecordedOp {
    OpKind kind;
    uint32_t addressLo;
    uint32_t length;
    FwSpeed speed;
};

struct ExpectedOp {
    OpKind kind;
    uint32_t addressLo;
    uint32_t length;
    FwSpeed speed;
};

inline void ExpectOperations(const std::vector<RecordedOp>& actual,
                             const std::vector<ExpectedOp>& expected) {
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(actual[i].kind, expected[i].kind) << "op " << i;
        EXPECT_EQ(actual[i].addressLo, expected[i].addressLo) << "op " << i;
        EXPECT_EQ(actual[i].length, expected[i].length) << "op " << i;
        EXPECT_EQ(actual[i].speed, expected[i].speed) << "op " << i;
    }
}

class IRMMockBus final : public IFireWireBus {
public:
    IRMMockBus() = default;

    AsyncHandle ReadBlock(Generation generation,
                          NodeId /*nodeId*/,
                          FWAddress address,
                          uint32_t length,
                          FwSpeed speed,
                          InterfaceCompletionCallback callback) override {
        Record(OpKind::Read, address.addressLo, length, speed);
        if (generation != generation_) {
            callback(AsyncStatus::kStaleGeneration, {});
            return NextHandle();
        }

        if (address.addressHi == 0xFFFF && address.addressLo == 0xF0000220U && length == 12) {
            std::vector<uint8_t> bytes(12);
            ::ASFW::FW::WriteBE32(bytes.data(), bandwidthAvailable_);
            ::ASFW::FW::WriteBE32(bytes.data() + 4, channelsAvailable31_0_);
            ::ASFW::FW::WriteBE32(bytes.data() + 8, channelsAvailable63_32_);
            callback(AsyncStatus::kSuccess, std::span<const uint8_t>(bytes.data(), bytes.size()));
            return NextHandle();
        }

        if (address.addressHi == 0xFFFF && length == 4) {
            std::vector<uint8_t> bytes(4);
            if (address.addressLo == 0xF0000220U) {
                ::ASFW::FW::WriteBE32(bytes.data(), bandwidthAvailable_);
            } else if (address.addressLo == 0xF0000224U) {
                ::ASFW::FW::WriteBE32(bytes.data(), channelsAvailable31_0_);
            } else if (address.addressLo == 0xF0000228U) {
                ::ASFW::FW::WriteBE32(bytes.data(), channelsAvailable63_32_);
            }
            callback(AsyncStatus::kSuccess, std::span<const uint8_t>(bytes.data(), bytes.size()));
            return NextHandle();
        }

        callback(AsyncStatus::kHardwareError, {});
        return NextHandle();
    }

    AsyncHandle WriteBlock(Generation generation,
                           NodeId /*nodeId*/,
                           FWAddress address,
                           std::span<const uint8_t> data,
                           FwSpeed speed,
                           InterfaceCompletionCallback callback) override {
        Record(OpKind::Write, address.addressLo, static_cast<uint32_t>(data.size()), speed);
        if (generation != generation_) {
            callback(AsyncStatus::kStaleGeneration, {});
            return NextHandle();
        }
        callback(AsyncStatus::kSuccess, {});
        return NextHandle();
    }

    AsyncHandle Lock(Generation generation,
                     NodeId /*nodeId*/,
                     FWAddress address,
                     LockOp /*lockOp*/,
                     std::span<const uint8_t> operand,
                     uint32_t responseLength,
                     FwSpeed speed,
                     InterfaceCompletionCallback callback) override {
        Record(OpKind::Lock, address.addressLo, static_cast<uint32_t>(operand.size()), speed);
        if (generation != generation_) {
            callback(AsyncStatus::kStaleGeneration, {});
            return NextHandle();
        }

        std::vector<uint8_t> response(responseLength, 0);
        if (operand.size() >= 8 && responseLength >= 4) {
            const uint32_t arg = ::ASFW::FW::ReadBE32(operand.data());
            const uint32_t data = ::ASFW::FW::ReadBE32(operand.data() + 4);

            if (address.addressHi == 0xFFFF && address.addressLo == 0xF0000220U) {
                ::ASFW::FW::WriteBE32(response.data(), bandwidthAvailable_);
                if (bandwidthAvailable_ == arg) {
                    bandwidthAvailable_ = data;
                }
            } else if (address.addressHi == 0xFFFF && address.addressLo == 0xF0000224U) {
                ::ASFW::FW::WriteBE32(response.data(), channelsAvailable31_0_);
                if (channelsAvailable31_0_ == arg) {
                    channelsAvailable31_0_ = data;
                }
            } else if (address.addressHi == 0xFFFF && address.addressLo == 0xF0000228U) {
                ::ASFW::FW::WriteBE32(response.data(), channelsAvailable63_32_);
                if (channelsAvailable63_32_ == arg) {
                    channelsAvailable63_32_ = data;
                }
            }
        }

        callback(AsyncStatus::kSuccess, std::span<const uint8_t>(response.data(), response.size()));
        return NextHandle();
    }

    bool Cancel(AsyncHandle) override { return false; }
    FwSpeed GetSpeed(NodeId) const override { return FwSpeed::S100; }
    uint32_t HopCount(NodeId, NodeId) const override { return 1; }
    uint8_t GetGapCount() const override { return 63; }
    Generation GetGeneration() const override { return generation_; }
    NodeId GetLocalNodeID() const override { return localNodeId_; }

    void SetIRMResourceState(uint32_t bandwidth, uint32_t ch31_0, uint32_t ch63_32) {
        bandwidthAvailable_ = bandwidth;
        channelsAvailable31_0_ = ch31_0;
        channelsAvailable63_32_ = ch63_32;
    }

    void SetGeneration(Generation gen) { generation_ = gen; }
    void SetLocalNodeID(NodeId id) { localNodeId_ = id; }
    void ClearOperations() { operations_.clear(); }

    [[nodiscard]] uint32_t BandwidthAvailable() const { return bandwidthAvailable_; }
    [[nodiscard]] uint32_t ChannelsAvailable31_0() const { return channelsAvailable31_0_; }
    [[nodiscard]] uint32_t ChannelsAvailable63_32() const { return channelsAvailable63_32_; }
    [[nodiscard]] const std::vector<RecordedOp>& Operations() const { return operations_; }

private:
    AsyncHandle NextHandle() { return AsyncHandle{nextHandle_++}; }

    void Record(OpKind kind, uint32_t addressLo, uint32_t length, FwSpeed speed) {
        operations_.push_back({kind, addressLo, length, speed});
    }

    uint32_t bandwidthAvailable_{4019};
    uint32_t channelsAvailable31_0_{0x3FFFFFFFU};
    uint32_t channelsAvailable63_32_{0xFFFFFFFFU};
    Generation generation_{Generation{1}};
    NodeId localNodeId_{NodeId{0}};
    uint32_t nextHandle_{1};
    std::vector<RecordedOp> operations_;
};

class LocalIRMCSRTestBackend {
public:
    LocalIRMCSRTestBackend(uint32_t bandwidthAvailable,
                           uint32_t channelsAvailableHi,
                           uint32_t channelsAvailableLo) {
        using namespace ::ASFW::Driver::IRMCSR;
        values_[static_cast<uint32_t>(CSRSelector::BusManagerId)] = kNoBusManagerId;
        values_[static_cast<uint32_t>(CSRSelector::BandwidthAvailable)] = bandwidthAvailable;
        values_[static_cast<uint32_t>(CSRSelector::ChannelsAvailableHi)] = channelsAvailableHi;
        values_[static_cast<uint32_t>(CSRSelector::ChannelsAvailableLo)] = channelsAvailableLo;
    }

    IRMClient::LocalIRMAccess Access() {
        return IRMClient::LocalIRMAccess{
            .read = [this](uint32_t selector) -> ::ASFW::Driver::LocalCSRReadResult {
                ++readCount_;
                return {::ASFW::Driver::LocalCSRLockResult::Status::Success, values_.at(selector)};
            },
            .compareSwap =
                [this](uint32_t selector,
                       uint32_t compareValue,
                       uint32_t newValue) -> ::ASFW::Driver::LocalCSRLockResult {
                ++compareSwapCount_;
                const uint32_t oldValue = values_.at(selector);
                if (oldValue == compareValue) {
                    values_[selector] = newValue;
                    return {::ASFW::Driver::LocalCSRLockResult::Status::Success, oldValue, true};
                }
                return {::ASFW::Driver::LocalCSRLockResult::Status::Success, oldValue, false};
            },
        };
    }

    [[nodiscard]] uint32_t Value(uint32_t selector) const {
        return values_.at(selector);
    }

    [[nodiscard]] uint32_t ReadCount() const { return readCount_; }
    [[nodiscard]] uint32_t CompareSwapCount() const { return compareSwapCount_; }

private:
    std::array<uint32_t, 4> values_{};
    uint32_t readCount_{0};
    uint32_t compareSwapCount_{0};
};

// --- Tests ---

TEST(IRMClientAllocationTests, IRMReadResourcesSnapshotUsesQuadletReads) {
    IRMMockBus bus;
    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});

    std::optional<AllocationStatus> status;
    ResourceSnapshot snapshot{};
    irm.ReadResourcesSnapshot([&](AllocationStatus s, ResourceSnapshot value) {
        status = s;
        snapshot = value;
    });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, AllocationStatus::Success);
    EXPECT_EQ(snapshot.bandwidthAvailable, 4019U);
    EXPECT_EQ(snapshot.channelsAvailable31_0, 0x3FFFFFFFU);
    EXPECT_EQ(snapshot.channelsAvailable63_32, 0xFFFFFFFFU);

    const std::vector<ExpectedOp> expected{
        {OpKind::Read, 0xF0000220U, 4, FwSpeed::S100},
        {OpKind::Read, 0xF0000224U, 4, FwSpeed::S100},
        {OpKind::Read, 0xF0000228U, 4, FwSpeed::S100},
    };
    ExpectOperations(bus.Operations(), expected);
}

TEST(IRMClientAllocationTests, IRMAllocateResourcesAllocatesChannelThenBandwidthLikeApple) {
    IRMMockBus bus;
    bus.SetIRMResourceState(4915U, 0xFFFFFFFEU, 0xFFFFFFFFU);

    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});

    std::optional<AllocationStatus> status;
    irm.AllocateResources(0, 320U, [&](AllocationStatus s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, AllocationStatus::Success);
    EXPECT_EQ(bus.BandwidthAvailable(), 4595U);
    EXPECT_EQ(bus.ChannelsAvailable31_0(), 0x7FFFFFFEU);

    const std::vector<ExpectedOp> expected{
        {OpKind::Read, 0xF0000224U, 4, FwSpeed::S100},
        {OpKind::Lock, 0xF0000224U, 8, FwSpeed::S100},
        {OpKind::Read, 0xF0000220U, 4, FwSpeed::S100},
        {OpKind::Lock, 0xF0000220U, 8, FwSpeed::S100},
    };
    ExpectOperations(bus.Operations(), expected);
}

// kIOReturnNoResources is one code for several bus conditions, so the IRM layer
// -- the only one that sees which register refused -- names the resource. A
// channel another node owns is not the same event as a lock that lost its race.
TEST(IRMClientAllocationTests, IRMAllocateResourcesNamesAChannelOwnedByAnotherNode) {
    IRMMockBus bus;
    // Bit 31 is channel 0: clear means channel 0 is already allocated.
    bus.SetIRMResourceState(4915U, 0x7FFFFFFFU, 0xFFFFFFFFU);
    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});

    std::optional<AllocationStatus> status;
    irm.AllocateResources(0, 320U, [&](AllocationStatus s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, AllocationStatus::ChannelBusy);
    // Nothing was taken, so nothing needed rolling back.
    EXPECT_EQ(bus.BandwidthAvailable(), 4915U);
    EXPECT_EQ(bus.ChannelsAvailable31_0(), 0x7FFFFFFFU);
}

TEST(IRMClientAllocationTests, IRMAllocateResourcesRollsBackChannelWhenBandwidthUnavailable) {
    IRMMockBus bus;
    bus.SetIRMResourceState(100U, 0xFFFFFFFEU, 0xFFFFFFFFU);

    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});

    std::optional<AllocationStatus> status;
    irm.AllocateResources(0, 320U, [&](AllocationStatus s) { status = s; });

    ASSERT_TRUE(status.has_value());
    // The rollback releases the channel but must not blur why the allocation
    // failed: the caller needs to know it was bandwidth, not the channel.
    EXPECT_EQ(*status, AllocationStatus::BandwidthShort);
    EXPECT_EQ(bus.BandwidthAvailable(), 100U);
    EXPECT_EQ(bus.ChannelsAvailable31_0(), 0xFFFFFFFEU);

    const std::vector<ExpectedOp> expected{
        {OpKind::Read, 0xF0000224U, 4, FwSpeed::S100},
        {OpKind::Lock, 0xF0000224U, 8, FwSpeed::S100},
        {OpKind::Read, 0xF0000220U, 4, FwSpeed::S100},
        {OpKind::Read, 0xF0000224U, 4, FwSpeed::S100},
        {OpKind::Lock, 0xF0000224U, 8, FwSpeed::S100},
    };
    ExpectOperations(bus.Operations(), expected);
}

TEST(IRMClientAllocationTests, IRMLocalAllocateResourcesUsesLocalCSRBackendWithoutAT) {
    using namespace ::ASFW::Driver::IRMCSR;

    IRMMockBus bus;
    bus.SetLocalNodeID(NodeId{0x02});
    LocalIRMCSRTestBackend localCSR(4915U, 0xFFFFFFFEU, 0xFFFFFFFFU);

    IRMClient irm(bus, localCSR.Access());
    irm.SetIRMNode(0x02, Generation{1});

    std::optional<AllocationStatus> status;
    irm.AllocateResources(0, 320U, [&](AllocationStatus s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, AllocationStatus::Success);
    EXPECT_TRUE(bus.Operations().empty());
    EXPECT_EQ(localCSR.Value(static_cast<uint32_t>(CSRSelector::BandwidthAvailable)), 4595U);
    EXPECT_EQ(localCSR.Value(static_cast<uint32_t>(CSRSelector::ChannelsAvailableHi)), 0x7FFFFFFEU);
    EXPECT_EQ(localCSR.Value(static_cast<uint32_t>(CSRSelector::ChannelsAvailableLo)), 0xFFFFFFFFU);
    EXPECT_EQ(localCSR.ReadCount(), 2U);
    EXPECT_EQ(localCSR.CompareSwapCount(), 2U);
}

TEST(IRMClientAllocationTests, IRMLocalAllocateResourcesChecksGenerationBeforeCSRAccess) {
    using namespace ::ASFW::Driver::IRMCSR;

    IRMMockBus bus;
    bus.SetLocalNodeID(NodeId{0x02});
    bus.SetGeneration(Generation{2});
    LocalIRMCSRTestBackend localCSR(4915U, 0xFFFFFFFFU, 0xFFFFFFFFU);

    IRMClient irm(bus, localCSR.Access());
    irm.SetIRMNode(0x02, Generation{1});

    std::optional<AllocationStatus> status;
    irm.AllocateResources(0, 320U, [&](AllocationStatus s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, AllocationStatus::GenerationMismatch);
    EXPECT_TRUE(bus.Operations().empty());
    EXPECT_EQ(localCSR.Value(static_cast<uint32_t>(CSRSelector::BandwidthAvailable)), 4915U);
    EXPECT_EQ(localCSR.ReadCount(), 0U);
    EXPECT_EQ(localCSR.CompareSwapCount(), 0U);
}

TEST(IRMClientAllocationTests, IRMPlaybackAllocationUsesAppleChannelThenBandwidthOrder) {
    IRMMockBus bus;
    bus.SetIRMResourceState(4915U, 0xFFFFFFFEU, 0xFFFFFFFFU);

    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});

    std::optional<AllocationStatus> status;
    irm.AllocateResources(0, 320U, [&](AllocationStatus s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, AllocationStatus::Success);

    const std::vector<ExpectedOp> expected{
        {OpKind::Read, 0xF0000224U, 4, FwSpeed::S100},
        {OpKind::Lock, 0xF0000224U, 8, FwSpeed::S100},
        {OpKind::Read, 0xF0000220U, 4, FwSpeed::S100},
        {OpKind::Lock, 0xF0000220U, 8, FwSpeed::S100},
    };
    ExpectOperations(bus.Operations(), expected);
}

TEST(IRMClientAllocationTests, IRMCaptureAllocationUsesAppleChannelThenBandwidthOrder) {
    IRMMockBus bus;
    bus.SetIRMResourceState(4595U, 0x7FFFFFFEU, 0xFFFFFFFFU);

    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});

    std::optional<AllocationStatus> status;
    irm.AllocateResources(1, 576U, [&](AllocationStatus s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, AllocationStatus::Success);

    const std::vector<ExpectedOp> expected{
        {OpKind::Read, 0xF0000224U, 4, FwSpeed::S100},
        {OpKind::Lock, 0xF0000224U, 8, FwSpeed::S100},
        {OpKind::Read, 0xF0000220U, 4, FwSpeed::S100},
        {OpKind::Lock, 0xF0000220U, 8, FwSpeed::S100},
    };
    ExpectOperations(bus.Operations(), expected);
}

TEST(IRMClientAllocationTests, IRMAllocateResourcesReturnsGenerationMismatchWhenBusMoves) {
    IRMMockBus bus;
    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});
    bus.SetGeneration(Generation{2});

    std::optional<AllocationStatus> status;
    irm.AllocateResources(0, 320U, [&](AllocationStatus s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, AllocationStatus::GenerationMismatch);
}

} // namespace
