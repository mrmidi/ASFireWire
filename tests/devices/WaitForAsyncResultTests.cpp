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

} // namespace
