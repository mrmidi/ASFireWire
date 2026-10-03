#include <gtest/gtest.h>

#include "ASFWDriver/Discovery/DeviceRegistry.hpp"
#include "ASFWDriver/Discovery/FWDevice.hpp"
#include "ASFWDriver/Protocols/AVC/FCPTransport.hpp"
#include "ASFWDriver/Protocols/AVC/Core/AvcFrame.hpp"
#include "DeferredFireWireBus.hpp"
#include "FakeSessionScheduler.hpp"

namespace {

using ASFW::Async::AsyncStatus;
using ASFW::Async::Testing::DeferredFireWireBus;
using ASFW::Discovery::ConfigROM;
using ASFW::Discovery::DeviceRecord;
using ASFW::Discovery::DeviceRegistry;
using ASFW::Discovery::FWDevice;
using ASFW::FW::Generation;
using ASFW::AVC::AvcErrorKind;
using ASFW::AVC::CommandType;
using ASFW::Protocols::AVC::FCPTransport;
using ASFW::Protocols::AVC::FCPTransportConfig;
using ASFW::Testing::FakeSessionScheduler;

constexpr uint64_t kMillisecondNs = 1'000'000ULL;
constexpr uint64_t kGuid = 0x0001020304050607ULL;

using Reply = ASFW::AVC::Expected<ASFW::AVC::Response>;

// UNIT INFO with no operands; CONTROL unless asked (CONTROL is never replayed).
ASFW::AVC::CommandFrame UnitInfo(CommandType type = CommandType::kControl) {
    return *ASFW::AVC::CommandFrame::Make(type, ASFW::AVC::SubunitAddress::Unit(),
                                          ASFW::AVC::Opcode::kUnitInfo, {});
}

// nullopt for a response, else the error the engine reported.
std::optional<AvcErrorKind> KindOf(const Reply& reply) {
    return reply ? std::nullopt : std::optional<AvcErrorKind>{reply.error().kind};
}

std::array<uint8_t, 3> MakeAcceptedUnitInfoResponse() {
    return {0x09, 0xFF, 0x30};  // ACCEPTED, unit, UNIT_INFO
}

class FCPTransportTests : public ::testing::Test {
protected:
    [[nodiscard]] static ConfigROM MakeROM(Generation generation, uint16_t nodeId) {
        ConfigROM rom{};
        rom.bib.guid = kGuid;
        rom.gen = generation;
        rom.nodeId = nodeId;
        return rom;
    }

    void RebindRoute(Generation generation, uint16_t nodeId) {
        routes_.InvalidateLiveMappingsForBusReset();
        (void)routes_.UpsertFromROM(MakeROM(generation, nodeId), {});
    }

    void SetUp() override {
        DeviceRecord record{};
        record.guid = kGuid;
        record.nodeId = 2;
        record.gen = Generation{1};
        device_ = FWDevice::Create(record, ConfigROM{});
        ASSERT_NE(device_, nullptr);
        (void)routes_.UpsertFromROM(MakeROM(record.gen, record.nodeId), {});

        config_.timeoutMs = 10;
        config_.interimTimeoutMs = 25;
        config_.maxRetries = 0;
        transport_ = std::make_shared<FCPTransport>();
        ASSERT_TRUE(transport_->init(&bus_, &bus_, device_.get(), routes_, scheduler_, config_));
    }

    void Send(std::function<void(Reply)> completion, CommandType type = CommandType::kControl) {
        transport_->Submit(UnitInfo(type), Generation{1}, std::move(completion));
    }

    void Reinit() {
        transport_->Shutdown();
        transport_ = std::make_shared<FCPTransport>();
        ASSERT_TRUE(transport_->init(&bus_, &bus_, device_.get(), routes_, scheduler_, config_));
    }

    void TearDown() override {
        if (transport_) {
            transport_->Shutdown();
        }
    }

    // Completion counter for tests that deliberately leave a command outstanding.
    //
    // TearDown() calls Shutdown(), which by design completes every pending and queued
    // command with kTransportError rather than leaking them. For a test whose command
    // never completes (route invalidated on purpose), that callback therefore runs
    // *after* TestBody() has returned. A `[&local]` capture would then write into a dead
    // stack frame -- stack-use-after-return, which crashes in release-shaped builds and
    // is reported by ASan. Owning the counter here keeps it alive through TearDown.
    int outstandingCompletionCount_{0};

