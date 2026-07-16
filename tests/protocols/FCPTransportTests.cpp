#include <gtest/gtest.h>

#include "ASFWDriver/Discovery/FWDevice.hpp"
#include "ASFWDriver/Protocols/AVC/FCPTransport.hpp"
#include "DeferredFireWireBus.hpp"
#include "FakeSessionScheduler.hpp"

namespace {

using ASFW::Async::AsyncStatus;
using ASFW::Async::Testing::DeferredFireWireBus;
using ASFW::Discovery::ConfigROM;
using ASFW::Discovery::DeviceRecord;
using ASFW::Discovery::FWDevice;
using ASFW::FW::Generation;
using ASFW::Protocols::AVC::FCPCompletion;
using ASFW::Protocols::AVC::FCPFrame;
using ASFW::Protocols::AVC::FCPStatus;
using ASFW::Protocols::AVC::FCPTransport;
using ASFW::Protocols::AVC::FCPTransportConfig;
using ASFW::Testing::FakeSessionScheduler;

constexpr uint64_t kMillisecondNs = 1'000'000ULL;

FCPFrame MakeUnitInfoCommand() {
    FCPFrame command{};
    command.length = 3;
    command.data[0] = 0x00;  // STATUS
    command.data[1] = 0xFF;  // unit subunit address
    command.data[2] = 0x30;  // UNIT_INFO
    return command;
}

std::array<uint8_t, 3> MakeAcceptedUnitInfoResponse() {
    return {0x09, 0xFF, 0x30};  // ACCEPTED, unit, UNIT_INFO
}

class FCPTransportTests : public ::testing::Test {
protected:
    void SetUp() override {
        DeviceRecord record{};
        record.guid = 0x0001020304050607ULL;
        record.nodeId = 2;
        record.gen = Generation{1};
        device_ = FWDevice::Create(record, ConfigROM{});
        ASSERT_NE(device_, nullptr);

        config_.timeoutMs = 10;
        config_.interimTimeoutMs = 25;
        config_.maxRetries = 0;
        transport_ = std::make_shared<FCPTransport>();
        ASSERT_TRUE(transport_->init(&bus_, &bus_, device_.get(), scheduler_, config_));
    }

    void TearDown() override {
        if (transport_) {
            transport_->Shutdown();
        }
    }

    DeferredFireWireBus bus_;
    FakeSessionScheduler scheduler_;
    std::shared_ptr<FWDevice> device_;
    std::shared_ptr<FCPTransport> transport_;
    FCPTransportConfig config_{};
};

TEST_F(FCPTransportTests, IgnoresResponseUntilCommandWriteCompletes) {
    int completionCount = 0;
    FCPStatus completionStatus = FCPStatus::kTransportError;
    const auto command = MakeUnitInfoCommand();
    const auto response = MakeAcceptedUnitInfoResponse();

    const auto handle = transport_->SubmitCommand(
        command, [&completionCount, &completionStatus](FCPStatus status, const FCPFrame&) {
            ++completionCount;
            completionStatus = status;
        });
    ASSERT_TRUE(handle.IsValid());
    ASSERT_EQ(bus_.PendingWriteCount(), 1U);

    transport_->OnFCPResponse(2, 1, response);
    EXPECT_EQ(completionCount, 0);

    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    transport_->OnFCPResponse(2, 1, response);

    EXPECT_EQ(completionCount, 1);
    EXPECT_EQ(completionStatus, FCPStatus::kOk);
}

TEST_F(FCPTransportTests, IgnoresResponseFromDifferentGeneration) {
    int completionCount = 0;
    ASSERT_TRUE(transport_->SubmitCommand(
                             MakeUnitInfoCommand(),
                             [&completionCount](FCPStatus, const FCPFrame&) { ++completionCount; })
                    .IsValid());
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));

    const auto response = MakeAcceptedUnitInfoResponse();
    transport_->OnFCPResponse(2, 2, response);
    EXPECT_EQ(completionCount, 0);

    transport_->OnFCPResponse(2, 1, response);
    EXPECT_EQ(completionCount, 1);
}

