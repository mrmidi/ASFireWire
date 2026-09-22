// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 ASFireWire Project
//
// DuplexIRMReservationsTests.cpp - Tests for generic audio duplex IRM stream
// reservation tracking, bandwidth budgeting, multi-stream channel mapping,
// and bus reset recovery across all audio device families.

#include <gtest/gtest.h>

#include "Async/Interfaces/IFireWireBus.hpp"
#include "Audio/Protocols/AudioTypes.hpp"
#include "Audio/Protocols/Backends/DuplexIRMReservations.hpp"
#include "Bus/IRM/IRMClient.hpp"
#include "Common/WireFormat.hpp"

#include <algorithm>
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
using ::ASFW::Audio::AudioDuplexChannels;
using ::ASFW::FW::FwSpeed;
using ::ASFW::FW::Generation;
using ::ASFW::FW::LockOp;
using ::ASFW::FW::NodeId;
using ::ASFW::IRM::IRMClient;

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

class DuplexMockFireWireBus final : public IFireWireBus {
public:
    DuplexMockFireWireBus() = default;

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
    FwSpeed GetSpeed(NodeId) const override { return FwSpeed::S400; }
    uint32_t HopCount(NodeId, NodeId) const override { return 1; }
    uint8_t GetGapCount() const override { return gapCount_; }
    void SetGapCount(uint8_t gap) { gapCount_ = gap; }
    Generation GetGeneration() const override { return generation_; }
    void SetGeneration(Generation gen) { generation_ = gen; }
    NodeId GetLocalNodeID() const override { return localNodeId_; }
    void SetLocalNodeID(NodeId id) { localNodeId_ = id; }

    void SetIRMResourceState(uint32_t bandwidth, uint32_t ch31_0, uint32_t ch63_32) {
        bandwidthAvailable_ = bandwidth;
        channelsAvailable31_0_ = ch31_0;
        channelsAvailable63_32_ = ch63_32;
    }

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
    uint8_t gapCount_{63};
    Generation generation_{Generation{1}};
    NodeId localNodeId_{NodeId{0}};
    uint32_t nextHandle_{1};
    std::vector<RecordedOp> operations_;
};

// --- Tests ---

TEST(DuplexIRMReservationsTests,
     DuplexIRMReservationsAllocatesAndReleasesBothDirections) {
    DuplexMockFireWireBus bus;
    bus.SetIRMResourceState(4915U, 0xFFFFFFFFU, 0xFFFFFFFFU);
    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});
    ASFW::Audio::Backends::DuplexIRMReservations reservations;

    // Under Apple IOFWIsochChannel wire parity, reserve takes the packet term
    // with zero gap overhead charged against BANDWIDTH_AVAILABLE.
    ASSERT_EQ(reservations.Reserve(irm, 0, 320U), kIOReturnSuccess);
    ASSERT_EQ(reservations.Reserve(irm, 1, 576U), kIOReturnSuccess);
    EXPECT_EQ(reservations.Count(), 2U);
    EXPECT_EQ(bus.BandwidthAvailable(), 4915U - 320U - 576U);
    EXPECT_EQ(bus.ChannelsAvailable31_0(), 0x3FFFFFFFU);

    reservations.ReleaseAll();
    EXPECT_EQ(reservations.Count(), 0U);
    EXPECT_EQ(bus.BandwidthAvailable(), 4915U);
    EXPECT_EQ(bus.ChannelsAvailable31_0(), 0xFFFFFFFFU);
}

TEST(DuplexIRMReservationsTests,
     DuplexIRMReservationsInvalidatesAfterGenerationChangeWithoutWireRelease) {
    DuplexMockFireWireBus bus;
    bus.SetIRMResourceState(4915U, 0xFFFFFFFFU, 0xFFFFFFFFU);
    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});
    ASFW::Audio::Backends::DuplexIRMReservations reservations;

    ASSERT_EQ(reservations.Reserve(irm, 0, 320U), kIOReturnSuccess);
    ASSERT_EQ(reservations.Reserve(irm, 1, 576U), kIOReturnSuccess);
    bus.ClearOperations();

    // A bus reset returns the IRM resource state to its initial values. The
    // old generation cannot accept a release transaction, so only local
    // bookkeeping may change here.
    bus.SetGeneration(Generation{2});
    bus.SetIRMResourceState(4915U, 0xFFFFFFFFU, 0xFFFFFFFFU);
    reservations.InvalidateAfterGenerationChange();

    EXPECT_EQ(reservations.Count(), 0U);
    EXPECT_TRUE(bus.Operations().empty());
    EXPECT_EQ(bus.BandwidthAvailable(), 4915U);
    EXPECT_EQ(bus.ChannelsAvailable31_0(), 0xFFFFFFFFU);
}

