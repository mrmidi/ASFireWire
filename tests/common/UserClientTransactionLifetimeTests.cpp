// SPDX-License-Identifier: Apache-2.0
#include "ASFWDriver.h"
#include "ASFWDriverUserClient.h"
#include "ASFWDriver/UserClient/Handlers/TransactionHandler.hpp"
#include "ASFWDriver/Async/Interfaces/IAsyncSubsystemPort.hpp"
#include <DriverKit/OSData.h>
#include "ASFWDriver/Async/Core/TransactionManager.hpp"
#include <gtest/gtest.h>
#include <array>
#include <barrier>
#include <thread>
#include <vector>

namespace {
using namespace ASFW::Async;
class DeferredPort final : public IAsyncSubsystemPort {
public:
    std::vector<CompletionCallback> pending;
    bool reject{false};
    bool rejectAfterRegistration{false};
    AsyncHandle Store(CompletionCallback cb) {
        if (reject) return {};
        pending.push_back(std::move(cb));
        return rejectAfterRegistration ? AsyncHandle{} : AsyncHandle{1};
    }
    AsyncHandle Read(const ReadParams&, CompletionCallback cb) override { return Store(std::move(cb)); }
    AsyncHandle ReadWithRetry(const ReadParams&, const RetryPolicy&, CompletionCallback cb) override { return Store(std::move(cb)); }
    AsyncHandle Write(const WriteParams&, CompletionCallback cb) override { return Store(std::move(cb)); }
    AsyncHandle Lock(const LockParams&, uint16_t, CompletionCallback cb) override { return Store(std::move(cb)); }
    AsyncHandle CompareSwap(const CompareSwapParams&, CompareSwapCallback) override { return {}; }
    AsyncHandle PhyRequest(const PhyParams&, CompletionCallback cb) override { return Store(std::move(cb)); }
    bool Cancel(AsyncHandle) override { return false; }
    void OnTimeoutTick() override {}
    AsyncWatchdogStats GetWatchdogStats() const override { return {}; }
    ASFW::Debug::BusResetPacketCapture* GetBusResetCapture() const override { return nullptr; }
    ASFW::Debug::AsyncTraceCapture* GetAsyncTraceCapture() const override { return nullptr; }
    ASFWDiagInboundCSRStats* GetInboundCSRStats() const override { return nullptr; }
    std::optional<AsyncStatusSnapshot> GetStatusSnapshot() const override { return {}; }
    void Complete() {
        auto callbacks = std::move(pending);
        pending.clear();
        for (auto& cb : callbacks) cb(AsyncHandle{1}, AsyncStatus::kAborted, 0xFF, {});
    }
    void Discard() { pending.clear(); }
};

// Each test executes the real handler. The IIG object and transport are mocks.
class UserClientTransactionLifetimeTests : public testing::TestWithParam<int> {
protected:
    DeferredPort port;
    ASFWDriver driver;
    std::atomic<bool> destroyed{false};
    ASFWDriverUserClient* client{new ASFWDriverUserClient()};
    ASFW::UserClient::TransactionHandler handler{&driver, nullptr};
    std::array<uint64_t, 4> inputs{0xFFC1, 0xFFFF, 0xF0000400, 4};
    std::array<uint64_t, 2> outputs{};
    std::array<uint8_t, 8> operand{};
    OSSharedPtr<OSData> payload;
    IOUserClientMethodArguments args{};
    void SetUp() override {
        driver.asyncPort = &port;
        client->ivars->driver = &driver;
        client->destroyed = &destroyed;
        args.scalarInput = inputs.data(); args.scalarInputCount = inputs.size();
        args.scalarOutput = outputs.data(); args.scalarOutputCount = outputs.size();
        payload = OSSharedPtr(OSData::withBytes(operand.data(), GetParam() == 4 ? 8 : 4), OSNoRetain);
        args.structureInput = payload.get();
    }
    void TearDown() override {
        port.Discard();
        // Keep the red regression run leak-free on platforms without LSan.
        // After the fix this loop releases exactly the test's one reference.
        if (!destroyed.load()) {
            while (client->GetRetainCount() > 1) client->release();
            client->release();
        }
    }
    kern_return_t Submit() {
        switch (GetParam()) {
        case 0: return handler.AsyncRead(&args, client);
        case 1: return handler.AsyncWrite(&args, client);
        case 2: return handler.AsyncBlockRead(&args, client);
        case 3: return handler.AsyncBlockWrite(&args, client);
        default: return handler.AsyncCompareSwap(&args, client);
        }
    }
};
TEST_P(UserClientTransactionLifetimeTests, DiscardedCallbackReleasesClient) {
    ASSERT_EQ(Submit(), kIOReturnSuccess);
    ASSERT_EQ(client->GetRetainCount(), 2);
    port.Discard();
    EXPECT_EQ(client->GetRetainCount(), 1);
    EXPECT_EQ(client->ivars->transactionHolds, 0u);
}
TEST_P(UserClientTransactionLifetimeTests, TerminalTransactionCancelAllReleasesClient) {
    ASSERT_EQ(Submit(), kIOReturnSuccess);
    TransactionManager manager;
    ASSERT_TRUE(manager.Initialize());
    auto result = manager.Allocate(TLabel{0}, BusGeneration{1}, NodeID{0xFFC1});
    ASSERT_TRUE(result);
    auto callback = std::move(port.pending.front());
    port.pending.clear();
    (*result)->SetResponseHandler([callback = std::move(callback)](kern_return_t, uint8_t code,
                                                                   std::span<const uint8_t> bytes) {
        callback(AsyncHandle{1}, AsyncStatus::kAborted, code, bytes);
    });
    (*result)->TransitionTo(TransactionState::Cancelled, "terminal transaction discarded by teardown");
    manager.CancelAll(); // real production path skips callback for terminal slots
    EXPECT_EQ(client->GetRetainCount(), 1);
    EXPECT_EQ(client->ivars->transactionHolds, 0u);
}
TEST_P(UserClientTransactionLifetimeTests, CopiedCallbackOwnsOneClientRetain) {
    ASSERT_EQ(Submit(), kIOReturnSuccess);
    auto copy = port.pending.front();
    port.Discard();
    EXPECT_EQ(client->GetRetainCount(), 2);
    EXPECT_EQ(client->ivars->transactionHolds, 1u);
    copy = {};
    EXPECT_EQ(client->GetRetainCount(), 1);
    EXPECT_EQ(client->ivars->transactionHolds, 0u);
}
TEST_P(UserClientTransactionLifetimeTests, AppCloseThenDiscardFreesClient) {
    ASSERT_EQ(Submit(), kIOReturnSuccess);
    client->release(); // app/framework connection owner leaves
    port.Discard();   // queued or terminal transaction destroyed at teardown
    EXPECT_TRUE(destroyed.load());
}
TEST_P(UserClientTransactionLifetimeTests, CompletionAfterAppCloseFreesClient) {
    ASSERT_EQ(Submit(), kIOReturnSuccess);
    client->release();
    port.Complete();
    EXPECT_TRUE(destroyed.load());
}
TEST_P(UserClientTransactionLifetimeTests, RejectedSubmissionDoesNotRetainClient) {
    port.reject = true;
    EXPECT_NE(Submit(), kIOReturnSuccess);
    EXPECT_EQ(client->GetRetainCount(), 1);
    EXPECT_EQ(client->ivars->transactionHolds, 0u);
}
// AsyncCommandImpl can fail after Tracking::RegisterTx has retained the
// callback (e.g. descriptor-chain construction). The invalid handle does not
// guarantee that no transaction still owns that callable.
TEST_P(UserClientTransactionLifetimeTests, FailureAfterRegistrationThenCompletionIsSafe) {
    port.rejectAfterRegistration = true;
    ASSERT_NE(Submit(), kIOReturnSuccess);
    client->release();
    port.Complete();
    EXPECT_TRUE(destroyed.load());
}
TEST_P(UserClientTransactionLifetimeTests, FailureAfterRegistrationThenDiscardFreesClient) {
    port.rejectAfterRegistration = true;
    ASSERT_NE(Submit(), kIOReturnSuccess);
    client->release();
    port.Discard();
    EXPECT_TRUE(destroyed.load());
}
TEST_P(UserClientTransactionLifetimeTests, CompletionRacingWithAppCloseFreesClient) {
    ASSERT_EQ(Submit(), kIOReturnSuccess);
    std::barrier start(2);
    std::thread completion([&] { start.arrive_and_wait(); port.Complete(); });
    start.arrive_and_wait();
    client->release();
    completion.join();
    EXPECT_TRUE(destroyed.load());
}
INSTANTIATE_TEST_SUITE_P(AllSubmissionKinds, UserClientTransactionLifetimeTests, testing::Range(0, 5));
}
