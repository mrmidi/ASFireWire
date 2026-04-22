#include <gtest/gtest.h>

#include "ASFWDriver/Protocols/SBP2/SBP2LoginSession.hpp"
#include "ASFWDriver/Testing/HostDriverKitStubs.hpp"
#include "tests/mocks/DeferredFireWireBus.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace {

using ASFW::Protocols::SBP2::AddressSpaceManager;
using ASFW::Protocols::SBP2::LoginState;
using ASFW::Protocols::SBP2::SBP2CommandORB;
using ASFW::Protocols::SBP2::SBP2LoginSession;
using ASFW::Protocols::SBP2::SBP2TargetInfo;
using ASFW::Protocols::SBP2::Wire::FromBE16;
using ASFW::Protocols::SBP2::Wire::FromBE32;
using ASFW::Protocols::SBP2::Wire::LoginORB;
using ASFW::Protocols::SBP2::Wire::LoginResponse;
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

uint64_t ReadORBAddress(AddressSpaceManager& manager,
                        uint64_t orbAddress,
                        size_t hiOffset,
                        size_t loOffset) {
    const uint32_t hi = FromBE32(ReadQuadlet(manager, orbAddress + hiOffset));
    const uint32_t lo = FromBE32(ReadQuadlet(manager, orbAddress + loOffset));
    return ComposeAddress(static_cast<uint16_t>(hi & 0xFFFFu), lo);
}

class SessionRig {
public:
    SessionRig()
        : session(bus, bus, addressManager) {
        queue.SetManualDispatchForTesting(true);
        ASFW::Testing::SetHostMonotonicClockForTesting([this]() { return nowNs; });

        bus.SetGeneration(ASFW::FW::Generation{1});
        bus.SetLocalNodeID(ASFW::FW::NodeId{0x2A});
        bus.SetDefaultSpeed(ASFW::FW::FwSpeed::S400);

        SBP2TargetInfo info{};
        info.managementAgentOffset = 0x100;
        info.lun = 3;
        info.managementTimeoutMs = 10;
        info.maxORBSize = 32;
        info.maxCommandBlockSize = 16;

        session.SetWorkQueue(&queue);
        session.Configure(info);
    }

    ~SessionRig() {
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

    void LoginSuccessfully(uint16_t loginId = 0x0042, uint32_t commandBlockAgentLo = 0x0020'0000) {
        ASSERT_TRUE(session.Login());
        ASSERT_EQ(1u, bus.PendingWriteCount());

        const auto& loginWrite = bus.WriteAt(0);
        const uint64_t loginOrbAddress = DecodeOrbAddressFromPayload(loginWrite.data);
        const uint64_t loginResponseAddress =
            ReadORBAddress(addressManager,
                           loginOrbAddress,
                           offsetof(LoginORB, loginResponseAddressHi),
                           offsetof(LoginORB, loginResponseAddressLo));
        const uint64_t statusAddress =
            ReadORBAddress(addressManager,
                           loginOrbAddress,
                           offsetof(LoginORB, statusFIFOAddressHi),
                           offsetof(LoginORB, statusFIFOAddressLo));
        sessionStatusAddress = statusAddress;

        LoginResponse response{};
        response.length = ToBE16(LoginResponse::kSize);
        response.loginID = ToBE16(loginId);
        response.commandBlockAgentAddressHi = ToBE32(0x0000'FFFFu);
        response.commandBlockAgentAddressLo = ToBE32(commandBlockAgentLo);
        response.reconnectHold = ToBE16(1);

        ASSERT_TRUE(bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
        addressManager.ApplyRemoteWrite(
            loginResponseAddress,
            std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&response), sizeof(response)});

        StatusBlock status{};
        status.details = 0;
        status.sbpStatus = SBPStatus::kNoAdditionalInfo;
        addressManager.ApplyRemoteWrite(
            statusAddress,
            std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&status), sizeof(status)});

        while (bus.PendingWriteCount() > 0U) {
            ASSERT_TRUE(bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
        }
        DrainReady();
        ASSERT_EQ(LoginState::LoggedIn, session.State());
    }

    ASFW::Async::Testing::DeferredFireWireBus bus;
    AddressSpaceManager addressManager{nullptr};
    SBP2LoginSession session;
    IODispatchQueue queue;
    uint64_t nowNs{0};
    uint64_t sessionStatusAddress{0};
};

TEST(SBP2LoginSessionTests, LoginAckCancelsStaleTimeoutBeforeStatusArrives) {
    SessionRig rig;

    ASSERT_TRUE(rig.session.Login());
    ASSERT_EQ(LoginState::LoggingIn, rig.session.State());
    ASSERT_EQ(1u, rig.bus.PendingWriteCount());

    rig.AdvanceMs(5);
    ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    rig.DrainReady();

    rig.AdvanceMs(5);

    EXPECT_EQ(LoginState::LoggingIn, rig.session.State());
    EXPECT_EQ(1u, rig.bus.WriteCount());
}