    DeferredFireWireBus bus_;
    FakeSessionScheduler scheduler_;
    DeviceRegistry routes_;
    std::shared_ptr<FWDevice> device_;
    std::shared_ptr<FCPTransport> transport_;
    FCPTransportConfig config_{};
};

TEST_F(FCPTransportTests, AcceptsResponseBeforeCommandWriteCompletion) {
    int completionCount = 0;
    std::optional<AvcErrorKind> kind{AvcErrorKind::kTransportError};
    Send([&](Reply reply) { ++completionCount; kind = KindOf(reply); });
    ASSERT_EQ(bus_.PendingWriteCount(), 1U);

    transport_->OnFCPResponse(2, 1, MakeAcceptedUnitInfoResponse());
    scheduler_.Advance(0);
    EXPECT_EQ(completionCount, 1);
    EXPECT_EQ(kind, std::nullopt);

    // AR Request is drained before AR Response. Once the target's FCP write
    // proves delivery, the queued local write acknowledgement is stale.
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    EXPECT_EQ(completionCount, 1);
    EXPECT_EQ(kind, std::nullopt);
}

TEST_F(FCPTransportTests, DeliversTheResponseOnlyAfterTheReceiveHandlerReturns) {
    int completionCount = 0;
    Send([&](Reply) { ++completionCount; });
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));

    // Our write response to the target's response goes out when the receive
    // handler returns; the command completes (and may submit the next one)
    // only after that (AV/C General 4.2 §6.5).
    transport_->OnFCPResponse(2, 1, MakeAcceptedUnitInfoResponse());
    EXPECT_EQ(completionCount, 0);
    EXPECT_EQ(scheduler_.PendingCount(), 1U) << "only the deferred delivery; the deadline is disarmed";
    scheduler_.Advance(0);
    EXPECT_EQ(completionCount, 1);
}

TEST_F(FCPTransportTests, AnsweredCommandIgnoresResetsTimeoutsAndDuplicates) {
    int completionCount = 0;
    std::optional<AvcErrorKind> kind{AvcErrorKind::kTransportError};
    Send([&](Reply reply) { ++completionCount; kind = KindOf(reply); });
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    transport_->OnFCPResponse(2, 1, MakeAcceptedUnitInfoResponse());

    // Between acceptance and delivery: a duplicate response, a reset and the
    // old deadline change nothing.
    transport_->OnFCPResponse(2, 1, MakeAcceptedUnitInfoResponse());
    transport_->OnBusReset(2);
    scheduler_.Advance(config_.timeoutMs * 2ULL * kMillisecondNs);
    EXPECT_EQ(completionCount, 1);
    EXPECT_EQ(kind, std::nullopt);
}

TEST_F(FCPTransportTests, IgnoresResponseFromDifferentGeneration) {
    int completionCount = 0;
    Send([&](Reply) { ++completionCount; });
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));

    const auto response = MakeAcceptedUnitInfoResponse();
    transport_->OnFCPResponse(2, 2, response);
    scheduler_.Advance(0);
    EXPECT_EQ(completionCount, 0);

    transport_->OnFCPResponse(2, 1, response);
    scheduler_.Advance(0);
    EXPECT_EQ(completionCount, 1);
}

TEST_F(FCPTransportTests, IgnoresResponseWithAnotherAddressOrOpcode) {
    int completionCount = 0;
    Send([&](Reply) { ++completionCount; });
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));

    constexpr std::array<uint8_t, 3> otherAddress{0x09, 0x60, 0x30};
    constexpr std::array<uint8_t, 3> otherOpcode{0x09, 0xFF, 0x31};
    transport_->OnFCPResponse(2, 1, otherAddress);
    transport_->OnFCPResponse(2, 1, otherOpcode);
    scheduler_.Advance(0);
    EXPECT_EQ(completionCount, 0);

    transport_->OnFCPResponse(2, 1, MakeAcceptedUnitInfoResponse());
    scheduler_.Advance(0);
    EXPECT_EQ(completionCount, 1);
}

