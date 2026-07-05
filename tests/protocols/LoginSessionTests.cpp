#include <gtest/gtest.h>

#include "ASFWDriver/Protocols/SBP2/Session/LoginSession.hpp"
#include "ASFWDriver/Testing/HostDriverKitStubs.hpp"
#include "tests/mocks/DeferredFireWireBus.hpp"
#include "FakeSessionScheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// LoginSession host tests — ported from PR #19's SBP2LoginSessionTests, adapted to
// the decomposed DICE component: the class is `LoginSession`, timers run on an
// injected ISessionScheduler (FakeSessionScheduler virtual clock) instead of the
// two-queue IOSleep model, and command-plane ORB submission/retry/status-matching
// is delegated to the composed FetchAgent. #19's two-queue-specific test
// (LoginRetryDelayUsesTimeoutQueueInsteadOfWorkQueue) is dropped — that machinery
// is removed in DICE (SBP2_SESSION_PORT.md §4).

namespace {

constexpr uint16_t kTargetNodeID = 0x05;

using ASFW::Protocols::SBP2::AddressSpaceManager;
using ASFW::Protocols::SBP2::LoginState;
using ASFW::Protocols::SBP2::LoginSession;
using ASFW::Protocols::SBP2::SBP2CommandORB;
using ASFW::Protocols::SBP2::SBP2TargetInfo;
using ASFW::Testing::FakeSessionScheduler;
using ASFW::Protocols::SBP2::Wire::CommandBlockAgentOffsets;
using ASFW::Protocols::SBP2::Wire::LoginORB;
using ASFW::Protocols::SBP2::Wire::LoginResponse;
using ASFW::Protocols::SBP2::Wire::NormalizeBusNodeID;
using ASFW::Protocols::SBP2::Wire::ReconnectORB;
using ASFW::Protocols::SBP2::Wire::StatusBlock;
namespace SBPStatus = ASFW::Protocols::SBP2::Wire::SBPStatus;
// OSSwapHostToBigInt*/OSSwapBigToHostInt* are global byte-order intrinsics.

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

uint32_t DecodeBE32Payload(std::span<const uint8_t> payload) {
    return (static_cast<uint32_t>(payload[0]) << 24) |
           (static_cast<uint32_t>(payload[1]) << 16) |
           (static_cast<uint32_t>(payload[2]) << 8) |
           static_cast<uint32_t>(payload[3]);
}

void ExpectBusyTimeoutWrite(const ASFW::Async::Testing::DeferredFireWireBus::WriteSummary& write,
                            uint16_t expectedNodeID) {
    ASSERT_EQ(4u, write.data.size());
    EXPECT_EQ(0xFFFFu, write.address.addressHi);
    EXPECT_EQ(0xF0000210u, write.address.addressLo);
    EXPECT_EQ(expectedNodeID, write.address.nodeID);
    EXPECT_EQ(static_cast<uint8_t>(expectedNodeID & 0x3Fu), write.nodeId.value);
    EXPECT_EQ(0x0000000Fu, DecodeBE32Payload(write.data));
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
    const uint32_t hi = OSSwapBigToHostInt32(ReadQuadlet(manager, orbAddress + hiOffset));
    const uint32_t lo = OSSwapBigToHostInt32(ReadQuadlet(manager, orbAddress + loOffset));
    return ComposeAddress(static_cast<uint16_t>(hi & 0xFFFFu), lo);
}

class SessionRig {
public:
    SessionRig()
        : sessionOwner(std::make_shared<LoginSession>(bus, bus, addressManager, scheduler))
        , session(*sessionOwner) {
        bus.SetGeneration(ASFW::FW::Generation{1});
        bus.SetLocalNodeID(ASFW::FW::NodeId{0x2A});
        bus.SetDefaultSpeed(ASFW::FW::FwSpeed::S400);

        SBP2TargetInfo info{};
        info.managementAgentOffset = 0x100;
        info.lun = 3;
        info.managementTimeoutMs = 10;
        info.maxORBSize = 32;
        info.maxCommandBlockSize = 12;
        info.targetNodeId = kTargetNodeID;

        session.Configure(info);
    }