TEST(DuplexIRMReservationsTests,
     DuplexIRMReservationsSelectsFirstAllowedChannelThatIRMReportsFree) {
    DuplexMockFireWireBus bus;
    // IRM channel bits are big-endian within each CSR quadlet: bit 31 is
    // channel 0 and bit 30 is channel 1. Make channel 0 busy and channel 1 free.
    bus.SetIRMResourceState(4915U, 0x7FFFFFFFU, 0xFFFFFFFFU);
    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});
    ASFW::Audio::Backends::DuplexIRMReservations reservations;

    const auto result = reservations.ReserveAny(
        irm, (uint64_t{1} << 0U) | (uint64_t{1} << 1U), 596U);

    ASSERT_EQ(result.status, kIOReturnSuccess);
    EXPECT_EQ(result.channel, 1U);
    EXPECT_EQ(reservations.Count(), 1U);
    EXPECT_EQ(result.charge.packetUnits, 596U);
    EXPECT_EQ(result.charge.overheadUnits, 0U);
    EXPECT_EQ(bus.BandwidthAvailable(), 4915U - 596U);
    EXPECT_EQ(bus.ChannelsAvailable31_0(), 0x3FFFFFFFU);
}

TEST(DuplexIRMReservationsTests,
     DuplexIRMReservationsTracksAndReleasesEveryMultistreamAllocation) {
    DuplexMockFireWireBus bus;
    bus.SetIRMResourceState(4915U, 0xFFFFFFFFU, 0xFFFFFFFFU);
    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});
    ASFW::Audio::Backends::DuplexIRMReservations reservations;

    for (uint8_t channel = 0; channel < 4; ++channel) {
        ASSERT_EQ(reservations.Reserve(irm, channel, 100U), kIOReturnSuccess);
    }
    EXPECT_EQ(reservations.Count(), 4U);
    EXPECT_EQ(reservations.Reserve(irm, 4, 100U), kIOReturnNoResources);
    EXPECT_EQ(bus.BandwidthAvailable(), 4915U - 4U * 100U);
    EXPECT_EQ(bus.ChannelsAvailable31_0(), 0x0FFFFFFFU);

    reservations.ReleaseAll();
    EXPECT_EQ(bus.BandwidthAvailable(), 4915U);
    EXPECT_EQ(bus.ChannelsAvailable31_0(), 0xFFFFFFFFU);
}

TEST(DuplexIRMReservationsTests,
     DuplexIRMReservationsRollsBackPriorSuccessAfterLaterFailure) {
    DuplexMockFireWireBus bus;
    // 500 units holds one 320-unit packet allocation but not two (640 units),
    // so the capture reservation is the one that is refused.
    bus.SetIRMResourceState(500U, 0xFFFFFFFFU, 0xFFFFFFFFU);
    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});
    ASFW::Audio::Backends::DuplexIRMReservationPair reservations;

    ASSERT_EQ(reservations.ReservePlayback(irm, 0, 320U), kIOReturnSuccess);
    EXPECT_EQ(reservations.ReserveCapture(irm, 1, 320U), kIOReturnNoResources);
    EXPECT_EQ(reservations.PlaybackCount(), 0U);
    EXPECT_EQ(reservations.CaptureCount(), 0U);

    reservations.ReleaseAll();
    EXPECT_EQ(bus.BandwidthAvailable(), 500U);
    EXPECT_EQ(bus.ChannelsAvailable31_0(), 0xFFFFFFFFU);
}

