#include <gtest/gtest.h>

#include "ASFWDriver/Protocols/SBP2/SBP2CommandORB.hpp"
#include "ASFWDriver/Protocols/SBP2/SBP2ManagementORB.hpp"
#include "ASFWDriver/Protocols/SBP2/SBP2PageTable.hpp"
#include "ASFWDriver/Protocols/SBP2/SBP2WireFormats.hpp"
#include "ASFWDriver/Testing/HostDriverKitStubs.hpp"
#include "tests/mocks/DeferredFireWireBus.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace {

using ASFW::Protocols::SBP2::AddressSpaceManager;
using ASFW::Protocols::SBP2::SBP2CommandORB;
using ASFW::Protocols::SBP2::SBP2ManagementORB;
using ASFW::Protocols::SBP2::SBP2PageTable;
using ASFW::Protocols::SBP2::Wire::FromBE16;
using ASFW::Protocols::SBP2::Wire::FromBE32;
using ASFW::Protocols::SBP2::Wire::ManagementAgentAddressLo;
using ASFW::Protocols::SBP2::Wire::NormalizeBusNodeID;
using ASFW::Protocols::SBP2::Wire::StatusBlock;
using ASFW::Protocols::SBP2::Wire::ToBE16;
using ASFW::Protocols::SBP2::Wire::ToBE32;
namespace SBPStatus = ASFW::Protocols::SBP2::Wire::SBPStatus;