TEST_F(FCPTransportTests, TapeTransportStateAcceptsAModeOpcodeAsItsResponse) {
    // Apple IOFireWireAVCCommand.cpp:122-125: a tape subunit answers TRANSPORT STATE with the current
    // mode as the opcode (Play, Wind, Record or LoadMedium, i.e. C1..C4), or with D0 itself.
    const auto tapeAddress = ASFW::AVC::SubunitAddress::Of(ASFW::AVC::SubunitType::kTape, 0);
    // The literal on purpose: it is the wire value (TRANSPORT STATE), independent of the constant under test.
    const auto transportState = static_cast<ASFW::AVC::Opcode>(0xD0);
    for (const uint8_t opcode : {uint8_t{0xD0}, uint8_t{0xC1}, uint8_t{0xC2}, uint8_t{0xC3}, uint8_t{0xC4}}) {
        int completionCount = 0;
        transport_->Submit(*ASFW::AVC::CommandFrame::Make(CommandType::kControl, tapeAddress, transportState, {}),
                           Generation{1}, [&](Reply) { ++completionCount; });
        ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
        const std::array<uint8_t, 3> response{0x09, 0x20, opcode};
        transport_->OnFCPResponse(2, 1, response);
        scheduler_.Advance(0);
        EXPECT_EQ(completionCount, 1) << "opcode " << int{opcode};
    }
}

TEST_F(FCPTransportTests, TapeTransportStateIgnoresOpcodesOutsideTheModeRange) {
    const auto tapeAddress = ASFW::AVC::SubunitAddress::Of(ASFW::AVC::SubunitType::kTape, 0);
    // The literal on purpose: it is the wire value (TRANSPORT STATE), independent of the constant under test.
    const auto transportState = static_cast<ASFW::AVC::Opcode>(0xD0);
    int completionCount = 0;
    transport_->Submit(*ASFW::AVC::CommandFrame::Make(CommandType::kControl, tapeAddress, transportState, {}),
                       Generation{1}, [&](Reply) { ++completionCount; });
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    for (const uint8_t opcode : {uint8_t{0xC0}, uint8_t{0xC5}, uint8_t{0xD1}, uint8_t{0x30}}) {
        const std::array<uint8_t, 3> response{0x09, 0x20, opcode};
        transport_->OnFCPResponse(2, 1, response);
        scheduler_.Advance(0);
        EXPECT_EQ(completionCount, 0) << "opcode " << int{opcode};
    }
    const std::array<uint8_t, 3> answer{0x09, 0x20, 0xD0};
    transport_->OnFCPResponse(2, 1, answer);
    scheduler_.Advance(0);
    EXPECT_EQ(completionCount, 1);
}

TEST_F(FCPTransportTests, ResponseOpcodeDifferingOnlyInBit7IsAcceptedForNow) {
    // CHARACTERIZATION of legacy behaviour (audit finding F6): the opcode is compared without bit 7.
    // Apple compares all eight bits (IOFireWireAVCCommand.cpp:154-157). When F6 is decided and the
    // comparison tightened, this test flips to "ignored".
    int completionCount = 0;
    Send([&](Reply) { ++completionCount; });
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    const std::array<uint8_t, 3> bit7Set{0x09, 0xFF, 0xB0};  // UNIT INFO (30) with bit 7 set
    transport_->OnFCPResponse(2, 1, bit7Set);
    scheduler_.Advance(0);
    EXPECT_EQ(completionCount, 1);
}

TEST_F(FCPTransportTests, RejectsResponseForInvalidatedRouteAfterRebind) {
    // This command never completes (the route is invalidated below), so its completion
    // is fired by Shutdown() during TearDown -- see outstandingCompletionCount_.
    int& completionCount = outstandingCompletionCount_;
    Send([&completionCount](Reply) { ++completionCount; });
    ASSERT_EQ(bus_.WriteCount(), 1U);
    EXPECT_EQ(bus_.WriteAt(0).nodeId.value, 2U);
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));

    // A rebind invalidates the old token. No callback from the prior route may
    // complete the logical operation, even if its node/generation are retained.
    RebindRoute(Generation{2}, 3);

    const auto response = MakeAcceptedUnitInfoResponse();
    transport_->OnFCPResponse(3, 1, response);
    scheduler_.Advance(0);
    EXPECT_EQ(completionCount, 0);

    transport_->OnFCPResponse(2, 1, response);
    scheduler_.Advance(0);
    EXPECT_EQ(completionCount, 0);
}

