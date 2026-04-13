// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// DICENotificationMailbox.hpp - Shared mailbox for DICE async notifications

#pragma once

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::DICE::NotificationMailbox {

/// Reference saffire.kext uses the local FW notification address for DICE notify quadlets.
constexpr uint64_t kHandlerOffset = 0x000100000000ULL;
/// Legacy ASFW software-latch address kept for diagnostics/backward compatibility.
constexpr uint64_t kLegacyHandlerOffset = 0x00FF0000D1CCULL;

inline std::atomic<uint32_t> gLatchedBits{0};
using ObserverFn = void(*)(void* context, uint32_t bits);
inline std::atomic<void*> gObserverContext{nullptr};
inline std::atomic<ObserverFn> gObserver{nullptr};

/// Reset any latched notification bits.
inline void Reset() noexcept {
    gLatchedBits.store(0, std::memory_order_release);
}

/// Latch notification bits observed from device writes.
inline void Publish(uint32_t bits) noexcept {
    gLatchedBits.fetch_or(bits, std::memory_order_acq_rel);
    if (const auto observer = gObserver.load(std::memory_order_acquire)) {
        observer(gObserverContext.load(std::memory_order_acquire), bits);
    }
}

inline void SetObserver(void* context, ObserverFn observer) noexcept {
    gObserverContext.store(context, std::memory_order_release);
    gObserver.store(observer, std::memory_order_release);
}

inline void ClearObserver(void* context) noexcept {
    if (gObserverContext.load(std::memory_order_acquire) != context) {
        return;
    }

    gObserver.store(nullptr, std::memory_order_release);
    gObserverContext.store(nullptr, std::memory_order_release);
}

[[nodiscard]] inline bool MatchesDestOffset(uint64_t destOffset) noexcept {
    return destOffset == kHandlerOffset || destOffset == kLegacyHandlerOffset;
}

[[nodiscard]] inline uint32_t DecodeWireQuadlet(const uint8_t* data) noexcept {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

[[nodiscard]] inline uint32_t PublishWireQuadlet(const uint8_t* data) noexcept {
    const uint32_t bits = DecodeWireQuadlet(data);
    Publish(bits);
    return bits;
}

/// Snapshot currently latched bits without clearing them.
[[nodiscard]] inline uint32_t Snapshot() noexcept {
    return gLatchedBits.load(std::memory_order_acquire);
}

/// Consume and clear all currently latched bits.
[[nodiscard]] inline uint32_t Consume() noexcept {
    return gLatchedBits.exchange(0, std::memory_order_acq_rel);
}

} // namespace ASFW::Audio::DICE::NotificationMailbox