TEST_F(FCPTransportTests, MatchesResponseAgainstNodeCapturedForWriteAttempt) {
    int completionCount = 0;
    ASSERT_TRUE(transport_->SubmitCommand(
                             MakeUnitInfoCommand(),
                             [&completionCount](FCPStatus, const FCPFrame&) { ++completionCount; })
                    .IsValid());
    ASSERT_EQ(bus_.WriteCount(), 1U);
    EXPECT_EQ(bus_.WriteAt(0).nodeId.value, 2U);
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));

    // A discovery rebind must not retroactively change the route expected by
    // the in-flight FCP request.
    device_->Publish();
    device_->Suspend();
    device_->Resume(Generation{2}, 3, {});

    const auto response = MakeAcceptedUnitInfoResponse();
    transport_->OnFCPResponse(3, 1, response);
    EXPECT_EQ(completionCount, 0);

    transport_->OnFCPResponse(2, 1, response);
    EXPECT_EQ(completionCount, 1);
}

TEST_F(FCPTransportTests, StartsTimeoutOnlyAfterCommandWriteCompletes) {
    int completionCount = 0;
    FCPStatus completionStatus = FCPStatus::kOk;

    ASSERT_TRUE(transport_->SubmitCommand(
                             MakeUnitInfoCommand(),
                             [&completionCount, &completionStatus](FCPStatus status, const FCPFrame&) {
                                 ++completionCount;
                                 completionStatus = status;
                             })
                    .IsValid());

    scheduler_.Advance(config_.timeoutMs * 2ULL * kMillisecondNs);
    EXPECT_EQ(completionCount, 0);
    EXPECT_EQ(scheduler_.PendingCount(), 0U);

    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    ASSERT_EQ(scheduler_.PendingCount(), 1U);
    scheduler_.Advance(config_.timeoutMs * kMillisecondNs - 1);
    EXPECT_EQ(completionCount, 0);

    scheduler_.Advance(1);
    EXPECT_EQ(completionCount, 1);
    EXPECT_EQ(completionStatus, FCPStatus::kTimeout);
}

TEST_F(FCPTransportTests, CancellingCommandCancelsItsTimeout) {
    int completionCount = 0;
    const auto handle = transport_->SubmitCommand(
        MakeUnitInfoCommand(), [&completionCount](FCPStatus, const FCPFrame&) { ++completionCount; });
    ASSERT_TRUE(handle.IsValid());
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    ASSERT_EQ(scheduler_.PendingCount(), 1U);

    EXPECT_TRUE(transport_->CancelCommand(handle));
    EXPECT_EQ(completionCount, 1);
    EXPECT_EQ(scheduler_.PendingCount(), 0U);

    scheduler_.Advance(config_.timeoutMs * kMillisecondNs);
    EXPECT_EQ(completionCount, 1);
}

TEST_F(FCPTransportTests, ResetRetryCancelsPreviousWriteBeforeResubmission) {
    config_.allowBusResetRetry = true;
    config_.maxRetries = 1;
    transport_->Shutdown();
    transport_ = std::make_shared<FCPTransport>();
    ASSERT_TRUE(transport_->init(&bus_, &bus_, device_.get(), scheduler_, config_));

    ASFW::Protocols::AVC::FCPCommandPolicy policy{};
    policy.retryClass = ASFW::Protocols::AVC::FCPRetryClass::kIdempotent;
    int completionCount = 0;
    ASSERT_TRUE(transport_->SubmitCommand(
                             MakeUnitInfoCommand(),
                             [&completionCount](FCPStatus, const FCPFrame&) { ++completionCount; },
                             std::move(policy))
                    .IsValid());
    ASSERT_EQ(bus_.PendingWriteCount(), 1U);

    bus_.SetGeneration(Generation{2});
    transport_->OnBusReset(2);

    EXPECT_EQ(bus_.WriteCount(), 2U);
    ASSERT_EQ(bus_.PendingWriteCount(), 1U);
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    const auto response = MakeAcceptedUnitInfoResponse();
    transport_->OnFCPResponse(2, 2, response);
    EXPECT_EQ(completionCount, 1);
}

TEST_F(FCPTransportTests, ControlCommandDoesNotRetryAfterTimeout) {
    config_.maxRetries = 1;
    transport_->Shutdown();
    transport_ = std::make_shared<FCPTransport>();
    ASSERT_TRUE(transport_->init(&bus_, &bus_, device_.get(), scheduler_, config_));

    FCPStatus completionStatus = FCPStatus::kOk;
    ASSERT_TRUE(transport_->SubmitCommand(
                             MakeUnitInfoCommand(),
                             [&completionStatus](FCPStatus status, const FCPFrame&) {
                                 completionStatus = status;
                             })
                    .IsValid());
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    scheduler_.Advance(config_.timeoutMs * kMillisecondNs);

    EXPECT_EQ(completionStatus, FCPStatus::kTimeout);
    EXPECT_EQ(bus_.WriteCount(), 1U);
}

