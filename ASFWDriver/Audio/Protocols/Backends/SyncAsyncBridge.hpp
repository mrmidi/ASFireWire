// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SyncAsyncBridge.hpp
//
// A reusable synchronous/asynchronous bridge (FW-65). WaitForAsyncResult starts an
// async operation and blocks the calling thread, polling a shared completion flag until
// the operation completes, a teardown cancel token fires (FW-61), or a timeout elapses.
//
// The definition bodies are copied verbatim from AudioDuplexCoordinator.cpp (only the
// linkage is upgraded — anonymous-namespace `constexpr` to header `inline constexpr`) so it
// can be reused by both the DICE duplex path and (later) the AV/C path. Behaviour is
// intentionally unchanged: same 10 ms poll interval, same cancel-token semantics, same
// timeout mapping. This move must not alter any observable behaviour.

#pragma once

#include <DriverKit/IOLib.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace ASFW::Audio {

// Poll interval for the blocking bridge, in milliseconds. Kept identical to the value the
// helper used inside the coordinator; do not change without a behaviour review. `inline`
// so the single definition is shared across every TU that includes this header.
//
// Note: this is distinct from AudioDuplexCoordinator's own `kSyncBridgePollMs`, which
// paces the coordinator's separate GUID/clock polling loops. The two are independent
// constants that happen to both be 10 ms — do not unify them without a behaviour review.
inline constexpr uint32_t kWaitPollMs = 10;

template <typename T>
struct SyncResult {
    IOReturn status{kIOReturnTimeout};
    T value{};
};

template <typename T, typename StartFn>
SyncResult<T> WaitForAsyncResult(StartFn&& fn,
                                 uint32_t timeoutMs,
                                 IOReturn timeoutStatus,
                                 const std::atomic<bool>* cancel = nullptr) noexcept {
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

    for (uint32_t waited = 0; waited < timeoutMs; waited += kWaitPollMs) {
        if (state->done.load(std::memory_order_acquire)) {
            return state->result;
        }
        // FW-61: abort the blocking bridge promptly on teardown so the dice queue drains
        // fast (instead of running to timeout) and the in-flight op issues no more MMIO.
        // The completion that would set `done` is delivered on the core queue, which is
        // blocked in DispatchSync during teardown, so cancel, not done, is what unblocks us.
        if (cancel != nullptr && cancel->load(std::memory_order_acquire)) {
            SyncResult<T> aborted{};
            aborted.status = kIOReturnAborted;
            return aborted;
        }
        IOSleep(kWaitPollMs);
    }

    if (state->done.load(std::memory_order_acquire)) {
        return state->result;
    }

    SyncResult<T> timeout{};
    timeout.status = timeoutStatus;
    return timeout;
}

} // namespace ASFW::Audio
