// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FamilyStageWait.hpp - Await a family's callback-driven stage as a FamilyDriver step.
//
// The BeBoB, Apogee and MOTU families still run their stages as chains of
// asynchronous bus, CMP and AV/C operations. Their FamilyDriver steps start the
// chain and wait here for its callback (AUDIO_SESSION_REDESIGN.md stage S5,
// structural). The session calls them off the Default queue, where the
// callbacks are delivered.

#pragma once

#include "../Backends/SyncAsyncBridge.hpp"

#include <DriverKit/IOReturn.h>

#include <atomic>
#include <cstdint>
#include <expected>
#include <utility>

namespace ASFW::Audio {

// Longest one stage may take. Every stage has its own bus-level timeouts well
// inside this; reaching it means a completion was lost.
inline constexpr uint32_t kFamilyStageTimeoutMs = 12000;

// Start a stage that answers `void(IOReturn, T)` and wait for it. Teardown
// (`cancel` reads true) gives up with kIOReturnAborted.
template <typename T, typename StartFn>
[[nodiscard]] std::expected<T, IOReturn> AwaitStage(StartFn&& start,
                                                    const std::atomic<bool>* cancel,
                                                    uint32_t timeoutMs = kFamilyStageTimeoutMs) noexcept {
    const SyncResult<T> result = WaitForAsyncResult<T>(std::forward<StartFn>(start), timeoutMs,
                                                       kIOReturnTimeout, cancel);
    if (result.status != kIOReturnSuccess) {
        return std::unexpected(result.status);
    }
    return result.value;
}

// Start a stage that answers `void(IOReturn)` and wait for it.
template <typename StartFn>
[[nodiscard]] IOReturn AwaitStageStatus(StartFn&& start, const std::atomic<bool>* cancel,
                                        uint32_t timeoutMs = kFamilyStageTimeoutMs) noexcept {
    return WaitForAsyncStatus(std::forward<StartFn>(start), timeoutMs, kIOReturnTimeout, cancel);
}

} // namespace ASFW::Audio
