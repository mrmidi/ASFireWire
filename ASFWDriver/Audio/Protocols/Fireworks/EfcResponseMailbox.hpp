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
// The AR dispatch path that calls Publish() and the queue that drops a protocol
// are not the same context, so an observer can be torn down while a response is
// being delivered to it (see FW-60 for what raw cross-queue pointers cost).
//
// That is handled by ownership rather than by waiting. A slot holds a weak_ptr;
// Publish() upgrades it to a shared_ptr for exactly as long as the callback
// runs, so an observer already being delivered to cannot be destroyed underneath
// it — destruction simply happens when that reference drops. An observer torn
// down before Publish() reaches it fails the upgrade and is skipped. There is no
// window between those two states, which is why there is no unregister step, no
// in-flight counter and no wait:
//
//   - A previous version cleared the slot and then spun on an in-flight counter
//     with IODelay(50) up to 2000 times — 100 ms of busy-wait on whichever queue
//     tore the transport down — and, on timeout, logged and freed the observer
//     anyway. That was a use-after-free with a bounded delay in front of it.
//   - Shrinking the bound could not fix it: the window spans the caller-supplied
//     completion function, so a shorter timeout converts waits that currently
//     succeed into use-after-frees.
//
// Slots are reclaimed automatically: a slot whose weak_ptr has expired is free,
// so a transport's destructor does not touch the mailbox at all.

#pragma once

#include "EfcProtocol.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace ASFW::Audio::Fireworks {

/// What the mailbox delivers to. Kept abstract so the mailbox does not depend on
/// EfcTransport, and so Publish() can stay inline in this header.
class IEfcResponseObserver {
public:
    virtual ~IEfcResponseObserver() = default;

    /// Returns true when this observer consumed the frame (its sequence number
    /// matched). Called on the AR dispatch path with the observer held alive for
    /// the duration of the call.
    [[nodiscard]] virtual bool OnEfcResponse(uint16_t sourceID,
                                             std::span<const uint8_t> payload) = 0;
};

} // namespace ASFW::Audio::Fireworks

namespace ASFW::Audio::Fireworks::EfcResponseMailbox {

inline constexpr uint64_t kWindowOffset = Efc::kResponseOffset;
inline constexpr uint64_t kAltWindowOffset = Efc::kAltResponseOffset;
inline constexpr uint64_t kWindowBytes = Efc::kResponseWindowBytes;
inline constexpr size_t kMaxObservers = 4;

// Slot access is guarded by a spin gate rather than an atomic weak_ptr, because
// std::atomic<std::weak_ptr<T>> is not implemented in this libc++ (the primary
// template demands a trivially copyable T). The gate is held only long enough to
// copy the slots -- never across a callback -- so it is nanoseconds of
// contention on a control-plane path, not the blocking wait this replaced.
struct Slot {
    std::weak_ptr<IEfcResponseObserver> observer{};  // guarded by Gate()
};

inline std::array<Slot, kMaxObservers>& Slots() noexcept {
    static std::array<Slot, kMaxObservers> slots{};
    return slots;
}

inline std::atomic_flag& Gate() noexcept {
    static std::atomic_flag gate = ATOMIC_FLAG_INIT;
    return gate;
}

class ScopedGate final {
public:
    ScopedGate() noexcept {
        while (Gate().test_and_set(std::memory_order_acquire)) {
            // Slots are only touched to copy or assign four weak_ptrs, so the
            // holder is never descheduled here in practice.
        }
    }
    ~ScopedGate() { Gate().clear(std::memory_order_release); }

    ScopedGate(const ScopedGate&) = delete;
    ScopedGate& operator=(const ScopedGate&) = delete;
};

[[nodiscard]] inline bool MatchesDestOffset(uint64_t destOffset) noexcept {
    // Both the Linux-documented default window and the one a real Onyx 400F
    // uses (see EfcProtocol.hpp, kAltResponseOffset).
    return (destOffset >= kWindowOffset && destOffset < kWindowOffset + kWindowBytes) ||
           (destOffset >= kAltWindowOffset && destOffset < kAltWindowOffset + kWindowBytes);
}

/// Claim a slot for `observer`. Slots whose observer has expired are free, so a
/// dead transport's slot is reused without anyone having to release it.
[[nodiscard]] inline bool Register(
    const std::shared_ptr<IEfcResponseObserver>& observer) noexcept {
    if (!observer) {
        return false;
    }
    const ScopedGate guard;
    for (auto& slot : Slots()) {
        if (slot.observer.expired()) {
            slot.observer = observer;
            return true;
        }
    }
    return false;
}

/// Offer a response frame to every live observer; true when one claimed it.
///
/// The upgrade is the whole safety argument: while `held` is alive the observer
/// cannot be destroyed, so the callback can never run against freed memory. An
/// observer torn down before we reach it fails the upgrade and is skipped, and
/// there is no window between those two states.
[[nodiscard]] inline bool Publish(uint16_t sourceID,
                                  std::span<const uint8_t> payload) noexcept {
    std::array<std::weak_ptr<IEfcResponseObserver>, kMaxObservers> snapshot{};
    {
        const ScopedGate guard;
        for (size_t i = 0; i < kMaxObservers; ++i) {
            snapshot[i] = Slots()[i].observer;
        }
    }
    // Callbacks run outside the gate: they end in caller-supplied completions,
    // which must never execute with a lock held.
    for (auto& weak : snapshot) {
        const std::shared_ptr<IEfcResponseObserver> held = weak.lock();
        if (!held) {
            continue;
        }
        if (held->OnEfcResponse(sourceID, payload)) {
            return true;
        }
    }
    return false;
}

/// Live observers, i.e. slots whose weak_ptr has not expired.
[[nodiscard]] inline size_t ObserverCount() noexcept {
    const ScopedGate guard;
    size_t live = 0;
    for (auto& slot : Slots()) {
        if (!slot.observer.expired()) {
            ++live;
        }
    }
    return live;
}

} // namespace ASFW::Audio::Fireworks::EfcResponseMailbox