    // Bus completions fire inline via CompleteNextWrite; timeouts/retries run on
    // the virtual-clock scheduler. There is no queue to drain.
    void DrainReady() {}

    void AdvanceMs(uint64_t milliseconds) {
        scheduler.Advance(milliseconds * 1'000'000ULL);
    }

    void LoginSuccessfully(uint16_t loginId = 0x0042,
                           uint32_t commandBlockAgentLo = 0x0020'0000,
                           bool drainPendingWrites = true) {
        ASSERT_TRUE(session.Login());
        ASSERT_EQ(1u, bus.PendingWriteCount());

        const auto& loginWrite = bus.WriteAt(0);
        const uint64_t loginOrbAddress = DecodeOrbAddressFromPayload(loginWrite.data);
        const uint64_t loginResponseAddress =
            ReadORBAddress(addressManager, loginOrbAddress,
                           offsetof(LoginORB, loginResponseAddressHi),
                           offsetof(LoginORB, loginResponseAddressLo));
        const uint64_t statusAddress =
            ReadORBAddress(addressManager, loginOrbAddress,
                           offsetof(LoginORB, statusFIFOAddressHi),
                           offsetof(LoginORB, statusFIFOAddressLo));
        sessionStatusAddress = statusAddress;

        LoginResponse response{};
        response.length = OSSwapHostToBigInt16(LoginResponse::kSize);
        response.loginID = OSSwapHostToBigInt16(loginId);
        response.commandBlockAgentAddressHi = OSSwapHostToBigInt32(0x0000'FFFFu);
        response.commandBlockAgentAddressLo = OSSwapHostToBigInt32(commandBlockAgentLo);
        response.reconnectHold = OSSwapHostToBigInt16(1);

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

        if (drainPendingWrites) {
            while (bus.PendingWriteCount() > 0U) {
                ASSERT_TRUE(bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
            }
        }
        ASSERT_EQ(LoginState::LoggedIn, session.State());
    }

    ASFW::Async::Testing::DeferredFireWireBus bus;
    FakeSessionScheduler scheduler;
    AddressSpaceManager addressManager{nullptr};
    std::shared_ptr<LoginSession> sessionOwner;
    LoginSession& session;
    uint64_t sessionStatusAddress{0};
};

TEST(LoginSessionTests, LoginAckCancelsStaleTimeoutBeforeStatusArrives) {
    SessionRig rig;

    ASSERT_TRUE(rig.session.Login());
    ASSERT_EQ(LoginState::LoggingIn, rig.session.State());
    ASSERT_EQ(1u, rig.bus.PendingWriteCount());

    rig.AdvanceMs(5);
    ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));

    rig.AdvanceMs(5);

