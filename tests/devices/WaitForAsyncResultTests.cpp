// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FW-65 characterization tests for the WaitForAsyncResult sync/async bridge.
//
// These lock the exact observable behaviour of the helper extracted from
// AudioDuplexCoordinator.cpp into SyncAsyncBridge.hpp, so the move is provably
// behaviour-preserving. Every timeout here is small (<= 60 ms) because the host mock
// IOSleep is a real std::this_thread::sleep_for.

#include <gtest/gtest.h>

#include "Audio/Protocols/Backends/SyncAsyncBridge.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

namespace {

using ASFW::Audio::SyncResult;
using ASFW::Audio::WaitForAsyncResult;

// A completion delivered synchronously before any timeout is forwarded verbatim.
TEST(WaitForAsyncResultTests, SuccessBeforeTimeout) {
    const auto r = WaitForAsyncResult<int>(
        [](auto cb) { cb(kIOReturnSuccess, 42); }, 60, kIOReturnTimeout);
    EXPECT_EQ(r.status, kIOReturnSuccess);
    EXPECT_EQ(r.value, 42);
}

// A non-success completion status is forwarded verbatim, paired with its value. This is
// what drives the coordinator's rollback FSM, so it must not be flattened to a sentinel.
TEST(WaitForAsyncResultTests, ErrorStatusForwardedVerbatim) {
    const auto r = WaitForAsyncResult<int>(
        [](auto cb) { cb(kIOReturnNoDevice, 7); }, 60, kIOReturnTimeout);
    EXPECT_EQ(r.status, kIOReturnNoDevice);
    EXPECT_EQ(r.value, 7);
}

// No completion -> the caller-supplied timeout status and a default-constructed value.
TEST(WaitForAsyncResultTests, TimeoutReturnsSuppliedStatusAndDefaultValue) {
    const auto r = WaitForAsyncResult<int>(
        [](auto) { /* never completes */ }, 30, kIOReturnTimeout);
    EXPECT_EQ(r.status, kIOReturnTimeout);
    EXPECT_EQ(r.value, 0);
}

// The timeout status is whatever the caller passes, not hard-coded to kIOReturnTimeout.
TEST(WaitForAsyncResultTests, TimeoutStatusIsCallerSupplied) {
    const auto r = WaitForAsyncResult<int>(
        [](auto) {}, 30, kIOReturnError);
    EXPECT_EQ(r.status, kIOReturnError);
}

// Cancel token set while waiting -> aborts promptly (not at timeout) with kIOReturnAborted.
// This is the FW-61 teardown unblock: the completion cannot arrive during teardown, so
// cancel, not done, is what releases the waiter.
TEST(WaitForAsyncResultTests, CancelDuringWaitReturnsAborted) {
    std::atomic<bool> cancel{false};
    std::thread setter([&cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        cancel.store(true, std::memory_order_release);
    });

    const auto start = std::chrono::steady_clock::now();
    const auto r = WaitForAsyncResult<int>(
        [](auto) { /* never completes */ }, 5000, kIOReturnTimeout, &cancel);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    setter.join();

    EXPECT_EQ(r.status, kIOReturnAborted);
    EXPECT_EQ(r.value, 0);
    // Aborted long before the 5 s timeout would have elapsed.
    EXPECT_LT(elapsed, std::chrono::milliseconds(2000));
}

// Cancel already set before the call -> aborts on the first loop iteration (cancel wins
// over a still-pending operation).
TEST(WaitForAsyncResultTests, CancelSetBeforeCallReturnsAborted) {
    std::atomic<bool> cancel{true};
    const auto start = std::chrono::steady_clock::now();
    const auto r = WaitForAsyncResult<int>(
        [](auto) {}, 5000, kIOReturnTimeout, &cancel);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_EQ(r.status, kIOReturnAborted);
    EXPECT_LT(elapsed, std::chrono::milliseconds(2000));
}

// cancel == nullptr is safe (no dereference) and normal completion still works.
TEST(WaitForAsyncResultTests, CancelNullptrIsSafe) {
    const auto r = WaitForAsyncResult<int>(
        [](auto cb) { cb(kIOReturnSuccess, 5); }, 60, kIOReturnTimeout, nullptr);
    EXPECT_EQ(r.status, kIOReturnSuccess);
    EXPECT_EQ(r.value, 5);
}

// A completion delivered asynchronously during the wait (from another thread) is picked up
// by the poll loop and returned. Also exercises the by-value [state] capture: the callback
// (holding the shared WaitState) outlives the WaitForAsyncResult frame via the worker
// thread, so writing to the result is safe.
TEST(WaitForAsyncResultTests, CompletionDuringWaitIsDelivered) {
    std::thread worker;
    const auto r = WaitForAsyncResult<int>(
        [&worker](auto cb) {
            worker = std::thread([cb = std::move(cb)]() mutable {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                cb(kIOReturnSuccess, 99);
            });
        },
        200, kIOReturnTimeout);
    worker.join();
    EXPECT_EQ(r.status, kIOReturnSuccess);
    EXPECT_EQ(r.value, 99);
}

// timeoutMs == 0: the poll loop body never executes (0 < 0 is false), so the ONLY path that
// can observe an inline completion is the post-loop final done.load(). A synchronous
// completion must still be returned. This deterministically pins the final load that an
// over-eager "dead code" cleanup would delete.
TEST(WaitForAsyncResultTests, FinalLoadCatchesInlineCompletionWhenLoopSkipped) {
    const auto r = WaitForAsyncResult<int>(
        [](auto cb) { cb(kIOReturnSuccess, 77); }, 0, kIOReturnTimeout);
    EXPECT_EQ(r.status, kIOReturnSuccess);
    EXPECT_EQ(r.value, 77);
}

// timeoutMs == 0 with no completion -> immediate timeout.
TEST(WaitForAsyncResultTests, ZeroTimeoutWithoutCompletionReturnsTimeout) {
    const auto r = WaitForAsyncResult<int>([](auto) {}, 0, kIOReturnTimeout);
    EXPECT_EQ(r.status, kIOReturnTimeout);
    EXPECT_EQ(r.value, 0);
}

// ---- Predicate-based cancel tests ----

// A callable predicate that returns true aborts with kIOReturnAborted, just like the
// atomic-bool overload.
TEST(WaitForAsyncResultTests, PredicateCancelReturnsAborted) {
    std::atomic<bool> shouldCancel{false};
    const auto start = std::chrono::steady_clock::now();
    std::thread setter([&shouldCancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        shouldCancel.store(true, std::memory_order_release);
    });

    const auto r = WaitForAsyncResult<int>(
        [](auto) { /* never completes */ },
        5000,
        kIOReturnTimeout,
        [&shouldCancel]() noexcept { return shouldCancel.load(std::memory_order_acquire); });
    const auto elapsed = std::chrono::steady_clock::now() - start;
    setter.join();

    EXPECT_EQ(r.status, kIOReturnAborted);
    EXPECT_EQ(r.value, 0);
    EXPECT_LT(elapsed, std::chrono::milliseconds(2000));
}

// The custom poll interval paces an actual wait: the predicate fires once per loop
// iteration, so cancelling on the 3rd call proves three iterations ran, and each
// iteration sleeps one interval. With interval=40 the waiter observes two real 40 ms
// waits before the cancel — a default-interval (10 ms) loop would finish at ~20 ms.
TEST(WaitForAsyncResultTests, CustomPollIntervalPacesTheWait) {
    std::atomic<int> predicateCalls{0};
    const auto start = std::chrono::steady_clock::now();
    const auto r = WaitForAsyncResult<int>(
        [](auto) { /* never completes */ },
        5000,
        kIOReturnTimeout,
        [&predicateCalls]() noexcept {
            return predicateCalls.fetch_add(1, std::memory_order_acq_rel) + 1 >= 3;
        },
        40);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count();

    EXPECT_EQ(r.status, kIOReturnAborted);
    EXPECT_EQ(predicateCalls.load(std::memory_order_acquire), 3);
    // Two full 40 ms waits must have elapsed. Generous margin above the ideal 80 ms
    // total; the lower bound is what distinguishes interval=40 from the 10 ms default.
    EXPECT_GE(elapsed, 60);
    EXPECT_LT(elapsed, 2000);
}

// Loop-boundary semantics: the first iteration's condition is 0 < timeoutMs, NOT
// pollIntervalMs < timeoutMs. With interval=50 > timeout=30 the body still runs once
// (one predicate call), then 50 < 30 fails and the loop exits to the final load. If the
// condition were pollIntervalMs < timeoutMs, the body would never run and the predicate
// would never fire.
TEST(WaitForAsyncResultTests, LoopBodyRunsOnceEvenWhenIntervalExceedsTimeout) {
    std::atomic<int> predicateCalls{0};
    const auto r = WaitForAsyncResult<int>(
        [](auto) { /* never completes */ },
        30,
        kIOReturnTimeout,
        [&predicateCalls]() noexcept {
            predicateCalls.fetch_add(1, std::memory_order_acq_rel);
            return false;
        },
        50);

    EXPECT_EQ(r.status, kIOReturnTimeout);
    EXPECT_EQ(predicateCalls.load(std::memory_order_acquire), 1);
}



// ---- WaitForAsyncStatus tests ----

using ASFW::Audio::WaitForAsyncStatus;

// Synchronous success via WaitForAsyncStatus.
TEST(WaitForAsyncStatusTests, SuccessBeforeTimeout) {
    const auto s = WaitForAsyncStatus(
        [](auto cb) { cb(kIOReturnSuccess); }, 60, kIOReturnTimeout);
    EXPECT_EQ(s, kIOReturnSuccess);
}

// Non-success forwarded verbatim via WaitForAsyncStatus.
TEST(WaitForAsyncStatusTests, ErrorStatusForwardedVerbatim) {
    const auto s = WaitForAsyncStatus(
        [](auto cb) { cb(kIOReturnNoDevice); }, 60, kIOReturnTimeout);
    EXPECT_EQ(s, kIOReturnNoDevice);
}

// No completion → timeout.
TEST(WaitForAsyncStatusTests, TimeoutReturnsSuppliedStatus) {
    const auto s = WaitForAsyncStatus(
        [](auto) { /* never completes */ }, 30, kIOReturnTimeout);
    EXPECT_EQ(s, kIOReturnTimeout);
}

// Atomic cancel token aborts with kIOReturnAborted.
TEST(WaitForAsyncStatusTests, CancelReturnsAborted) {
    std::atomic<bool> cancel{false};
    std::thread setter([&cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        cancel.store(true, std::memory_order_release);
    });

    const auto start = std::chrono::steady_clock::now();
    const auto s = WaitForAsyncStatus(
        [](auto) { /* never completes */ }, 5000, kIOReturnTimeout, &cancel);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    setter.join();

    EXPECT_EQ(s, kIOReturnAborted);
    EXPECT_LT(elapsed, std::chrono::milliseconds(2000));
}

// Predicate cancel via WaitForAsyncStatus.
TEST(WaitForAsyncStatusTests, PredicateCancelReturnsAborted) {
    std::atomic<bool> shouldCancel{false};
    std::thread setter([&shouldCancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        shouldCancel.store(true, std::memory_order_release);
    });

    const auto start = std::chrono::steady_clock::now();
    const auto s = WaitForAsyncStatus(
        [](auto) {},
        5000,
        kIOReturnTimeout,
        [&shouldCancel]() noexcept { return shouldCancel.load(std::memory_order_acquire); });
    const auto elapsed = std::chrono::steady_clock::now() - start;
    setter.join();

    EXPECT_EQ(s, kIOReturnAborted);
    EXPECT_LT(elapsed, std::chrono::milliseconds(2000));
}

// Async completion during wait via WaitForAsyncStatus.
TEST(WaitForAsyncStatusTests, CompletionDuringWaitIsDelivered) {
    std::thread worker;
    const auto s = WaitForAsyncStatus(
        [&worker](auto cb) {
            worker = std::thread([cb = std::move(cb)]() mutable {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                cb(kIOReturnSuccess);
            });
        },
        200, kIOReturnTimeout);
    worker.join();
    EXPECT_EQ(s, kIOReturnSuccess);
}

// Abort provenance: device callback completing with kIOReturnAborted must NOT set wasCancelled.
// Even if teardown/cancellation latches immediately after the wait completes, the provenance
// was not a lifecycle cancellation, preserving correct accounting.
TEST(WaitForAsyncResultTests, DeviceCallbackAbortDoesNotSetWasCancelled) {
    std::atomic<bool> teardownLatch{false};
    const auto r = WaitForAsyncResult<int>(
        [](auto cb) {
            // Device returns kIOReturnAborted on its own (e.g. bus reset or rejected command)
            cb(kIOReturnAborted, 0);
        },
        60,
        kIOReturnTimeout,
        [&teardownLatch]() noexcept { return teardownLatch.load(std::memory_order_acquire); });

    // Teardown latches after the wait completed
    teardownLatch.store(true, std::memory_order_release);

    EXPECT_EQ(r.status, kIOReturnAborted);
    EXPECT_FALSE(r.wasCancelled);
    EXPECT_FALSE(r.timedOut);
}

// Abort provenance: when the cancellation predicate fires, wasCancelled MUST be set to true.
TEST(WaitForAsyncResultTests, PredicateCancelSetsWasCancelled) {
    std::atomic<bool> cancelled{true};
    const auto r = WaitForAsyncResult<int>(
        [](auto) { /* never completes */ },
        5000,
        kIOReturnTimeout,
        [&cancelled]() noexcept { return cancelled.load(std::memory_order_acquire); });

    EXPECT_EQ(r.status, kIOReturnAborted);
    EXPECT_TRUE(r.wasCancelled);
    EXPECT_FALSE(r.timedOut);
}

// Timeout provenance: timeout sets timedOut to true and wasCancelled to false.
TEST(WaitForAsyncResultTests, TimeoutSetsTimedOutFlag) {
    const auto r = WaitForAsyncResult<int>(
        [](auto) { /* never completes */ },
        20,
        kIOReturnTimeout);

    EXPECT_EQ(r.status, kIOReturnTimeout);
    EXPECT_TRUE(r.timedOut);
    EXPECT_FALSE(r.wasCancelled);
}

} // namespace

