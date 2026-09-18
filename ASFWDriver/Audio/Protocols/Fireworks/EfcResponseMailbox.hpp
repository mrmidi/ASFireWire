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
// Same shape as DICE::NotificationMailbox (header-only, atomics, a fixed number
// of observer slots), plus a per-slot in-flight counter so RemoveObserver()
// cannot return while Publish() is still inside that observer on another
// queue — the AR dispatch path and the queue dropping a protocol are not the
// same context (see FW-60 for what raw cross-queue pointers cost).

#pragma once

#include "EfcProtocol.hpp"

#include <DriverKit/IOLib.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ASFW::Audio::Fireworks::EfcResponseMailbox {

inline constexpr uint64_t kWindowOffset = Efc::kResponseOffset;
inline constexpr uint64_t kAltWindowOffset = Efc::kAltResponseOffset;
inline constexpr uint64_t kWindowBytes = Efc::kResponseWindowBytes;
inline constexpr size_t kMaxObservers = 4;

// Bounded wait for an in-flight Publish() to leave a slot being removed.
inline constexpr uint32_t kRemoveWaitStepUs = 50;
inline constexpr uint32_t kRemoveWaitMaxSteps = 2000;  // 100 ms

// Returns true when the observer consumed the frame (its seqnum matched).
using ObserverFn = bool (*)(void* context, uint16_t sourceID, std::span<const uint8_t> payload);

struct Slot {
    std::atomic<void*> context{nullptr};
    std::atomic<ObserverFn> fn{nullptr};
    std::atomic<uint32_t> inflight{0};
};

inline std::array<Slot, kMaxObservers>& Slots() noexcept {
    static std::array<Slot, kMaxObservers> slots{};
    return slots;
}

[[nodiscard]] inline bool MatchesDestOffset(uint64_t destOffset) noexcept {
    // Both the Linux-documented default window and the one a real Onyx 400F
    // uses (see EfcProtocol.hpp, kAltResponseOffset).
    return (destOffset >= kWindowOffset && destOffset < kWindowOffset + kWindowBytes) ||
           (destOffset >= kAltWindowOffset && destOffset < kAltWindowOffset + kWindowBytes);
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

// Clears the slot and waits (bounded) until no Publish() is still executing
// inside the observer. Returns false if the wait timed out; the caller should
// then treat its object as possibly still referenced and log it.
inline bool RemoveObserver(void* context) noexcept {
    bool quiesced = true;
    for (auto& slot : Slots()) {
        if (slot.context.load(std::memory_order_acquire) != context) {
            continue;
        }
        slot.fn.store(nullptr, std::memory_order_release);
        slot.context.store(nullptr, std::memory_order_release);
        uint32_t steps = 0;
        while (slot.inflight.load(std::memory_order_acquire) != 0) {
            if (++steps > kRemoveWaitMaxSteps) {
                quiesced = false;
                break;
            }
            IODelay(kRemoveWaitStepUs);
        }
    }
    return quiesced;
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
        slot.inflight.fetch_add(1, std::memory_order_acq_rel);
        // Re-validate after publishing our presence: a concurrent RemoveObserver
        // that cleared the slot before the increment must not see us call in.
        const bool stillOwned = slot.context.load(std::memory_order_acquire) == context;
        const bool claimed = stillOwned && fn(context, sourceID, payload);
        slot.inflight.fetch_sub(1, std::memory_order_acq_rel);
        if (claimed) {
            return true;
        }
    }
    return false;
}

} // namespace ASFW::Audio::Fireworks::EfcResponseMailbox