    EXPECT_EQ(LoginState::LoggingIn, rig.session.State());
    EXPECT_EQ(1u, rig.bus.WriteCount());
}

TEST(LoginSessionTests, LoginORBMatchesAppleWireLayout) {
    SessionRig rig;

    ASSERT_TRUE(rig.session.Login());
    ASSERT_EQ(1u, rig.bus.PendingWriteCount());

    const auto& loginWrite = rig.bus.WriteAt(0);
    const uint64_t loginOrbAddress = DecodeOrbAddressFromPayload(loginWrite.data);

    const uint32_t quadlet4 = OSSwapBigToHostInt32(
        ReadQuadlet(rig.addressManager, loginOrbAddress + offsetof(LoginORB, options)));
    const uint32_t quadlet5 = OSSwapBigToHostInt32(
        ReadQuadlet(rig.addressManager, loginOrbAddress + offsetof(LoginORB, passwordLength)));

    EXPECT_EQ(0x9000u, static_cast<uint16_t>(quadlet4 >> 16));
    EXPECT_EQ(3u, static_cast<uint16_t>(quadlet4 & 0xFFFFu));
    EXPECT_EQ(0u, static_cast<uint16_t>(quadlet5 >> 16));
    EXPECT_EQ(LoginResponse::kSize, static_cast<uint16_t>(quadlet5 & 0xFFFFu));
}

TEST(LoginSessionTests, LoginORBUsesFullBusNodeIdInEmbeddedAddresses) {
    SessionRig rig;

    ASSERT_TRUE(rig.session.Login());
    ASSERT_EQ(1u, rig.bus.PendingWriteCount());

    const auto& loginWrite = rig.bus.WriteAt(0);
    ASSERT_EQ(8u, loginWrite.data.size());

    const uint16_t payloadNode =
        static_cast<uint16_t>((static_cast<uint16_t>(loginWrite.data[0]) << 8) |
                              loginWrite.data[1]);
    const uint64_t loginOrbAddress = DecodeOrbAddressFromPayload(loginWrite.data);
    const uint32_t responseHi = OSSwapBigToHostInt32(
        ReadQuadlet(rig.addressManager, loginOrbAddress + offsetof(LoginORB, loginResponseAddressHi)));
    const uint32_t statusHi = OSSwapBigToHostInt32(
        ReadQuadlet(rig.addressManager, loginOrbAddress + offsetof(LoginORB, statusFIFOAddressHi)));

    const uint16_t expectedNode = NormalizeBusNodeID(0x2A);
    EXPECT_EQ(expectedNode, payloadNode);
    EXPECT_EQ((static_cast<uint32_t>(expectedNode) << 16) | 0xFFFFu, responseHi);
    EXPECT_EQ((static_cast<uint32_t>(expectedNode) << 16) | 0xFFFFu, statusHi);
}

TEST(LoginSessionTests, QueuedUnsolicitedStatusEnableUsesDerivedAddressAfterLogin) {
    SessionRig rig;

    rig.session.EnableUnsolicitedStatus();
    constexpr uint32_t commandBlockAgentLo = 0x0030'0000;
    rig.LoginSuccessfully(0x0042, commandBlockAgentLo);

    ASSERT_GE(rig.bus.WriteCount(), 3u);
    const auto& unsolicitedWrite = rig.bus.WriteAt(1);
    EXPECT_EQ(0xFFFFu, unsolicitedWrite.address.addressHi);
    EXPECT_EQ(commandBlockAgentLo + CommandBlockAgentOffsets::kUnsolicitedStatusEnable,
              unsolicitedWrite.address.addressLo);
}

TEST(LoginSessionTests, LoginWritesBusyTimeoutRegister) {
    SessionRig rig;

    rig.LoginSuccessfully();

    ASSERT_GE(rig.bus.WriteCount(), 2u);
    ExpectBusyTimeoutWrite(rig.bus.WriteAt(1), kTargetNodeID);
}

TEST(LoginSessionTests, ReconnectReplaysBusyTimeoutRegister) {
    SessionRig rig;
    rig.LoginSuccessfully();

    rig.bus.SetGeneration(ASFW::FW::Generation{2});
    rig.session.HandleBusReset(2);
    ASSERT_EQ(LoginState::Suspended, rig.session.State());
    ASSERT_TRUE(rig.session.Reconnect());
    ASSERT_EQ(LoginState::Reconnecting, rig.session.State());

    const size_t reconnectWriteIndex = rig.bus.WriteCount() - 1;
    const uint64_t reconnectOrbAddress =
        DecodeOrbAddressFromPayload(rig.bus.WriteAt(reconnectWriteIndex).data);
    const uint64_t reconnectStatusAddress =
        ReadORBAddress(rig.addressManager, reconnectOrbAddress,
                       offsetof(ReconnectORB, statusFIFOAddressHi),
                       offsetof(ReconnectORB, statusFIFOAddressLo));

    ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    StatusBlock status{};
    status.details = 0;
    status.sbpStatus = SBPStatus::kNoAdditionalInfo;
    rig.addressManager.ApplyRemoteWrite(
        reconnectStatusAddress,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&status), sizeof(status)});

    ASSERT_GE(rig.bus.WriteCount(), reconnectWriteIndex + 2);
    ExpectBusyTimeoutWrite(rig.bus.WriteAt(reconnectWriteIndex + 1), kTargetNodeID);
}