TEST_F(FCPTransportTests, RejectsWriteCompletionFromInvalidatedRoute) {
    // As above: the rebind strands this command, so Shutdown() completes it at TearDown.
    int& completionCount = outstandingCompletionCount_;
    Send([&completionCount](Reply) { ++completionCount; });
    ASSERT_EQ(bus_.WriteCount(), 1U);
    EXPECT_EQ(bus_.WriteAt(0).nodeId.value, 2U);
    EXPECT_EQ(bus_.WriteAt(0).generation.value, 1U);

    // The write was issued against the old token. A rebind before completion
    // makes both that completion and its later response stale.
    RebindRoute(Generation{2}, 3);

    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    const auto response = MakeAcceptedUnitInfoResponse();
    transport_->OnFCPResponse(3, 2, response);
    scheduler_.Advance(0);
    EXPECT_EQ(completionCount, 0);

    transport_->OnFCPResponse(2, 1, response);
    scheduler_.Advance(0);
    EXPECT_EQ(completionCount, 0);
}

TEST_F(FCPTransportTests, StartsTimeoutOnlyAfterCommandWriteCompletes) {
    int completionCount = 0;
    std::optional<AvcErrorKind> kind;
    Send([&](Reply reply) { ++completionCount; kind = KindOf(reply); });

    scheduler_.Advance(config_.timeoutMs * 2ULL * kMillisecondNs);
    EXPECT_EQ(completionCount, 0);
    EXPECT_EQ(scheduler_.PendingCount(), 0U);

    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    ASSERT_EQ(scheduler_.PendingCount(), 1U);
    scheduler_.Advance(config_.timeoutMs * kMillisecondNs - 1);
    EXPECT_EQ(completionCount, 0);

    scheduler_.Advance(1);
    EXPECT_EQ(completionCount, 1);
    EXPECT_EQ(kind, AvcErrorKind::kTimeout);
}

TEST_F(FCPTransportTests, WriteFailureCompletesWithoutArmingResponseTimeout) {
    std::optional<AvcErrorKind> kind;
    Send([&](Reply reply) { kind = KindOf(reply); });
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kTimeout));

    EXPECT_EQ(kind, AvcErrorKind::kTransportError);
    EXPECT_EQ(scheduler_.PendingCount(), 0U);
}

TEST_F(FCPTransportTests, SynchronousWriteAdmissionFailureCompletesExactlyOnce) {
    bus_.FailNextWriteBlock();
    int completionCount = 0;
    std::optional<AvcErrorKind> kind;
    Send([&](Reply reply) { ++completionCount; kind = KindOf(reply); });

    EXPECT_EQ(completionCount, 1);
    EXPECT_EQ(kind, AvcErrorKind::kTransportError);
    EXPECT_EQ(scheduler_.PendingCount(), 0U);
}

TEST_F(FCPTransportTests, InterimResponseExtendsDeadlineWithoutCompletingCommand) {
    int completionCount = 0;
    Send([&](Reply) { ++completionCount; });
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    ASSERT_EQ(scheduler_.PendingCount(), 1U);

    constexpr std::array<uint8_t, 3> interim{0x0F, 0xFF, 0x30};
    transport_->OnFCPResponse(2, 1, interim);
    scheduler_.Advance(0);
    EXPECT_EQ(completionCount, 0);
    EXPECT_EQ(scheduler_.PendingCount(), 1U);

    scheduler_.Advance(config_.timeoutMs * kMillisecondNs);
    EXPECT_EQ(completionCount, 0);
    transport_->OnFCPResponse(2, 1, MakeAcceptedUnitInfoResponse());
    scheduler_.Advance(0);
    EXPECT_EQ(completionCount, 1);
}