TEST(SBP2LoginSessionTests, BusResetWhileLoggingInRetriesLoginAfterDelay) {
    SessionRig rig;

    ASSERT_TRUE(rig.session.Login());
    ASSERT_EQ(LoginState::LoggingIn, rig.session.State());
    ASSERT_EQ(1u, rig.bus.WriteCount());

    rig.bus.SetGeneration(ASFW::FW::Generation{2});
    rig.session.HandleBusReset(2);
    EXPECT_EQ(LoginState::Idle, rig.session.State());

    rig.AdvanceMs(99);
    EXPECT_EQ(1u, rig.bus.WriteCount());

    rig.AdvanceMs(1);
    EXPECT_EQ(LoginState::LoggingIn, rig.session.State());
    EXPECT_EQ(2u, rig.bus.WriteCount());
    EXPECT_EQ(2u, rig.session.Generation());
}

TEST(SBP2LoginSessionTests, BusResetWhileReconnectingRetriesReconnectAfterDelay) {
    SessionRig rig;
    rig.LoginSuccessfully();

    rig.bus.SetGeneration(ASFW::FW::Generation{2});
    rig.session.HandleBusReset(2);
    ASSERT_EQ(LoginState::Suspended, rig.session.State());
    ASSERT_TRUE(rig.session.Reconnect());
    ASSERT_EQ(LoginState::Reconnecting, rig.session.State());
    ASSERT_EQ(2u, rig.session.Generation());
    ASSERT_EQ(3u, rig.bus.WriteCount());

    rig.bus.SetGeneration(ASFW::FW::Generation{3});
    rig.session.HandleBusReset(3);
    EXPECT_EQ(LoginState::Suspended, rig.session.State());

    rig.AdvanceMs(99);
    EXPECT_EQ(3u, rig.bus.WriteCount());

    rig.AdvanceMs(1);
    EXPECT_EQ(LoginState::Reconnecting, rig.session.State());
    EXPECT_EQ(4u, rig.bus.WriteCount());
    EXPECT_EQ(3u, rig.session.Generation());
}

TEST(SBP2LoginSessionTests, ImmediateORBRetryStaysBoundToOriginalORBAndQueuesNextImmediate) {
    SessionRig rig;
    rig.LoginSuccessfully();

    SBP2CommandORB first(rig.addressManager, &rig.session, 16);
    first.SetFlags(SBP2CommandORB::kImmediate);
    first.SetTimeout(50);

    SBP2CommandORB second(rig.addressManager, &rig.session, 16);
    second.SetFlags(SBP2CommandORB::kImmediate);

    ASSERT_TRUE(rig.session.SubmitORB(&first));
    ASSERT_EQ(1u, rig.bus.PendingWriteCount());
    const size_t firstFetchWriteIndex = rig.bus.WriteCount() - 1;
    const uint32_t firstAddressLo = static_cast<uint32_t>(
        DecodeOrbAddressFromPayload(rig.bus.WriteAt(firstFetchWriteIndex).data));

    ASSERT_TRUE(rig.session.SubmitORB(&second));
    EXPECT_EQ(3u, rig.bus.WriteCount());

    ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kTimeout));
    rig.DrainReady();

    rig.AdvanceMs(999);
    EXPECT_EQ(3u, rig.bus.WriteCount());

    rig.AdvanceMs(1);
    ASSERT_EQ(4u, rig.bus.WriteCount());
    const uint32_t retryAddressLo = static_cast<uint32_t>(
        DecodeOrbAddressFromPayload(rig.bus.WriteAt(3).data));
    EXPECT_EQ(firstAddressLo, retryAddressLo);
}

TEST(SBP2LoginSessionTests, SolicitedStatusCompletesORBMatchingByORBAddress) {
    SessionRig rig;
    rig.LoginSuccessfully();

    ASSERT_NE(0u, rig.session.CommandBlockAgent().addressLo);

    SBP2CommandORB first(rig.addressManager, &rig.session, 16);
    first.SetFlags(0);
    int firstStatus = 99;
    first.SetCompletionCallback([&firstStatus](int status) { firstStatus = status; });

    SBP2CommandORB second(rig.addressManager, &rig.session, 16);
    second.SetFlags(0);
    int secondStatus = 99;
    second.SetCompletionCallback([&secondStatus](int status) { secondStatus = status; });

    ASSERT_TRUE(rig.session.SubmitORB(&first));
    ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    rig.DrainReady();

    ASSERT_TRUE(rig.session.SubmitORB(&second));
    ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    rig.DrainReady();

    StatusBlock status{};
    const auto firstAddress = first.GetORBAddress();
    status.details = 0;
    status.sbpStatus = SBPStatus::kNoAdditionalInfo;
    status.orbOffsetHi = ToBE16(firstAddress.addressHi);
    status.orbOffsetLo = ToBE32(firstAddress.addressLo);

    rig.addressManager.ApplyRemoteWrite(
        rig.sessionStatusAddress,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&status), sizeof(status)});

    EXPECT_EQ(0, firstStatus);
    EXPECT_EQ(99, secondStatus);
}

} // namespace
