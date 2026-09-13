// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// EfcResponseMailbox.hpp — Host-side landing zone for Fireworks EFC responses.
//
// A Fireworks device answers an EFC command by block-writing the response
// frame to 0xECC0'8000'0000 in the *host's* address space. The local request
// dispatch (Service/LocalRequestWiring.cpp) claims that window and hands the
// payload here; every live EfcTransport registers itself as an observer and
// claims the frames whose sequence number it is waiting for.
//
// Same shape as DICE::NotificationMailbox: header-only, lock-free, a fixed
// number of observer slots (one per Fireworks device on the bus).

#pragma once

#include "EfcProtocol.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ASFW::Audio::Fireworks::EfcResponseMailbox {

inline constexpr uint64_t kWindowOffset = Efc::kResponseOffset;
inline constexpr uint64_t kWindowBytes = Efc::kResponseWindowBytes;
inline constexpr size_t kMaxObservers = 4;

// Returns true when the observer consumed the frame (its seqnum matched).
using ObserverFn = bool (*)(void* context, uint16_t sourceID, std::span<const uint8_t> payload);

struct Slot {
    std::atomic<void*> context{nullptr};
    std::atomic<ObserverFn> fn{nullptr};
};

inline std::array<Slot, kMaxObservers>& Slots() noexcept {
    static std::array<Slot, kMaxObservers> slots{};
    return slots;
}

[[nodiscard]] inline bool MatchesDestOffset(uint64_t destOffset) noexcept {
    return destOffset >= kWindowOffset && destOffset < kWindowOffset + kWindowBytes;
}

[[nodiscard]] inline bool AddObserver(void* context, ObserverFn fn) noexcept {
    if (context == nullptr || fn == nullptr) {
        return false;
    }
    for (auto& slot : Slots()) {
        void* expected = nullptr;
        if (slot.context.compare_exchange_strong(expected, context, std::memory_order_acq_rel)) {
            slot.fn.store(fn, std::memory_order_release);
            return true;
        }
    }
    return false;
}

inline void RemoveObserver(void* context) noexcept {
    for (auto& slot : Slots()) {
        if (slot.context.load(std::memory_order_acquire) == context) {
            slot.fn.store(nullptr, std::memory_order_release);
            slot.context.store(nullptr, std::memory_order_release);
        }
    }
}

[[nodiscard]] inline size_t ObserverCount() noexcept {
    size_t n = 0;
    for (auto& slot : Slots()) {
        if (slot.context.load(std::memory_order_acquire) != nullptr) {
            ++n;
        }
    }
    return n;
}

// Offer a response frame to every observer; true when one of them claimed it.
[[nodiscard]] inline bool Publish(uint16_t sourceID, std::span<const uint8_t> payload) noexcept {
    for (auto& slot : Slots()) {
        void* context = slot.context.load(std::memory_order_acquire);
        ObserverFn fn = slot.fn.load(std::memory_order_acquire);
        if (context == nullptr || fn == nullptr) {
            continue;
        }
        if (fn(context, sourceID, payload)) {
            return true;
        }
    }
    return false;
}

} // namespace ASFW::Audio::Fireworks::EfcResponseMailbox