TEST_F(FCPTransportTests, ResetRetryWaitsForRevalidatedRouteBeforeResubmission) {
    config_.allowBusResetRetry = true;
    config_.maxRetries = 1;
    Reinit();

    int completionCount = 0;
    Send([&](Reply) { ++completionCount; }, CommandType::kStatus);
    ASSERT_EQ(bus_.PendingWriteCount(), 1U);

    bus_.SetGeneration(Generation{2});
    routes_.InvalidateLiveMappingsForBusReset();
    transport_->OnBusReset(2);

    EXPECT_EQ(bus_.WriteCount(), 1U);
    EXPECT_EQ(bus_.PendingWriteCount(), 0U);
    EXPECT_EQ(completionCount, 0);

    // A reset makes the prior route invalid. Rebinding the GUID to node 3 in
    // generation 2 is the only event allowed to replay this idempotent query.
    (void)routes_.UpsertFromROM(MakeROM(Generation{2}, 3), {});
    const auto route = routes_.CurrentRoute(kGuid);
    ASSERT_TRUE(route.has_value());
    transport_->OnRouteRevalidated(*route);

    EXPECT_EQ(bus_.WriteCount(), 2U);
    EXPECT_EQ(bus_.WriteAt(1).nodeId.value, 3U);
    EXPECT_EQ(bus_.WriteAt(1).generation.value, 2U);
    ASSERT_EQ(bus_.PendingWriteCount(), 1U);
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    transport_->OnFCPResponse(3, 2, MakeAcceptedUnitInfoResponse());
    scheduler_.Advance(0);
    EXPECT_EQ(completionCount, 1);
}

TEST_F(FCPTransportTests, QueuedAvcSubmissionKeepsItsRequestedGeneration) {
    Send([](Reply) {});
    ASSERT_EQ(bus_.WriteCount(), 1U);

    std::optional<Reply> result;
    Send([&result](Reply response) { result = std::move(response); });

    // Discovery has moved the route to generation 2 before the queued frame
    // becomes active. Only its explicit requested generation may decide if it
    // can be written.
    RebindRoute(Generation{2}, 3);
    transport_->OnBusReset(2);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(KindOf(*result), AvcErrorKind::kBusReset);
    EXPECT_EQ(bus_.WriteCount(), 1U) << "the stale queued command never reaches the rebound node";
}

TEST_F(FCPTransportTests, DirectStaleAvcSubmissionCompletesWithoutWriting) {
    RebindRoute(Generation{2}, 3);
    size_t completionCount = 0;
    std::optional<AvcErrorKind> kind;
    Send([&](Reply reply) { ++completionCount; kind = KindOf(reply); });

    EXPECT_EQ(completionCount, 1U);
    EXPECT_EQ(kind, AvcErrorKind::kBusReset);
    EXPECT_EQ(bus_.WriteCount(), 0U);
}

TEST_F(FCPTransportTests, ExplicitIdempotentResetRetryMayBindToRevalidatedGeneration) {
    config_.allowBusResetRetry = true;
    config_.maxRetries = 1;
    Reinit();

    int& completionCount = outstandingCompletionCount_;
    Send([&completionCount](Reply) { ++completionCount; }, CommandType::kStatus);
    ASSERT_EQ(bus_.WriteCount(), 1U);

    bus_.SetGeneration(Generation{2});
    routes_.InvalidateLiveMappingsForBusReset();
    transport_->OnBusReset(2);
    EXPECT_EQ(completionCount, 0);
    EXPECT_EQ(bus_.WriteCount(), 1U);

    (void)routes_.UpsertFromROM(MakeROM(Generation{2}, 3), {});
    const auto route = routes_.CurrentRoute(kGuid);
    ASSERT_TRUE(route.has_value());
    transport_->OnRouteRevalidated(*route);

    EXPECT_EQ(bus_.WriteCount(), 2U);
    EXPECT_EQ(bus_.WriteAt(1).nodeId.value, 3U);
    EXPECT_EQ(bus_.WriteAt(1).generation.value, 2U);
}

TEST_F(FCPTransportTests, ShutdownCompletesPendingAndQueuedCommandsExactlyOnce) {
    std::vector<std::optional<AvcErrorKind>> completions;
    Send([&completions](Reply reply) { completions.push_back(KindOf(reply)); });
    Send([&completions](Reply reply) { completions.push_back(KindOf(reply)); });

    transport_->Shutdown();
    ASSERT_EQ(completions.size(), 2U);
    EXPECT_EQ(completions[0], AvcErrorKind::kTransportError);
    EXPECT_EQ(completions[1], AvcErrorKind::kTransportError);
    EXPECT_EQ(bus_.PendingWriteCount(), 0U);

    transport_->OnFCPResponse(2, 1, MakeAcceptedUnitInfoResponse());
    scheduler_.Advance(0);
    EXPECT_EQ(completions.size(), 2U);
}