TEST(LoginSessionTests, ReconnectAckStartsStatusTimeoutAndFallsBackToLogin) {
    SessionRig rig;
    rig.LoginSuccessfully();

    rig.bus.SetGeneration(ASFW::FW::Generation{2});
    rig.session.HandleBusReset(2);
    ASSERT_EQ(LoginState::Suspended, rig.session.State());
    ASSERT_TRUE(rig.session.Reconnect());
    ASSERT_EQ(LoginState::Reconnecting, rig.session.State());

    const size_t reconnectWriteIndex = rig.bus.WriteCount() - 1;
    ASSERT_TRUE(rig.bus.CompleteWrite(rig.bus.WriteAt(reconnectWriteIndex).handle,
                                      ASFW::Async::AsyncStatus::kSuccess));

    rig.AdvanceMs(1009);
    EXPECT_EQ(LoginState::Reconnecting, rig.session.State());
    EXPECT_EQ(reconnectWriteIndex + 1, rig.bus.WriteCount());

    rig.AdvanceMs(1);
    EXPECT_EQ(LoginState::LoggingIn, rig.session.State());
    EXPECT_EQ(reconnectWriteIndex + 2, rig.bus.WriteCount());
}

TEST(LoginSessionTests, BusyTimeoutReplayCancelsInFlightWrite) {
    SessionRig rig;

    rig.LoginSuccessfully(0x0042, 0x0020'0000, false);
    const auto firstBusyHandle = rig.bus.WriteAt(1).handle;

    rig.bus.SetGeneration(ASFW::FW::Generation{2});
    rig.session.HandleBusReset(2);
    ASSERT_TRUE(rig.session.Reconnect());

    const size_t reconnectWriteIndex = rig.bus.WriteCount() - 1;
    const uint64_t reconnectOrbAddress =
        DecodeOrbAddressFromPayload(rig.bus.WriteAt(reconnectWriteIndex).data);
    const uint64_t reconnectStatusAddress =
        ReadORBAddress(rig.addressManager, reconnectOrbAddress,
                       offsetof(ReconnectORB, statusFIFOAddressHi),
                       offsetof(ReconnectORB, statusFIFOAddressLo));

    ASSERT_TRUE(rig.bus.CompleteWrite(rig.bus.WriteAt(reconnectWriteIndex).handle,
                                      ASFW::Async::AsyncStatus::kSuccess));
    StatusBlock status{};
    status.details = 0;
    status.sbpStatus = SBPStatus::kNoAdditionalInfo;
    rig.addressManager.ApplyRemoteWrite(
        reconnectStatusAddress,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&status), sizeof(status)});

    EXPECT_FALSE(rig.bus.CompleteWrite(firstBusyHandle, ASFW::Async::AsyncStatus::kSuccess));
    ASSERT_GE(rig.bus.WriteCount(), reconnectWriteIndex + 2);
    ExpectBusyTimeoutWrite(rig.bus.WriteAt(reconnectWriteIndex + 1), kTargetNodeID);
}