// Per Apple IOFWIsochChannel wire parity (IOFWIsochChannel.cpp:664),
// bandwidth reservations charge only the packet term against BANDWIDTH_AVAILABLE.
// The live gap count is retained in the charge record for diagnostics/CMP,
// but zero gap overhead is subtracted from the IRM ledger.
TEST(DuplexIRMReservationsTests,
     DuplexIRMReservationsChargesPacketUnitsWithoutGapOverhead) {
    DuplexMockFireWireBus bus;
    bus.SetIRMResourceState(4915U, 0xFFFFFFFFU, 0xFFFFFFFFU);
    bus.SetGapCount(5);
    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});
    ASFW::Audio::Backends::DuplexIRMReservations reservations;

    const auto result = reservations.ReserveAny(irm, uint64_t{1} << 0U, 1064U);

    ASSERT_EQ(result.status, kIOReturnSuccess);
    EXPECT_EQ(result.charge.gapCount, 5U);
    EXPECT_EQ(result.charge.packetUnits, 1064U);
    EXPECT_EQ(result.charge.overheadUnits, 0U);
    EXPECT_EQ(result.charge.Total(), 1064U);
    EXPECT_EQ(bus.BandwidthAvailable(), 4915U - 1064U);

    // The release hands back exactly what was taken.
    reservations.ReleaseAll();
    EXPECT_EQ(bus.BandwidthAvailable(), 4915U);
}

TEST(DuplexIRMReservationsTests,
     DuplexIRMReservationsReleaseOriginalChargesAfterGapCountChanges) {
    DuplexMockFireWireBus bus;
    bus.SetIRMResourceState(4915U, 0xFFFFFFFFU, 0xFFFFFFFFU);
    bus.SetGapCount(63);
    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});
    ASFW::Audio::Backends::DuplexIRMReservations reservations;

    const auto first = reservations.ReserveAny(irm, uint64_t{1} << 0U, 532U);
    ASSERT_EQ(first.status, kIOReturnSuccess);
    EXPECT_EQ(first.charge.Total(), 532U);

    // Change the reported gap without resetting the fake ledger. This isolates
    // charge lifetime: new allocations use the new gap, old ones retain theirs.
    bus.SetGapCount(5);
    const auto second = reservations.ReserveAny(irm, uint64_t{1} << 3U, 276U);
    ASSERT_EQ(second.status, kIOReturnSuccess);
    EXPECT_EQ(second.charge.Total(), 276U);
    EXPECT_EQ(bus.BandwidthAvailable(), 4915U - 532U - 276U);

    reservations.ReleaseAll();
    EXPECT_EQ(reservations.Count(), 0U);
    EXPECT_EQ(bus.BandwidthAvailable(), 4915U);
    EXPECT_EQ(bus.ChannelsAvailable31_0(), 0xFFFFFFFFU);

    reservations.ReleaseAll();
    EXPECT_EQ(bus.BandwidthAvailable(), 4915U);
}

