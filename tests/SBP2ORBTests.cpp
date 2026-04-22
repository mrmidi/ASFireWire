#include <gtest/gtest.h>

#include "ASFWDriver/Protocols/SBP2/SBP2CommandORB.hpp"
#include "ASFWDriver/Protocols/SBP2/SBP2ManagementORB.hpp"
#include "ASFWDriver/Protocols/SBP2/SBP2WireFormats.hpp"
#include "ASFWDriver/Testing/HostDriverKitStubs.hpp"
#include "tests/mocks/DeferredFireWireBus.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace {

using ASFW::Protocols::SBP2::AddressSpaceManager;
using ASFW::Protocols::SBP2::SBP2CommandORB;
using ASFW::Protocols::SBP2::SBP2ManagementORB;
using ASFW::Protocols::SBP2::Wire::FromBE32;
using ASFW::Protocols::SBP2::Wire::ManagementAgentAddressLo;
using ASFW::Protocols::SBP2::Wire::StatusBlock;
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