TEST_F(FCPTransportTests, IdempotentCommandRetriesAfterTimeout) {
    config_.maxRetries = 1;
    transport_->Shutdown();
    transport_ = std::make_shared<FCPTransport>();
    ASSERT_TRUE(transport_->init(&bus_, &bus_, device_.get(), scheduler_, config_));

    ASFW::Protocols::AVC::FCPCommandPolicy policy{};
    policy.retryClass = ASFW::Protocols::AVC::FCPRetryClass::kIdempotent;
    FCPStatus completionStatus = FCPStatus::kTransportError;
    ASSERT_TRUE(transport_->SubmitCommand(
                             MakeUnitInfoCommand(),
                             [&completionStatus](FCPStatus status, const FCPFrame&) {
                                 completionStatus = status;
                             },
                             std::move(policy))
                    .IsValid());
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    scheduler_.Advance(config_.timeoutMs * kMillisecondNs);
    ASSERT_EQ(bus_.WriteCount(), 2U);

    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    transport_->OnFCPResponse(2, 1, MakeAcceptedUnitInfoResponse());
    EXPECT_EQ(completionStatus, FCPStatus::kOk);
}

TEST_F(FCPTransportTests, CommandSpecificMatcherRejectsSameOpcodeStaleResponse) {
    int completionCount = 0;
    FCPStatus completionStatus = FCPStatus::kTransportError;
    ASFW::Protocols::AVC::FCPCommandPolicy policy{};
    policy.responseMatcher = [](std::span<const uint8_t>, std::span<const uint8_t> response) {
        return response.size() >= 4U && response[3] == 0xA5U;
    };

    ASSERT_TRUE(transport_->SubmitCommand(
                             MakeUnitInfoCommand(),
                             [&completionCount, &completionStatus](FCPStatus status, const FCPFrame&) {
                                 ++completionCount;
                                 completionStatus = status;
                             },
                             std::move(policy))
                    .IsValid());
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));

    constexpr std::array<uint8_t, 4> stale{0x09, 0xFF, 0x30, 0x00};
    transport_->OnFCPResponse(2, 1, stale);
    EXPECT_EQ(completionCount, 0);

    constexpr std::array<uint8_t, 4> matching{0x09, 0xFF, 0x30, 0xA5};
    transport_->OnFCPResponse(2, 1, matching);
    EXPECT_EQ(completionCount, 1);
    EXPECT_EQ(completionStatus, FCPStatus::kOk);
}

TEST_F(FCPTransportTests, QueuesCommandsFifoAndAllowsQueuedCancellation) {
    std::vector<uint32_t> completions;
    const auto first = transport_->SubmitCommand(
        MakeUnitInfoCommand(), [&completions](FCPStatus, const FCPFrame&) { completions.push_back(1); });
    const auto second = transport_->SubmitCommand(
        MakeUnitInfoCommand(), [&completions](FCPStatus status, const FCPFrame&) {
            completions.push_back(status == FCPStatus::kOk ? 2U : 20U);
        });
    const auto third = transport_->SubmitCommand(
        MakeUnitInfoCommand(), [&completions](FCPStatus status, const FCPFrame&) {
            completions.push_back(status == FCPStatus::kTransportError ? 3U : 30U);
        });

    ASSERT_TRUE(first.IsValid());
    ASSERT_TRUE(second.IsValid());
    ASSERT_TRUE(third.IsValid());
    EXPECT_NE(first.transactionID, second.transactionID);
    EXPECT_EQ(bus_.WriteCount(), 1U);

    EXPECT_TRUE(transport_->CancelCommand(third));
    EXPECT_EQ(completions, std::vector<uint32_t>{3});

    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    transport_->OnFCPResponse(2, 1, MakeAcceptedUnitInfoResponse());
    EXPECT_EQ(completions, (std::vector<uint32_t>{3, 1}));
    EXPECT_EQ(bus_.WriteCount(), 2U);

    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));
    transport_->OnFCPResponse(2, 1, MakeAcceptedUnitInfoResponse());
    EXPECT_EQ(completions, (std::vector<uint32_t>{3, 1, 2}));
    EXPECT_EQ(bus_.WriteCount(), 2U);
}

} // namespace