// The reported failing configuration: a Midas Venice F24 carrying two
// playback streams (16, 8 slots) and two capture streams (16, 8 slots) at S200.
// Under Apple IOFWIsochChannel wire parity, all 4 streams consume 3232 units
// and fit within the 4915-unit ledger on both unoptimised (gap=63) and
// optimised (gap=5) buses.
TEST(DuplexIRMReservationsTests,
     VeniceF24StreamSetFitsOnBothUnoptimisedAndOptimisedBusesAtS200) {
    constexpr uint32_t kSixteenSlotsAtS200 = 1064U;
    constexpr uint32_t kEightSlotsAtS200 = 552U;
    constexpr uint32_t kTotalVeniceF24Bandwidth =
        2U * kSixteenSlotsAtS200 + 2U * kEightSlotsAtS200;  // 3232 units

    // Unoptimised bus (gap_count = 63, standard 2-node topology default):
    {
        DuplexMockFireWireBus bus;
        bus.SetIRMResourceState(4915U, 0xFFFFFFFFU, 0xFFFFFFFFU);
        bus.SetGapCount(63);
        IRMClient irm(bus);
        irm.SetIRMNode(0x03, Generation{1});
        ASFW::Audio::Backends::DuplexIRMReservationPair reservations;

        ASSERT_EQ(reservations.ReserveAnyPlayback(irm, uint64_t{1} << 0U, kSixteenSlotsAtS200).status,
                  kIOReturnSuccess);
        ASSERT_EQ(reservations.ReserveAnyPlayback(irm, uint64_t{1} << 2U, kEightSlotsAtS200).status,
                  kIOReturnSuccess);
        ASSERT_EQ(reservations.ReserveAnyCapture(irm, uint64_t{1} << 1U, kSixteenSlotsAtS200).status,
                  kIOReturnSuccess);
        ASSERT_EQ(reservations.ReserveAnyCapture(irm, uint64_t{1} << 3U, kEightSlotsAtS200).status,
                  kIOReturnSuccess);

        EXPECT_EQ(reservations.PlaybackCount(), 2U);
        EXPECT_EQ(reservations.CaptureCount(), 2U);
        EXPECT_EQ(bus.BandwidthAvailable(), 4915U - kTotalVeniceF24Bandwidth);

        reservations.ReleaseAll();
        EXPECT_EQ(bus.BandwidthAvailable(), 4915U);
        EXPECT_EQ(bus.ChannelsAvailable31_0(), 0xFFFFFFFFU);
    }

    // Optimised bus (gap_count = 5):
    {
        DuplexMockFireWireBus bus;
        bus.SetIRMResourceState(4915U, 0xFFFFFFFFU, 0xFFFFFFFFU);
        bus.SetGapCount(5);
        IRMClient irm(bus);
        irm.SetIRMNode(0x03, Generation{1});
        ASFW::Audio::Backends::DuplexIRMReservationPair reservations;

        ASSERT_EQ(reservations.ReserveAnyPlayback(irm, uint64_t{1} << 0U, kSixteenSlotsAtS200).status,
                  kIOReturnSuccess);
        ASSERT_EQ(reservations.ReserveAnyPlayback(irm, uint64_t{1} << 2U, kEightSlotsAtS200).status,
                  kIOReturnSuccess);
        ASSERT_EQ(reservations.ReserveAnyCapture(irm, uint64_t{1} << 1U, kSixteenSlotsAtS200).status,
                  kIOReturnSuccess);
        ASSERT_EQ(reservations.ReserveAnyCapture(irm, uint64_t{1} << 3U, kEightSlotsAtS200).status,
                  kIOReturnSuccess);

        EXPECT_EQ(reservations.PlaybackCount(), 2U);
        EXPECT_EQ(reservations.CaptureCount(), 2U);
        EXPECT_EQ(bus.BandwidthAvailable(), 4915U - kTotalVeniceF24Bandwidth);
    }
}

// kIOReturnNoResources on its own cannot tell a full bus from a planner that
// asked for nothing, so each refusal names which one it was.
TEST(DuplexIRMReservationsTests, DuplexIRMReservationsAttributesEmptyChannelMask) {
    DuplexMockFireWireBus bus;
    bus.SetIRMResourceState(4915U, 0xFFFFFFFFU, 0xFFFFFFFFU);
    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});
    ASFW::Audio::Backends::DuplexIRMReservations reservations;

    const auto result = reservations.ReserveAny(irm, 0U, 532U);

    EXPECT_EQ(result.status, kIOReturnNoResources);
    EXPECT_EQ(result.failure, ASFW::Audio::Backends::IsochReserveFailure::kNoChannelsAllowed);
    EXPECT_EQ(bus.BandwidthAvailable(), 4915U);
}

TEST(DuplexIRMReservationsTests, DuplexIRMReservationsAttributesAnAlreadyAllocatedChannel) {
    DuplexMockFireWireBus bus;
    // Bit 31 is channel 0: clear it to mark channel 0 taken by another node.
    bus.SetIRMResourceState(4915U, 0x7FFFFFFFU, 0xFFFFFFFFU);
    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});
    ASFW::Audio::Backends::DuplexIRMReservations reservations;

    const auto result = reservations.ReserveAny(irm, uint64_t{1} << 0U, 532U);

    EXPECT_EQ(result.status, kIOReturnNoResources);
    EXPECT_EQ(result.failure, ASFW::Audio::Backends::IsochReserveFailure::kChannelBusy);
    EXPECT_EQ(result.refusedChannels, uint64_t{1} << 0U);
    EXPECT_EQ(result.availableUnits, 4915U);
}