TEST_F(FCPTransportTests, NonIdempotentCommandCompletesOnBusResetWithoutReplay) {
    std::optional<AvcErrorKind> kind;
    Send([&](Reply reply) { kind = KindOf(reply); });
    ASSERT_EQ(bus_.WriteCount(), 1U);

    bus_.SetGeneration(Generation{2});
    transport_->OnBusReset(2);

    EXPECT_EQ(kind, AvcErrorKind::kBusReset);
    EXPECT_EQ(bus_.WriteCount(), 1U);
    EXPECT_EQ(bus_.PendingWriteCount(), 0U);
}

TEST_F(FCPTransportTests, ControlCommandDoesNotRetryAfterTimeout) {
    config_.maxRetries = 1;
    Reinit();

    std::optional<AvcErrorKind> kind;
    Send([&](Reply reply) { kind = KindOf(reply); }, CommandType::kControl);
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    scheduler_.Advance(config_.timeoutMs * kMillisecondNs);

    EXPECT_EQ(kind, AvcErrorKind::kTimeout);
    EXPECT_EQ(bus_.WriteCount(), 1U);
}

TEST_F(FCPTransportTests, StatusCommandRetriesAfterTimeout) {
    config_.maxRetries = 1;
    Reinit();

    std::optional<AvcErrorKind> kind{AvcErrorKind::kTransportError};
    Send([&](Reply reply) { kind = KindOf(reply); }, CommandType::kStatus);
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    scheduler_.Advance(config_.timeoutMs * kMillisecondNs);
    ASSERT_EQ(bus_.WriteCount(), 2U);

    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    transport_->OnFCPResponse(2, 1, MakeAcceptedUnitInfoResponse());
    scheduler_.Advance(0);
    EXPECT_EQ(kind, std::nullopt);
}

TEST_F(FCPTransportTests, StatusCommandRetriesAfterWriteFailure) {
    config_.maxRetries = 1;
    Reinit();

    std::optional<AvcErrorKind> kind{AvcErrorKind::kTransportError};
    Send([&](Reply reply) { kind = KindOf(reply); }, CommandType::kStatus);
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kTimeout));
    ASSERT_EQ(bus_.WriteCount(), 2U) << "a failed STATUS write is replayed";
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kTimeout));
    EXPECT_EQ(kind, AvcErrorKind::kTransportError) << "and fails once its replays are spent";
}

TEST_F(FCPTransportTests, QueuesCommandsFifo) {
    std::vector<uint32_t> completions;
    Send([&completions](Reply) { completions.push_back(1); });
    Send([&completions](Reply reply) { completions.push_back(reply ? 2U : 20U); });
    EXPECT_EQ(bus_.WriteCount(), 1U);

    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    transport_->OnFCPResponse(2, 1, MakeAcceptedUnitInfoResponse());
    scheduler_.Advance(0);
    EXPECT_EQ(completions, std::vector<uint32_t>{1});
    EXPECT_EQ(bus_.WriteCount(), 2U);

    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    transport_->OnFCPResponse(2, 1, MakeAcceptedUnitInfoResponse());
    scheduler_.Advance(0);
    EXPECT_EQ(completions, (std::vector<uint32_t>{1, 2}));
    EXPECT_EQ(bus_.WriteCount(), 2U);
}

TEST_F(FCPTransportTests, AllowlistRefusesWithoutWriting) {
    static constexpr ASFW::Protocols::AVC::FCPPermittedFrame kOnlyPlugInfo[] = {
        {.prefix = {0x01, 0xFF, 0x02, 0x00}, .care = {0xFF, 0xFF, 0xFF, 0xFF}, .length = 4, .name = "PLUG INFO"},
    };
    config_.permittedFrames = kOnlyPlugInfo;
    Reinit();

    std::optional<AvcErrorKind> kind;
    Send([&](Reply reply) { kind = KindOf(reply); });
    EXPECT_EQ(kind, AvcErrorKind::kRefused);
    EXPECT_EQ(bus_.WriteCount(), 0U);
}

// --- Exchange log: what the AV/C Report exports --------------------------------

using ASFW::Protocols::AVC::FcpExchangeOutcome;
using ASFW::Protocols::AVC::FcpExchangeRecorder;

