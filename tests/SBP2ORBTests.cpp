#include <gtest/gtest.h>

#include "ASFWDriver/Protocols/SBP2/SBP2CommandORB.hpp"
#include "ASFWDriver/Protocols/SBP2/SBP2ManagementORB.hpp"
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
        workQueue.SetManualDispatchForTesting(true);
        timeoutQueue.SetManualDispatchForTesting(true);
        ASFW::Testing::SetHostMonotonicClockForTesting([this]() { return nowNs; });

        bus.SetGeneration(ASFW::FW::Generation{1});
        bus.SetLocalNodeID(ASFW::FW::NodeId{0x21});
        bus.SetDefaultSpeed(ASFW::FW::FwSpeed::S400);
    }

    ~ORBTimerRig() {
        ASFW::Testing::ResetHostMonotonicClockForTesting();
    }

    void DrainReady() {
        while (workQueue.DrainReadyForTesting() > 0U ||
               timeoutQueue.DrainReadyForTesting() > 0U) {
        }
    }

    void AdvanceMs(uint64_t milliseconds) {
        nowNs += milliseconds * 1'000'000ULL;
        DrainReady();
    }

    ASFW::Async::Testing::DeferredFireWireBus bus;
    AddressSpaceManager addressManager{nullptr};
    IODispatchQueue workQueue;
    IODispatchQueue timeoutQueue;
    uint64_t nowNs{0};
};

TEST(SBP2ORBTests, CommandORBTimerFiresOnHostQueue) {
    ORBTimerRig rig;

    SBP2CommandORB orb(rig.addressManager, reinterpret_cast<void*>(0x1), 16);
    int completionStatus = 99;
    orb.SetTimeout(5);
    orb.SetCompletionCallback([&completionStatus](int status, uint8_t) { completionStatus = status; });

    orb.StartTimer(&rig.workQueue, &rig.timeoutQueue);
    rig.AdvanceMs(5);

    EXPECT_EQ(-1, completionStatus);
}

TEST(SBP2ORBTests, CommandORBCancelSuppressesPendingTimeout) {
    ORBTimerRig rig;

    SBP2CommandORB orb(rig.addressManager, reinterpret_cast<void*>(0x2), 16);
    int completionCount = 0;
    orb.SetTimeout(5);
    orb.SetCompletionCallback([&completionCount](int, uint8_t) { ++completionCount; });

    orb.StartTimer(&rig.workQueue, &rig.timeoutQueue);
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
        orb->StartTimer(&rig.workQueue, &rig.timeoutQueue);
    }

    rig.AdvanceMs(5);
    EXPECT_EQ(0, completionCount);
}