TEST(DuplexIRMReservationsTests, DuplexIRMReservationsAttributesBandwidthExhaustion) {
    DuplexMockFireWireBus bus;
    bus.SetIRMResourceState(500U, 0xFFFFFFFFU, 0xFFFFFFFFU);
    bus.SetGapCount(5);
    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});
    ASFW::Audio::Backends::DuplexIRMReservations reservations;

    const auto result = reservations.ReserveAny(irm, uint64_t{1} << 0U, 532U);

    EXPECT_EQ(result.status, kIOReturnNoResources);
    EXPECT_EQ(result.failure, ASFW::Audio::Backends::IsochReserveFailure::kBandwidthShort);
    EXPECT_EQ(result.charge.Total(), 532U);
    EXPECT_EQ(result.availableUnits, 500U);
    EXPECT_EQ(bus.BandwidthAvailable(), 500U);
}

TEST(DuplexIRMReservationsTests, DuplexIRMReservationsAttributesADirectionAtCapacity) {
    DuplexMockFireWireBus bus;
    bus.SetIRMResourceState(4915U, 0xFFFFFFFFU, 0xFFFFFFFFU);
    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});
    ASFW::Audio::Backends::DuplexIRMReservations reservations;

    for (uint8_t channel = 0; channel < 4; ++channel) {
        ASSERT_EQ(reservations.Reserve(irm, channel, 100U), kIOReturnSuccess);
    }
    const auto result = reservations.ReserveAny(irm, uint64_t{1} << 5U, 100U);

    EXPECT_EQ(result.status, kIOReturnNoResources);
    EXPECT_EQ(result.failure, ASFW::Audio::Backends::IsochReserveFailure::kTableFull);
}

TEST(DuplexIRMReservationsTests,
     DuplexIRMReservationsPropagatesGenerationMismatchWithoutTrackingEntry) {
    DuplexMockFireWireBus bus;
    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});
    bus.SetGeneration(Generation{2});
    ASFW::Audio::Backends::DuplexIRMReservations reservations;

    EXPECT_EQ(reservations.Reserve(irm, 0, 320U), kIOReturnOffline);
    EXPECT_EQ(reservations.Count(), 0U);
}

// AudioDuplexChannels invariant: stream[0] resolves to the legacy scalar field so
// single-stream call sites that only set the scalar stay byte-for-byte unchanged;
// the per-stream arrays carry the *additional* streams (Venice F32 = 2×16).
TEST(DuplexIRMReservationsTests, DuplexChannelsStreamZeroIsLegacyScalar) {
    AudioDuplexChannels ch{};
    ch.deviceToHostIsoChannel = 7;  // capture stream[0] (host IR)
    ch.hostToDeviceIsoChannel = 5;  // playback stream[0] (host IT)
    // Arrays still at defaults; stream[0] must ignore them.
    EXPECT_EQ(ch.CaptureChannel(0), 7);
    EXPECT_EQ(ch.PlaybackChannel(0), 5);
}

TEST(DuplexIRMReservationsTests, DuplexChannelsAdditionalStreamsUseArrays) {
    AudioDuplexChannels ch{};
    ch.deviceToHostIsoChannel = 1;
    ch.hostToDeviceIsoChannel = 0;
    ch.captureStreamCount = 2;
    ch.playbackStreamCount = 2;
    ch.captureIsoChannels[1] = 2;   // capture stream[1]
    ch.playbackIsoChannels[1] = 3;  // playback stream[1]

    EXPECT_EQ(ch.CaptureChannel(0), 1);
    EXPECT_EQ(ch.CaptureChannel(1), 2);
    EXPECT_EQ(ch.PlaybackChannel(0), 0);
    EXPECT_EQ(ch.PlaybackChannel(1), 3);
    // All four wire channels distinct — no bus collision across the duplex set.
    EXPECT_NE(ch.CaptureChannel(0), ch.CaptureChannel(1));
    EXPECT_NE(ch.PlaybackChannel(0), ch.PlaybackChannel(1));
    EXPECT_NE(ch.CaptureChannel(0), ch.PlaybackChannel(0));
    EXPECT_NE(ch.CaptureChannel(1), ch.PlaybackChannel(1));
}

} // namespace
