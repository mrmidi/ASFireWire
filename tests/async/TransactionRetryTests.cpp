// SPDX-License-Identifier: Apache-2.0
#include "ASFWDriver/Async/Core/TransactionRetry.hpp"
#include "tests/mocks/FakeTimerScheduler.hpp"
#include <gtest/gtest.h>

namespace {
using namespace ASFW::Async;
using Operation = TransactionRetry<uint32_t>;
struct Rig {
    ASFW::Testing::FakeTimerScheduler timer;
    AsyncStatus response{AsyncStatus::kSuccess};
    unsigned submissions{0}, callbacks{0}, cancellations{0};
    AsyncStatus final{AsyncStatus::kHardwareError};
    bool current{true}, defer{false}, admit{true};
    Operation::Callback pending;
    std::shared_ptr<Operation> Start(TransactionRetryPolicy policy = {}) {
        auto op = Operation::Create(&timer, policy,
            [this](Operation::Callback callback) {
                ++submissions;
                if (!admit) return AsyncHandle{};
                if (defer) pending = std::move(callback);
                else callback(response, 42);
                return AsyncHandle{submissions};
            }, [this](AsyncHandle) { ++cancellations; },
            [this] { return current; },
            [this](AsyncStatus status, uint32_t) { ++callbacks; final = status; });
        op->Begin();
        return op;
    }
};

TEST(TransactionRetryCore, AppleBusyBudgetIncludesFinalTimeoutWait) {
    Rig r; r.response = AsyncStatus::kBusyRetryExhausted;
    auto op = r.Start();
    r.timer.Advance(124'999'999);
    EXPECT_EQ(r.submissions, 1U);
    r.timer.Advance(1);
    EXPECT_EQ(r.submissions, 2U);
    r.timer.Advance(250'000'000);
    EXPECT_EQ(r.submissions, 4U);
    EXPECT_EQ(r.callbacks, 0U);
    r.timer.Advance(125'000'000);
    EXPECT_EQ(r.callbacks, 1U);
    EXPECT_EQ(r.final, AsyncStatus::kTimeout);
    EXPECT_TRUE(op->Done());
}

TEST(TransactionRetryCore, BusyWaitIsMeasuredFromSubmissionNotCompletion) {
    Rig r; r.defer = true;
    auto op = r.Start();
    r.timer.Advance(100'000'000);
    r.pending(AsyncStatus::kBusyRetryExhausted, 0);
    r.timer.Advance(24'999'999);
    EXPECT_EQ(r.submissions, 1U);
    r.timer.Advance(1);
    EXPECT_EQ(r.submissions, 2U);
    op->Cancel();
}

TEST(TransactionRetryCore, AppleDoesNotBlindlyRepeatAcceptedReadTimeout) {
    Rig r; r.response = AsyncStatus::kTimeout;
    auto op = r.Start();
    r.timer.Advance(1'000'000'000);
    EXPECT_EQ(r.submissions, 1U);
    EXPECT_EQ(r.callbacks, 1U);
    EXPECT_EQ(r.final, AsyncStatus::kTimeout);
}

TEST(TransactionRetryCore, VendorControlBudgetIsSeventeenCommandsAboveAppleBudget) {
    Rig r; r.response = AsyncStatus::kBusyRetryExhausted;
    auto op = r.Start({.repeat = CommandRepeat::Failure, .commandAttempts = 17});
    r.timer.Advance(8'499'999'999);
    EXPECT_EQ(r.submissions, 68U);
    EXPECT_EQ(r.callbacks, 0U);
    r.timer.Advance(1);
    EXPECT_EQ(r.callbacks, 1U);
    EXPECT_EQ(r.final, AsyncStatus::kTimeout);
}

TEST(TransactionRetryCore, VendorControlRepeatsOtherErrorsWithoutBlockingOrRecursion) {
    Rig r; r.response = AsyncStatus::kShortRead;
    auto op = r.Start({.repeat = CommandRepeat::Failure, .commandAttempts = 17});
    EXPECT_EQ(r.submissions, 1U);
    r.timer.Advance(0);
    EXPECT_EQ(r.submissions, 17U);
    EXPECT_EQ(r.callbacks, 1U);
    EXPECT_EQ(r.final, AsyncStatus::kShortRead);
}

TEST(TransactionRetryCore, FailedAdmissionUsesSameBoundedControlBudget) {
    Rig r; r.admit = false;
    auto op = r.Start({.repeat = CommandRepeat::Failure, .commandAttempts = 17});
    r.timer.Advance(0);
    EXPECT_EQ(r.submissions, 17U);
    EXPECT_EQ(r.callbacks, 1U);
    EXPECT_EQ(r.final, AsyncStatus::kHardwareError);
}

TEST(TransactionRetryCore, VendorAsyncReadHasSixteenTimeoutAttemptsSpacedTenMilliseconds) {
    Rig r; r.response = AsyncStatus::kTimeout;
    auto op = r.Start({.repeat = CommandRepeat::Timeout, .commandAttempts = 16,
                       .repeatDelayNs = 10'000'000});
    r.timer.Advance(149'999'999);
    EXPECT_EQ(r.submissions, 15U);
    EXPECT_EQ(r.callbacks, 0U);
    r.timer.Advance(1);
    EXPECT_EQ(r.submissions, 16U);
    EXPECT_EQ(r.callbacks, 1U);
}

TEST(TransactionRetryCore, VendorAsyncReadDoesNotRepeatOtherErrors) {
    Rig r; r.response = AsyncStatus::kShortRead;
    auto op = r.Start({.repeat = CommandRepeat::Timeout, .commandAttempts = 16});
    r.timer.Advance(1'000'000'000);
    EXPECT_EQ(r.submissions, 1U);
    EXPECT_EQ(r.final, AsyncStatus::kShortRead);
}

TEST(TransactionRetryCore, StaleRouteOrAbortNeverConsumesAnOuterRetryBudget) {
    for (const auto status : {AsyncStatus::kAborted, AsyncStatus::kStaleGeneration}) {
        Rig r; r.response = status;
        auto op = r.Start({.repeat = CommandRepeat::Failure, .commandAttempts = 17});
        r.timer.Advance(1'000'000'000);
        EXPECT_EQ(r.submissions, 1U);
        EXPECT_EQ(r.callbacks, 1U);
        EXPECT_EQ(r.final, status);
    }
}

TEST(TransactionRetryCore, CoalescedReadWaitsFiveMillisecondsAndReturnsFreshResult) {
    Rig r; r.defer = true;
    auto op = r.Start({.coalescedDelayNs = 5'000'000});
    unsigned second = 0, third = 0;
    Operation::Callback cb2 = [&](AsyncStatus s, uint32_t v) {
        EXPECT_EQ(s, AsyncStatus::kSuccess); EXPECT_EQ(v, 99U); ++second;
    };
    Operation::Callback cb3 = [&](AsyncStatus, uint32_t v) { EXPECT_EQ(v, 99U); ++third; };
    ASSERT_TRUE(op->Coalesce(cb2)); ASSERT_TRUE(op->Coalesce(cb3));
    auto old = r.pending;
    old(AsyncStatus::kSuccess, 42);
    EXPECT_EQ(r.callbacks, 1U); EXPECT_EQ(second, 0U);
    r.timer.Advance(4'999'999); EXPECT_EQ(r.submissions, 1U);
    r.timer.Advance(1); EXPECT_EQ(r.submissions, 2U);
    old(AsyncStatus::kSuccess, 42); // Duplicate completion cannot finish the new read.
    EXPECT_EQ(second, 0U);
    r.pending(AsyncStatus::kSuccess, 99);
    EXPECT_EQ(second, 1U); EXPECT_EQ(third, 1U);
    EXPECT_TRUE(op->Done());
}

TEST(TransactionRetryCore, CancelCoalescedDelayCompletesQueuedRequestOnce) {
    Rig r; r.defer = true;
    auto op = r.Start({.coalescedDelayNs = 5'000'000});
    unsigned queued = 0;
    Operation::Callback cb = [&](AsyncStatus s, uint32_t) { EXPECT_EQ(s, AsyncStatus::kAborted); ++queued; };
    ASSERT_TRUE(op->Coalesce(cb));
    r.pending(AsyncStatus::kSuccess, 42);
    op->Cancel();
    r.timer.Advance(5'000'000);
    EXPECT_EQ(r.submissions, 1U); EXPECT_EQ(queued, 1U);
    EXPECT_EQ(r.timer.PendingCount(), 0U);
}

TEST(TransactionRetryCore, OwnerCancelsAllOutstandingCommandsAndRejectsNewOnes) {
    Rig r; r.defer = true;
    TransactionRetryOwner<uint32_t> owner;
    auto first = r.Start(), second = r.Start();
    owner.Begin(first); owner.Begin(second);
    owner.CancelAll(true);
    EXPECT_EQ(r.callbacks, 2U); EXPECT_EQ(r.cancellations, 2U);
    unsigned rejected = 0;
    auto next = Operation::Create(&r.timer, {},
        [](Operation::Callback) { ADD_FAILURE(); return AsyncHandle{}; },
        [](AsyncHandle) {}, [] { return true; },
        [&](AsyncStatus s, uint32_t) { EXPECT_EQ(s, AsyncStatus::kAborted); ++rejected; });
    owner.Begin(next);
    EXPECT_EQ(rejected, 1U);
}

TEST(TransactionRetryCore, ResetDuringBackoffDoesNotSubmitAgain) {
    Rig r; r.response = AsyncStatus::kBusyRetryExhausted;
    auto op = r.Start();
    r.current = false;
    r.timer.Advance(125'000'000);
    EXPECT_EQ(r.submissions, 1U);
    EXPECT_EQ(r.final, AsyncStatus::kStaleGeneration);
}

TEST(TransactionRetryCore, MissingTimerFailsWithoutImmediateRetryStorm) {
    unsigned submissions = 0, callbacks = 0;
    auto op = Operation::Create(nullptr, {.repeat = CommandRepeat::Failure, .commandAttempts = 17},
        [&](Operation::Callback cb) { ++submissions; cb(AsyncStatus::kBusyRetryExhausted, 0); return AsyncHandle{1}; },
        [](AsyncHandle) {}, [] { return true; },
        [&](AsyncStatus s, uint32_t) { EXPECT_EQ(s, AsyncStatus::kBusyRetryExhausted); ++callbacks; });
    op->Begin();
    EXPECT_EQ(submissions, 1U); EXPECT_EQ(callbacks, 1U);
}

TEST(TransactionRetryCore, MissingTimerRejectsCoalescingWithoutConsumingCallback) {
    Operation::Callback pending;
    auto op = Operation::Create(nullptr, {},
        [&](Operation::Callback cb) { pending = std::move(cb); return AsyncHandle{1}; },
        [](AsyncHandle) {}, [] { return true; }, [](AsyncStatus, uint32_t) {});
    op->Begin();
    Operation::Callback next = [](AsyncStatus, uint32_t) {};
    EXPECT_FALSE(op->Coalesce(next));
    EXPECT_TRUE(static_cast<bool>(next));
    op->Cancel();
}

TEST(TransactionRetryCore, TimerAdmissionFailureCompletesOnceWithoutResubmission) {
    struct Scheduler final : ASFW::Scheduling::ITimerScheduler {
        ASFW::Scheduling::TimerToken ScheduleAfter(uint64_t, std::function<void()>) override { return 0; }
        void Cancel(ASFW::Scheduling::TimerToken) override { ADD_FAILURE(); }
        uint64_t NowNs() const noexcept override { return 0; }
    } timer;
    unsigned submissions = 0, callbacks = 0;
    auto op = Operation::Create(&timer, {},
        [&](Operation::Callback cb) { ++submissions; cb(AsyncStatus::kBusyRetryExhausted, 0); return AsyncHandle{1}; },
        [](AsyncHandle) {}, [] { return true; },
        [&](AsyncStatus s, uint32_t) { EXPECT_EQ(s, AsyncStatus::kHardwareError); ++callbacks; });
    op->Begin();
    EXPECT_EQ(submissions, 1U); EXPECT_EQ(callbacks, 1U);
    EXPECT_TRUE(op->Done());
}
} // namespace

