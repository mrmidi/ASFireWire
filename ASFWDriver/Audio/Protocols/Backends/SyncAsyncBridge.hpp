// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SyncAsyncBridge.hpp
//
// A reusable synchronous/asynchronous bridge (FW-65). WaitForAsyncResult starts an
// async operation and blocks the calling thread, polling a shared completion flag until
// the operation completes, a teardown cancel token fires (FW-61), or a timeout elapses.
//
// Used by Session::DuplexControlAdapter, the one place the session turns callback-style
// device stages into blocking calls, and by the DICE backend's health reads.

#pragma once

#include <DriverKit/IOLib.h>

#include <atomic>
#include <concepts>
#include <cstdint>
#include <memory>
#include <utility>

namespace ASFW::Audio {

// Poll interval for the blocking bridge, in milliseconds; do not change without a
// behaviour review. `inline` so the single definition is shared across every TU that
// includes this header.
inline constexpr uint32_t kWaitPollMs = 10;

template <typename T>
struct SyncResult {
    IOReturn status{kIOReturnTimeout};
    T value{};
    bool wasCancelled{false};
    bool timedOut{false};
};

/// Starts an async operation taking a `void(IOReturn, T)` callback and polls until
/// completion, predicate cancellation, or timeout.
template <typename T, typename StartFn, typename CancelFn>
requires std::invocable<CancelFn>
SyncResult<T> WaitForAsyncResult(StartFn&& fn,
                                 uint32_t timeoutMs,
                                 IOReturn timeoutStatus,
                                 CancelFn&& isCancelled,
                                 uint32_t pollIntervalMs = kWaitPollMs) noexcept {
    struct WaitState {
        std::atomic<bool> done{false};
        SyncResult<T> result{};
    };

    auto state = std::make_shared<WaitState>();
    fn([state](IOReturn status, T value) {
        state->result.status = status;
        state->result.value = std::move(value);
        state->done.store(true, std::memory_order_release);
    });

    const uint32_t pollMs = (pollIntervalMs == 0) ? kWaitPollMs : pollIntervalMs;
    for (uint32_t waited = 0; waited < timeoutMs; waited += pollMs) {
        if (state->done.load(std::memory_order_acquire)) {
            return state->result;
        }
        // FW-61: abort promptly when the cancellation predicate fires so threads drain
        // quickly instead of running to timeout while hardware/queues are quiescing.
        if (isCancelled()) {
            SyncResult<T> aborted{};
            aborted.status = kIOReturnAborted;
            aborted.wasCancelled = true;
            return aborted;
        }
        IOSleep(pollMs);
    }

    if (state->done.load(std::memory_order_acquire)) {
        return state->result;
    }

    SyncResult<T> timeout{};
    timeout.status = timeoutStatus;
    timeout.timedOut = true;
    return timeout;
}

/// Pointer-based overload of WaitForAsyncResult for atomic cancel tokens.
template <typename T, typename StartFn>
SyncResult<T> WaitForAsyncResult(StartFn&& fn,
                                 uint32_t timeoutMs,
                                 IOReturn timeoutStatus,
                                 const std::atomic<bool>* cancel = nullptr,
                                 uint32_t pollIntervalMs = kWaitPollMs) noexcept {
    return WaitForAsyncResult<T>(
        std::forward<StartFn>(fn),
        timeoutMs,
        timeoutStatus,
        [cancel]() noexcept {
            return cancel != nullptr && cancel->load(std::memory_order_acquire);
        },
        pollIntervalMs);
}

/// Starts an async operation taking a `void(IOReturn)` callback and polls until
/// completion, predicate cancellation, or timeout.
template <typename StartFn, typename CancelFn>
requires std::invocable<CancelFn>
IOReturn WaitForAsyncStatus(StartFn&& fn,
                            uint32_t timeoutMs,
                            IOReturn timeoutStatus,
                            CancelFn&& isCancelled,
                            uint32_t pollIntervalMs = kWaitPollMs) noexcept {
    struct WaitState {
        std::atomic<bool> done{false};
        std::atomic<IOReturn> status{kIOReturnTimeout};
    };

    auto state = std::make_shared<WaitState>();
    fn([state](IOReturn status) {
        state->status.store(status, std::memory_order_release);
        state->done.store(true, std::memory_order_release);
    });

    const uint32_t pollMs = (pollIntervalMs == 0) ? kWaitPollMs : pollIntervalMs;
    for (uint32_t waited = 0; waited < timeoutMs; waited += pollMs) {
        if (state->done.load(std::memory_order_acquire)) {
            return state->status.load(std::memory_order_acquire);
        }
        if (isCancelled()) {
            return kIOReturnAborted;
        }
        IOSleep(pollMs);
    }

    if (state->done.load(std::memory_order_acquire)) {
        return state->status.load(std::memory_order_acquire);
    }

    return timeoutStatus;
}

/// Pointer-based overload of WaitForAsyncStatus for atomic cancel tokens.
template <typename StartFn>
IOReturn WaitForAsyncStatus(StartFn&& fn,
                            uint32_t timeoutMs,
                            IOReturn timeoutStatus,
                            const std::atomic<bool>* cancel = nullptr,
                            uint32_t pollIntervalMs = kWaitPollMs) noexcept {
    return WaitForAsyncStatus(
        std::forward<StartFn>(fn),
        timeoutMs,
        timeoutStatus,
        [cancel]() noexcept {
            return cancel != nullptr && cancel->load(std::memory_order_acquire);
        },
        pollIntervalMs);
}

} // namespace ASFW::Audio