uint64_t ComposeAddress(uint16_t hi, uint32_t lo) {
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

uint64_t DecodeOrbAddressFromPayload(std::span<const uint8_t> payload) {
    const uint16_t addressHi =
        static_cast<uint16_t>((static_cast<uint16_t>(payload[2]) << 8) | payload[3]);
    const uint32_t addressLo =
        (static_cast<uint32_t>(payload[4]) << 24) |
        (static_cast<uint32_t>(payload[5]) << 16) |
        (static_cast<uint32_t>(payload[6]) << 8) |
        static_cast<uint32_t>(payload[7]);
    return ComposeAddress(addressHi, addressLo);
}

uint32_t ReadQuadlet(AddressSpaceManager& manager, uint64_t address) {
    uint32_t value = 0;
    EXPECT_EQ(ASFW::Async::ResponseCode::Complete, manager.ReadQuadlet(address, &value));
    return value;
}

uint64_t ReadStatusAddressFromManagementORB(AddressSpaceManager& manager, uint64_t orbAddress) {
    const uint32_t hi = FromBE32(ReadQuadlet(
        manager,
        orbAddress + offsetof(ASFW::Protocols::SBP2::Wire::TaskManagementORB, statusFIFOAddressHi)));
    const uint32_t lo = FromBE32(ReadQuadlet(
        manager,
        orbAddress + offsetof(ASFW::Protocols::SBP2::Wire::TaskManagementORB, statusFIFOAddressLo)));
    return ComposeAddress(static_cast<uint16_t>(hi & 0xFFFFu), lo);
}

class ORBTimerRig {
public:
    ORBTimerRig() {
        queue.SetManualDispatchForTesting(true);
        ASFW::Testing::SetHostMonotonicClockForTesting([this]() { return nowNs; });

        bus.SetGeneration(ASFW::FW::Generation{1});
        bus.SetLocalNodeID(ASFW::FW::NodeId{0x21});
        bus.SetDefaultSpeed(ASFW::FW::FwSpeed::S400);
    }

    ~ORBTimerRig() {
        ASFW::Testing::ResetHostMonotonicClockForTesting();
    }

    void DrainReady() {
        while (queue.DrainReadyForTesting() > 0U) {
        }
    }

    void AdvanceMs(uint64_t milliseconds) {
        nowNs += milliseconds * 1'000'000ULL;
        DrainReady();
    }

    ASFW::Async::Testing::DeferredFireWireBus bus;
    AddressSpaceManager addressManager{nullptr};
    IODispatchQueue queue;
    uint64_t nowNs{0};
};

TEST(SBP2ORBTests, CommandORBTimerFiresOnHostQueue) {
    ORBTimerRig rig;

    SBP2CommandORB orb(rig.addressManager, reinterpret_cast<void*>(0x1), 16);
    int completionStatus = 99;
    orb.SetTimeout(5);
    orb.SetCompletionCallback([&completionStatus](int status, uint8_t) { completionStatus = status; });

    orb.StartTimer(&rig.queue);
    rig.AdvanceMs(5);

    EXPECT_EQ(-1, completionStatus);
}

TEST(SBP2ORBTests, CommandORBCancelSuppressesPendingTimeout) {
    ORBTimerRig rig;

    SBP2CommandORB orb(rig.addressManager, reinterpret_cast<void*>(0x2), 16);
    int completionCount = 0;
    orb.SetTimeout(5);
    orb.SetCompletionCallback([&completionCount](int, uint8_t) { ++completionCount; });

    orb.StartTimer(&rig.queue);
    orb.CancelTimer();
    rig.AdvanceMs(5);

    EXPECT_EQ(0, completionCount);
}

TEST(SBP2ORBTests, CommandORBDestructionInvalidatesPendingTimeout) {
    ORBTimerRig rig;

    int completionCount = 0;
    {
        auto orb = std::make_unique<SBP2CommandORB>(
            rig.addressManager, reinterpret_cast<void*>(0x3), 16);
        orb->SetTimeout(5);
        orb->SetCompletionCallback([&completionCount](int, uint8_t) { ++completionCount; });
        orb->StartTimer(&rig.queue);
    }

    rig.AdvanceMs(5);
    EXPECT_EQ(0, completionCount);
}

TEST(SBP2ORBTests, PageTableUsesDirectDescriptorForSingleAlignedSegment) {
    ORBTimerRig rig;

    SBP2PageTable pageTable(rig.addressManager, reinterpret_cast<void*>(0x40));
    const std::array<SBP2PageTable::Segment, 1> segments{{
        {.address = 0x0001'2345'6000ULL, .length = 512},
    }};

    ASSERT_TRUE(pageTable.Build(segments, 0x21));

    const auto& result = pageTable.GetResult();
    EXPECT_TRUE(result.isDirect);
    EXPECT_EQ(1u, pageTable.EntryCount());
    EXPECT_EQ(0x0001u, FromBE32(result.dataDescriptorHi) & 0xFFFFu);
    EXPECT_EQ(0x2345'6000u, FromBE32(result.dataDescriptorLo));
    EXPECT_EQ(512u, FromBE16(result.dataSize));
    EXPECT_EQ(0u, result.options);
}

TEST(SBP2ORBTests, PageTableSplitsSegmentsIntoPublishedEntries) {
    ORBTimerRig rig;

    SBP2PageTable pageTable(rig.addressManager, reinterpret_cast<void*>(0x41));
    const std::array<SBP2PageTable::Segment, 1> segments{{
        {.address = 0x0001'0000'1000ULL, .length = 0x30},
    }};

    ASSERT_TRUE(pageTable.Build(segments, 0x21, 0x10));

    const auto& result = pageTable.GetResult();
    ASSERT_FALSE(result.isDirect);
    ASSERT_EQ(3u, pageTable.EntryCount());
    EXPECT_EQ(3u, FromBE16(result.dataSize));
    EXPECT_EQ(ASFW::Protocols::SBP2::Wire::Options::kPageTableUnrestricted,
              result.options);

    const uint32_t descriptorHi = FromBE32(result.dataDescriptorHi);
    const uint16_t expectedNode = NormalizeBusNodeID(0x21);
    EXPECT_EQ(expectedNode, static_cast<uint16_t>(descriptorHi >> 16));
    EXPECT_EQ(0xFFFFu, descriptorHi & 0xFFFFu);

    const uint64_t tableAddress =
        ComposeAddress(static_cast<uint16_t>(descriptorHi & 0xFFFFu),
                       FromBE32(result.dataDescriptorLo));
    const uint32_t firstEntryHeader = FromBE32(ReadQuadlet(rig.addressManager, tableAddress));
    const uint32_t firstEntryLo = FromBE32(ReadQuadlet(rig.addressManager, tableAddress + 4));

    EXPECT_EQ(0x0010u, firstEntryHeader >> 16);
    EXPECT_EQ(0x0001u, firstEntryHeader & 0xFFFFu);
    EXPECT_EQ(0x0000'1000u, firstEntryLo);
}

TEST(SBP2ORBTests, ManagementORBStatusWriteCancelsTimeout) {
    ORBTimerRig rig;

    SBP2ManagementORB orb(rig.bus, rig.bus, rig.addressManager, reinterpret_cast<void*>(0x4));
    orb.SetFunction(SBP2ManagementORB::Function::AbortTaskSet);
    orb.SetLoginID(0x12);
    orb.SetManagementAgentOffset(0x80);
    orb.SetTargetNode(1, 0x3F);
    orb.SetTimeout(5);
    orb.SetWorkQueue(&rig.queue);

    int completionStatus = 99;
    orb.SetCompletionCallback([&completionStatus](int status) { completionStatus = status; });

    ASSERT_TRUE(orb.Execute());
    ASSERT_EQ(1u, rig.bus.PendingWriteCount());

    const auto& write = rig.bus.WriteAt(0);
    ASSERT_EQ(ManagementAgentAddressLo(0x80), write.address.addressLo);
    const uint64_t orbAddress = DecodeOrbAddressFromPayload(write.data);

    ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    rig.DrainReady();

    StatusBlock status{};
    status.details = 0;
    status.sbpStatus = SBPStatus::kNoAdditionalInfo;
    rig.addressManager.ApplyRemoteWrite(
        ReadStatusAddressFromManagementORB(rig.addressManager, orbAddress),
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&status), sizeof(status)});

    rig.AdvanceMs(5);
    EXPECT_EQ(0, completionStatus);
}

TEST(SBP2ORBTests, ManagementORBUsesFullBusNodeIdInEmbeddedAddresses) {
    ORBTimerRig rig;

    SBP2ManagementORB orb(rig.bus, rig.bus, rig.addressManager, reinterpret_cast<void*>(0x6));
    orb.SetFunction(SBP2ManagementORB::Function::AbortTaskSet);
    orb.SetLoginID(0x12);
    orb.SetManagementAgentOffset(0x80);
    orb.SetTargetNode(1, 0x3F);

    ASSERT_TRUE(orb.Execute());
    ASSERT_EQ(1u, rig.bus.PendingWriteCount());

    const auto& write = rig.bus.WriteAt(0);
    const uint16_t payloadNode =
        static_cast<uint16_t>((static_cast<uint16_t>(write.data[0]) << 8) | write.data[1]);
    const uint64_t orbAddress = DecodeOrbAddressFromPayload(write.data);
    const uint32_t statusHi = FromBE32(ReadQuadlet(
        rig.addressManager,
        orbAddress + offsetof(ASFW::Protocols::SBP2::Wire::TaskManagementORB, statusFIFOAddressHi)));

    const uint16_t expectedNode = NormalizeBusNodeID(0x21);
    EXPECT_EQ(expectedNode, payloadNode);
    EXPECT_EQ((static_cast<uint32_t>(expectedNode) << 16) | 0xFFFFu, statusHi);
}

TEST(SBP2ORBTests, CommandORBDirectDescriptorUsesFullBusNodeId) {
    ORBTimerRig rig;

    SBP2CommandORB orb(rig.addressManager, reinterpret_cast<void*>(0x7), 16);
    ASFW::Protocols::SBP2::SBP2PageTable::Result descriptor{};
    descriptor.dataDescriptorHi = ToBE32(0x0000FFFFu);
    descriptor.dataDescriptorLo = ToBE32(0x00112200u);
    descriptor.dataSize = ToBE16(512);
    descriptor.isDirect = true;

    orb.SetDataDescriptor(descriptor);
    orb.PrepareForExecution(0x21, ASFW::FW::FwSpeed::S400, 6);

    const auto orbAddress = orb.GetORBAddress();
    const uint64_t packedAddress = ComposeAddress(orbAddress.addressHi, orbAddress.addressLo);
    const uint32_t dataDescriptorHi = FromBE32(ReadQuadlet(
        rig.addressManager,
        packedAddress + offsetof(ASFW::Protocols::SBP2::Wire::NormalORB, dataDescriptorHi)));

    const uint16_t expectedNode = NormalizeBusNodeID(0x21);
    EXPECT_EQ((static_cast<uint32_t>(expectedNode) << 16) | 0xFFFFu, dataDescriptorHi);
}

TEST(SBP2ORBTests, ManagementORBDestructionInvalidatesPendingTimeout) {
    ORBTimerRig rig;

    int completionCount = 0;
    {
        auto orb = std::make_unique<SBP2ManagementORB>(
            rig.bus, rig.bus, rig.addressManager, reinterpret_cast<void*>(0x5));
        orb->SetFunction(SBP2ManagementORB::Function::AbortTaskSet);
        orb->SetLoginID(0x34);
        orb->SetManagementAgentOffset(0x81);
        orb->SetTargetNode(1, 0x3F);
        orb->SetTimeout(5);
        orb->SetWorkQueue(&rig.queue);
        orb->SetCompletionCallback([&completionCount](int) { ++completionCount; });

        ASSERT_TRUE(orb->Execute());
        ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
        rig.DrainReady();
    }

    rig.AdvanceMs(5);
    EXPECT_EQ(0, completionCount);
}

} // namespace