TEST(SBP2ORBTests, ManagementORBStatusWriteCancelsTimeout) {
    ORBTimerRig rig;

    SBP2ManagementORB orb(rig.bus, rig.bus, rig.addressManager, reinterpret_cast<void*>(0x4));
    orb.SetFunction(SBP2ManagementORB::Function::AbortTaskSet);
    orb.SetLoginID(0x12);
    orb.SetManagementAgentOffset(0x80);
    orb.SetTargetNode(1, 0x3F);
    orb.SetTimeout(5);
    orb.SetWorkQueue(&rig.workQueue);
    orb.SetTimeoutQueue(&rig.timeoutQueue);

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
    status.orbOffsetHi = ToBE16(static_cast<uint16_t>((orbAddress >> 32) & 0xFFFFu));
    status.orbOffsetLo = ToBE32(static_cast<uint32_t>(orbAddress & 0xFFFF'FFFFu));
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
    ASSERT_EQ(kIOReturnSuccess, orb.PrepareForExecution(0x21, ASFW::FW::FwSpeed::S400, 6));

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
        orb->SetWorkQueue(&rig.workQueue);
        orb->SetTimeoutQueue(&rig.timeoutQueue);
        orb->SetCompletionCallback([&completionCount](int) { ++completionCount; });

        ASSERT_TRUE(orb->Execute());
        ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
        rig.DrainReady();
    }

    rig.AdvanceMs(5);
    EXPECT_EQ(0, completionCount);
}

TEST(SBP2ORBTests, ManagementORBPropagatesDeviceStatusFailure) {
    ORBTimerRig rig;

    SBP2ManagementORB orb(rig.bus, rig.bus, rig.addressManager, reinterpret_cast<void*>(0x8));
    orb.SetFunction(SBP2ManagementORB::Function::LogicalUnitReset);
    orb.SetLoginID(0x44);
    orb.SetManagementAgentOffset(0x90);
    orb.SetTargetNode(1, 0x3F);
    orb.SetTimeout(5);
    orb.SetWorkQueue(&rig.workQueue);
    orb.SetTimeoutQueue(&rig.timeoutQueue);

    int completionStatus = 99;
    orb.SetCompletionCallback([&completionStatus](int status) { completionStatus = status; });

    ASSERT_TRUE(orb.Execute());
    const auto& write = rig.bus.WriteAt(0);
    const uint64_t orbAddress = DecodeOrbAddressFromPayload(write.data);

    ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    rig.DrainReady();

    StatusBlock status{};
    status.details = 0;
    status.sbpStatus = SBPStatus::kFunctionRejected;
    status.orbOffsetHi = ToBE16(static_cast<uint16_t>((orbAddress >> 32) & 0xFFFFu));
    status.orbOffsetLo = ToBE32(static_cast<uint32_t>(orbAddress & 0xFFFF'FFFFu));
    rig.addressManager.ApplyRemoteWrite(
        ReadStatusAddressFromManagementORB(rig.addressManager, orbAddress),
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&status), sizeof(status)});

    rig.DrainReady();
    EXPECT_EQ(-4, completionStatus);
}

TEST(SBP2ORBTests, ManagementORBRejectsMalformedStatusPayload) {
    ORBTimerRig rig;

    SBP2ManagementORB orb(rig.bus, rig.bus, rig.addressManager, reinterpret_cast<void*>(0x9));
    orb.SetFunction(SBP2ManagementORB::Function::AbortTaskSet);
    orb.SetLoginID(0x45);
    orb.SetManagementAgentOffset(0x91);
    orb.SetTargetNode(1, 0x3F);
    orb.SetTimeout(5);
    orb.SetWorkQueue(&rig.workQueue);
    orb.SetTimeoutQueue(&rig.timeoutQueue);

    int completionStatus = 99;
    orb.SetCompletionCallback([&completionStatus](int status) { completionStatus = status; });

    ASSERT_TRUE(orb.Execute());
    const auto& write = rig.bus.WriteAt(0);
    const uint64_t orbAddress = DecodeOrbAddressFromPayload(write.data);

    ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    rig.DrainReady();

    const std::array<uint8_t, 4> shortPayload{0, 0, 0, 0};
    rig.addressManager.ApplyRemoteWrite(
        ReadStatusAddressFromManagementORB(rig.addressManager, orbAddress),
        std::span<const uint8_t>{shortPayload.data(), shortPayload.size()});

    rig.DrainReady();
    EXPECT_EQ(-3, completionStatus);
}

TEST(SBP2ORBTests, ManagementORBRejectsMismatchedStatusORBAddress) {
    ORBTimerRig rig;

    SBP2ManagementORB orb(rig.bus, rig.bus, rig.addressManager, reinterpret_cast<void*>(0xA));
    orb.SetFunction(SBP2ManagementORB::Function::AbortTaskSet);
    orb.SetLoginID(0x46);
    orb.SetManagementAgentOffset(0x92);
    orb.SetTargetNode(1, 0x3F);
    orb.SetTimeout(5);
    orb.SetWorkQueue(&rig.workQueue);
    orb.SetTimeoutQueue(&rig.timeoutQueue);

    int completionStatus = 99;
    orb.SetCompletionCallback([&completionStatus](int status) { completionStatus = status; });

    ASSERT_TRUE(orb.Execute());
    const auto& write = rig.bus.WriteAt(0);
    const uint64_t orbAddress = DecodeOrbAddressFromPayload(write.data);

    ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    rig.DrainReady();

    StatusBlock status{};
    status.details = 0;
    status.sbpStatus = SBPStatus::kNoAdditionalInfo;
    status.orbOffsetHi = ToBE16(static_cast<uint16_t>(((orbAddress + 8) >> 32) & 0xFFFFu));
    status.orbOffsetLo = ToBE32(static_cast<uint32_t>((orbAddress + 8) & 0xFFFF'FFFFu));
    rig.addressManager.ApplyRemoteWrite(
        ReadStatusAddressFromManagementORB(rig.addressManager, orbAddress),
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&status), sizeof(status)});

    rig.DrainReady();
    EXPECT_EQ(-3, completionStatus);
}

TEST(SBP2ORBTests, ManagementORBDestroyedAfterExecuteIgnoresPendingWriteAndStatus) {
    ORBTimerRig rig;

    int completionCount = 0;
    uint64_t statusAddress = 0;
    {
        auto orb = std::make_unique<SBP2ManagementORB>(
            rig.bus, rig.bus, rig.addressManager, reinterpret_cast<void*>(0xB));
        orb->SetFunction(SBP2ManagementORB::Function::AbortTaskSet);
        orb->SetLoginID(0x47);
        orb->SetManagementAgentOffset(0x93);
        orb->SetTargetNode(1, 0x3F);
        orb->SetTimeout(5);
        orb->SetWorkQueue(&rig.workQueue);
        orb->SetTimeoutQueue(&rig.timeoutQueue);
        orb->SetCompletionCallback([&completionCount](int) { ++completionCount; });

        ASSERT_TRUE(orb->Execute());
        const auto& write = rig.bus.WriteAt(0);
        const uint64_t orbAddress = DecodeOrbAddressFromPayload(write.data);
        statusAddress = ReadStatusAddressFromManagementORB(rig.addressManager, orbAddress);
    }

    EXPECT_EQ(0u, rig.bus.PendingWriteCount());
    StatusBlock status{};
    status.details = 0;
    status.sbpStatus = SBPStatus::kNoAdditionalInfo;
    rig.addressManager.ApplyRemoteWrite(
        statusAddress,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&status), sizeof(status)});
    rig.AdvanceMs(5);

    EXPECT_EQ(0, completionCount);
}

} // namespace