TEST(LoginSessionTests, BusResetWhileLoggingInRetriesLoginAfterDelay) {
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

TEST(LoginSessionTests, BusResetWhileLoggingInDefersRetryOnScheduler) {
    SessionRig rig;

    ASSERT_TRUE(rig.session.Login());
    rig.bus.SetGeneration(ASFW::FW::Generation{2});
    rig.session.HandleBusReset(2);

    // The retry is deferred on the scheduler, not run inline.
    EXPECT_GT(rig.scheduler.PendingCount(), 0u);
    EXPECT_EQ(LoginState::Idle, rig.session.State());
    EXPECT_EQ(1u, rig.bus.WriteCount());
}

TEST(LoginSessionTests, BusResetWhileReconnectingRetriesReconnectAfterDelay) {
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

TEST(LoginSessionTests, ImmediateORBRetryStaysBoundToOriginalORBAndQueuesNextImmediate) {
    SessionRig rig;
    rig.LoginSuccessfully();

    SBP2CommandORB first(rig.addressManager, &rig.session, 16);
    first.SetFlags(SBP2CommandORB::kNormalORB);
    first.SetTimeout(50);

    SBP2CommandORB second(rig.addressManager, &rig.session, 16);
    second.SetFlags(SBP2CommandORB::kNormalORB);

    ASSERT_TRUE(rig.session.SubmitORB(&first));
    ASSERT_EQ(1u, rig.bus.PendingWriteCount());
    const size_t firstFetchWriteIndex = rig.bus.WriteCount() - 1;
    const uint32_t firstAddressLo = static_cast<uint32_t>(
        DecodeOrbAddressFromPayload(rig.bus.WriteAt(firstFetchWriteIndex).data));

    ASSERT_TRUE(rig.session.SubmitORB(&second));
    EXPECT_EQ(3u, rig.bus.WriteCount());

    ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kTimeout));

    rig.AdvanceMs(999);
    EXPECT_EQ(3u, rig.bus.WriteCount());

    rig.AdvanceMs(1);
    ASSERT_EQ(4u, rig.bus.WriteCount());
    const uint32_t retryAddressLo = static_cast<uint32_t>(
        DecodeOrbAddressFromPayload(rig.bus.WriteAt(3).data));
    EXPECT_EQ(firstAddressLo, retryAddressLo);
}

TEST(LoginSessionTests, SubmittedImmediateORBStartsTimeoutAfterFetchAgentWriteSucceeds) {
    SessionRig rig;
    rig.LoginSuccessfully();

    SBP2CommandORB orb(rig.addressManager, &rig.session, 16);
    orb.SetFlags(SBP2CommandORB::kNormalORB);
    orb.SetTimeout(25);

    int callbackStatus = 99;
    int callbackCount = 0;
    orb.SetCompletionCallback([&](int status, uint8_t) {
        ++callbackCount;
        callbackStatus = status;
    });

    const size_t pendingTimersBeforeSubmit = rig.scheduler.PendingCount();

    ASSERT_TRUE(rig.session.SubmitORB(&orb));
    EXPECT_EQ(pendingTimersBeforeSubmit, rig.scheduler.PendingCount());

    ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    EXPECT_EQ(pendingTimersBeforeSubmit + 1U, rig.scheduler.PendingCount());

    rig.AdvanceMs(24);
    EXPECT_EQ(0, callbackCount);

    rig.AdvanceMs(1);
    EXPECT_EQ(1, callbackCount);
    EXPECT_EQ(-1, callbackStatus);
}

TEST(LoginSessionTests, SolicitedStatusCompletesORBMatchingByORBAddress) {
    SessionRig rig;
    rig.LoginSuccessfully();

    ASSERT_NE(0u, rig.session.CommandBlockAgent().addressLo);

    SBP2CommandORB first(rig.addressManager, &rig.session, 16);
    first.SetFlags(0);
    int firstStatus = 99;
    first.SetCompletionCallback([&firstStatus](int status, uint8_t) { firstStatus = status; });

    SBP2CommandORB second(rig.addressManager, &rig.session, 16);
    second.SetFlags(0);
    int secondStatus = 99;
    second.SetCompletionCallback([&secondStatus](int status, uint8_t) { secondStatus = status; });

    ASSERT_TRUE(rig.session.SubmitORB(&first));
    ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));

    ASSERT_TRUE(rig.session.SubmitORB(&second));
    ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));

    StatusBlock status{};
    const auto firstAddress = first.GetORBAddress();
    status.details = 0;
    status.sbpStatus = SBPStatus::kNoAdditionalInfo;
    status.orbOffsetHi = OSSwapHostToBigInt16(firstAddress.addressHi);
    status.orbOffsetLo = OSSwapHostToBigInt32(firstAddress.addressLo);

    rig.addressManager.ApplyRemoteWrite(
        rig.sessionStatusAddress,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&status), sizeof(status)});

    EXPECT_EQ(0, firstStatus);
    EXPECT_EQ(99, secondStatus);
}