TEST_F(FCPTransportTests, ExchangeLogKeepsCommandResponseAndInterim) {
    Send([](Reply) {});
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    constexpr std::array<uint8_t, 3> interim{0x0F, 0xFF, 0x30};
    transport_->OnFCPResponse(2, 1, interim);
    scheduler_.Advance(0);
    transport_->OnFCPResponse(2, 1, MakeAcceptedUnitInfoResponse());
    scheduler_.Advance(0);

    const auto log = transport_->CopyExchangeLog();
    EXPECT_EQ(log.session, 1U);
    EXPECT_EQ(log.dropped, 0U);
    ASSERT_EQ(log.records.size(), 1U);
    const auto& record = log.records[0];
    EXPECT_EQ(record.outcome, FcpExchangeOutcome::kResponse);
    EXPECT_TRUE(record.interim);
    EXPECT_EQ(record.generation, 1U);
    // What went on the wire: the frame padded to a quadlet.
    EXPECT_EQ(record.command, (std::vector<uint8_t>{0x00, 0xFF, 0x30, 0x00}));
    EXPECT_EQ(record.response, (std::vector<uint8_t>{0x09, 0xFF, 0x30}));
}

TEST_F(FCPTransportTests, ExchangeLogKeepsTimeoutsAndRetries) {
    config_.maxRetries = 1;
    Reinit();
    Send([](Reply) {}, CommandType::kStatus);
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    scheduler_.Advance(config_.timeoutMs * kMillisecondNs);
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    scheduler_.Advance(config_.timeoutMs * kMillisecondNs);

    const auto log = transport_->CopyExchangeLog();
    ASSERT_EQ(log.records.size(), 1U);
    EXPECT_EQ(log.records[0].outcome, FcpExchangeOutcome::kTimeout);
    EXPECT_EQ(log.records[0].retries, 1U);
    EXPECT_TRUE(log.records[0].response.empty());
}

TEST_F(FCPTransportTests, ExchangeLogKeepsCommandsThatNeverReachTheBus) {
    // Built for generation 9; the route is at generation 1.
    const auto frame = UnitInfo(CommandType::kStatus);
    transport_->Submit(frame, Generation{9}, [](Reply) {});
    EXPECT_EQ(bus_.WriteCount(), 0U);

    const auto log = transport_->CopyExchangeLog();
    ASSERT_EQ(log.records.size(), 1U);
    EXPECT_EQ(log.records[0].outcome, FcpExchangeOutcome::kBusReset);
    const auto wire = frame.WireBytes();
    EXPECT_EQ(log.records[0].command, (std::vector<uint8_t>(wire.begin(), wire.end())));
}

TEST_F(FCPTransportTests, NewExchangeSessionStartsAnEmptyLog) {
    Send([](Reply) {});
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    transport_->OnFCPResponse(2, 1, MakeAcceptedUnitInfoResponse());
    scheduler_.Advance(0);
    ASSERT_EQ(transport_->CopyExchangeLog().records.size(), 1U);

    transport_->BeginExchangeSession();
    const auto log = transport_->CopyExchangeLog();
    EXPECT_EQ(log.session, 2U);
    EXPECT_TRUE(log.records.empty());
}

TEST(FcpExchangeRecorderTests, FullSessionKeepsTheFirstExchangesAndCountsDrops) {
    FcpExchangeRecorder recorder(/*maxRecords=*/2, /*maxBytes=*/64);
    recorder.BeginSession();
    constexpr std::array<uint8_t, 3> command{0x01, 0xFF, 0x30};
    for (uint32_t i = 0; i < 4; ++i) {
        recorder.Record(i, FcpExchangeOutcome::kTimeout, false, 0, command, {});
    }
    EXPECT_EQ(recorder.Log().records.size(), 2U);
    EXPECT_EQ(recorder.Log().records[0].generation, 0U);
    EXPECT_EQ(recorder.Log().records[1].generation, 1U);
    EXPECT_EQ(recorder.Log().dropped, 2U);

    FcpExchangeRecorder bytes(/*maxRecords=*/8, /*maxBytes=*/5);
    bytes.BeginSession();
    bytes.Record(0, FcpExchangeOutcome::kTimeout, false, 0, command, {});
    bytes.Record(0, FcpExchangeOutcome::kTimeout, false, 0, command, {});
    EXPECT_EQ(bytes.Log().records.size(), 1U);
    EXPECT_EQ(bytes.Log().dropped, 1U);
}

} // namespace