TEST(LoginSessionTests, ImmediateORBWriteRetryExhaustionFailsActiveCommandAndResetsFetchAgent) {
    SessionRig rig;
    rig.LoginSuccessfully();

    SBP2CommandORB orb(rig.addressManager, &rig.session, 16);
    orb.SetFlags(SBP2CommandORB::kNormalORB);

    int callbackStatus = 99;
    int callbackCount = 0;
    orb.SetCompletionCallback([&](int status, uint8_t) {
        ++callbackCount;
        callbackStatus = status;
    });

    const auto commandBlock = rig.session.CommandBlockAgent();
    const size_t writesBeforeSubmit = rig.bus.WriteCount();
    ASSERT_TRUE(rig.session.SubmitORB(&orb));
    orb.SetFetchAgentWriteRetries(0);
    ASSERT_EQ(writesBeforeSubmit + 1, rig.bus.WriteCount());

    ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kTimeout));

    ASSERT_EQ(1, callbackCount);
    EXPECT_EQ(-1, callbackStatus);
    EXPECT_FALSE(orb.IsAppended());
    EXPECT_GE(rig.bus.WriteCount(), writesBeforeSubmit + 2);
    EXPECT_EQ(commandBlock.addressLo + CommandBlockAgentOffsets::kAgentReset,
              rig.bus.WriteAt(rig.bus.WriteCount() - 1).address.addressLo);

    EXPECT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
}

TEST(LoginSessionTests,
     ImmediateORBWriteRetryExhaustionClearsPendingImmediateQueueAndFailsActiveCommandOnce) {
    SessionRig rig;
    rig.LoginSuccessfully();

    SBP2CommandORB first(rig.addressManager, &rig.session, 16);
    first.SetFlags(SBP2CommandORB::kNormalORB);
    int firstCallbackStatus = 99;
    int firstCallbackCount = 0;
    first.SetCompletionCallback([&](int status, uint8_t) {
        ++firstCallbackCount;
        firstCallbackStatus = status;
    });

    SBP2CommandORB second(rig.addressManager, &rig.session, 16);
    second.SetFlags(SBP2CommandORB::kNormalORB);
    int secondCallbackStatus = 99;
    int secondCallbackCount = 0;
    second.SetCompletionCallback([&](int status, uint8_t) {
        ++secondCallbackCount;
        secondCallbackStatus = status;
    });

    const auto commandBlock = rig.session.CommandBlockAgent();
    const size_t writesBeforeSubmit = rig.bus.WriteCount();

    ASSERT_TRUE(rig.session.SubmitORB(&first));
    ASSERT_TRUE(rig.session.SubmitORB(&second));
    first.SetFetchAgentWriteRetries(0);
    EXPECT_EQ(writesBeforeSubmit + 1, rig.bus.WriteCount());

    ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kTimeout));

    EXPECT_EQ(1, firstCallbackCount);
    EXPECT_EQ(-1, firstCallbackStatus);
    EXPECT_EQ(1, secondCallbackCount);
    EXPECT_EQ(-1, secondCallbackStatus);
    EXPECT_FALSE(first.IsAppended());
    EXPECT_FALSE(second.IsAppended());
    EXPECT_GE(rig.bus.WriteCount(), writesBeforeSubmit + 2);
    EXPECT_EQ(commandBlock.addressLo + CommandBlockAgentOffsets::kAgentReset,
              rig.bus.WriteAt(rig.bus.WriteCount() - 1).address.addressLo);
    EXPECT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
}

} // namespace
