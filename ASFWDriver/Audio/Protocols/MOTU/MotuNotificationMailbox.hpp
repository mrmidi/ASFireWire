// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include "MotuRegisters.hpp"
#include "../../../Discovery/DeviceRouteToken.hpp"
#include "../../../Async/Rx/LocalRequestDispatch.hpp"
#include "../../../Logging/Logging.hpp"
#include <DriverKit/IOLib.h>
#include <array>
#include <functional>
#include <memory>
#include <utility>

namespace ASFW::Audio::Motu {
// Control-plane locks only. No callback runs while a lock is held.
class ControlLock final {
public:
    ControlLock() noexcept : lock_(IOLockAlloc()) {}
    ~ControlLock() { if (lock_) IOLockFree(lock_); }
    ControlLock(const ControlLock&) = delete;
    ControlLock& operator=(const ControlLock&) = delete;
    [[nodiscard]] bool Valid() const noexcept { return lock_ != nullptr; }
    void Lock() noexcept { IOLockLock(lock_); }
    void Unlock() noexcept { IOLockUnlock(lock_); }
private:
    IOLock* lock_;
};

// A clock change needs both a successful write and a CLK_CHANGED message.
// A message may precede the write response. Timeout/cancel/error complete once.
class ClockChangeWait final {
public:
    explicit ClockChangeWait(std::function<void(IOReturn)> callback)
        : callback_(std::move(callback)) {}
    [[nodiscard]] bool Valid() const noexcept { return lock_.Valid(); }
    void Ack(IOReturn status) { Advance(status, true, false); }
    void Notify() { Advance(kIOReturnSuccess, false, true); }
    void Fail(IOReturn status) { Advance(status, false, false); }
    [[nodiscard]] bool Done() {
        lock_.Lock(); const bool done = done_; lock_.Unlock(); return done;
    }
private:
    void Advance(IOReturn status, bool ack, bool notify) {
        std::function<void(IOReturn)> callback;
        lock_.Lock();
        if (!done_) {
            ack_ |= ack; notified_ |= notify;
            if (status != kIOReturnSuccess || (ack_ && notified_)) {
                done_ = true; callback = std::move(callback_);
            }
        }
        lock_.Unlock();
        if (callback) callback(status);
    }
    ControlLock lock_;
    std::function<void(IOReturn)> callback_;
    bool ack_{false}, notified_{false}, done_{false};
};

class NotificationMailbox final {
public:
    /// What one notification did. `accepted`: it came from this device on its
    /// current route. `claimedByWait`: it completed a clock change the host
    /// asked for. `clockChanged`: the model names a clock-changed bit
    /// (`clockChangedMask`) and it is set.
    struct Delivery {
        bool accepted{false};
        bool claimedByWait{false};
        bool clockChanged{false};
        uint64_t guid{0};
    };

    /// `clockChangedMask`: the bits this model's firmware sets when its clock
    /// changed. V3 names one, CLK_CHANGED 0x2 (Linux motu-protocol-v3.c:40,
    /// V3_MSG_FLAG_CLK_CHANGED); V1/V2 name none, so theirs is 0.
    explicit NotificationMailbox(Discovery::DeviceRouteToken route, uint32_t clockChangedMask = 0x2)
        : route_(route), clockChangedMask_(clockChangedMask) {}
    [[nodiscard]] bool Valid() const noexcept { return lock_.Valid(); }
    void UpdateRoute(Discovery::DeviceRouteToken route) {
        std::shared_ptr<ClockChangeWait> old;
        lock_.Lock();
        if (route_ == route) { lock_.Unlock(); return; }
        route_ = route; old = std::exchange(pending_, {}); lock_.Unlock();
        if (old) old->Fail(kIOReturnAborted);
    }
    void Cancel() { UpdateRoute({}); }
    [[nodiscard]] std::shared_ptr<ClockChangeWait> Begin(std::function<void(IOReturn)> callback) {
        auto wait = std::make_shared<ClockChangeWait>(std::move(callback));
        if (!wait->Valid()) return {};
        lock_.Lock();
        if (!route_ || (pending_ && !pending_->Done())) { lock_.Unlock(); return {}; }
        pending_ = wait;
        lock_.Unlock();
        return wait;
    }
    Delivery Deliver(uint32_t generation, uint16_t source, uint32_t bits) {
        Delivery out{};
        std::shared_ptr<ClockChangeWait> wait;
        lock_.Lock();
        if (route_ && route_.generation.value == generation &&
            (route_.nodeId & 0x3f) == (source & 0x3f)) {
            out.accepted = true;
            out.guid = route_.guid;
            out.clockChanged = (bits & clockChangedMask_) != 0;
            if ((bits & 2) && pending_ && !pending_->Done()) wait = pending_;
        }
        lock_.Unlock();
        if (wait) {
            wait->Notify();
            out.claimedByWait = true;
        }
        return out;
    }
private:
    ControlLock lock_;
    Discovery::DeviceRouteToken route_;
    std::shared_ptr<ClockChangeWait> pending_;
    uint32_t clockChangedMask_{0x2};
};

// Lifetime-owned slots, like EFC's mailbox. The dispatch holds only a weak
// reference; delivery upgrades it before touching state. No protocol pointer
// crosses the queues. Each slot owns one aligned quadlet in Linux's documented
// region (motu-transaction.c:37-67,95-121).
namespace Notifications {
inline constexpr size_t kSlots = 64;
inline auto& Slots() { static std::array<std::weak_ptr<NotificationMailbox>, kSlots> slots; return slots; }
inline auto& Lock() { static ControlLock lock; return lock; }

/// One observer for notifications no host-requested wait claimed (the MOTU
/// family adapter). Called with the observer lock held, so once ClearObserver
/// returns no call is running or will start -- the DiceNotificationRouter
/// contract, and the one exception to this file's "no callback under a lock".
using Observer = void (*)(void* context, uint64_t guid, uint32_t bits, bool clockChanged);
struct ObserverSlot {
    void* context{nullptr};
    Observer observer{nullptr};
};
inline auto& ObserverLock() { static ControlLock lock; return lock; }
inline auto& CurrentObserver() { static ObserverSlot slot; return slot; }
inline void SetObserver(void* context, Observer observer) {
    auto& lock = ObserverLock();
    if (!lock.Valid()) return;
    lock.Lock(); CurrentObserver() = {context, observer}; lock.Unlock();
}
inline void ClearObserver(void* context) {
    auto& lock = ObserverLock();
    if (!lock.Valid()) return;
    lock.Lock();
    if (CurrentObserver().context == context) CurrentObserver() = {};
    lock.Unlock();
}
[[nodiscard]] inline uint64_t Register(const std::shared_ptr<NotificationMailbox>& mailbox) {
    auto& lock = Lock();
    if (!lock.Valid() || !mailbox || !mailbox->Valid()) return 0;
    lock.Lock();
    for (size_t i = 0; i < kSlots; ++i) if (Slots()[i].expired()) {
        Slots()[i] = mailbox; lock.Unlock(); return kAsyncMessageRegionStart + i * 4;
    }
    lock.Unlock(); return 0;
}
[[nodiscard]] inline Async::LocalRequestResult Handle(const Async::LocalRequestContext& ctx) {
    const auto offset = ctx.destOffset;
    if (offset < kAsyncMessageRegionStart || offset >= kAsyncMessageRegionStart + kSlots * 4)
        return Async::LocalRequestResult::NotMine();
    if (ctx.tCode != 0) return Async::LocalRequestResult::Write(Async::ResponseCode::Complete);
    if (offset & 3) return Async::LocalRequestResult::Write(Async::ResponseCode::AddressError);
    if (ctx.writePayload.size() != 4) return Async::LocalRequestResult::Write(Async::ResponseCode::TypeError);
    auto& lock = Lock();
    if (!lock.Valid()) return Async::LocalRequestResult::Write(Async::ResponseCode::AddressError);
    lock.Lock(); const auto held = Slots()[(offset - kAsyncMessageRegionStart) / 4].lock(); lock.Unlock();
    if (held) {
        const auto bytes = ctx.writePayload;
        const uint32_t bits = (uint32_t{bytes[0]} << 24) | (uint32_t{bytes[1]} << 16) |
                              (uint32_t{bytes[2]} << 8) | bytes[3];
        const auto delivery = held->Deliver(ctx.generation, ctx.sourceID, bits);
        if (delivery.accepted && !delivery.claimedByWait) {
            // Unsolicited: the device changed something on its own (front
            // panel, external clock). Rare, so logged every time. Only a named
            // clock-changed bit becomes a host event; other bits stay unnamed
            // until traced (tmp/motu-research/vendor-controller-followup.md).
            ASFW_LOG(Audio, "[MotuNotify] guid=%016llx bits=0x%08x unsolicited clockChanged=%u",
                     delivery.guid, bits, delivery.clockChanged ? 1U : 0U);
            auto& observerLock = ObserverLock();
            if (observerLock.Valid()) {
                observerLock.Lock();
                const auto slot = CurrentObserver();
                if (slot.observer) slot.observer(slot.context, delivery.guid, bits, delivery.clockChanged);
                observerLock.Unlock();
            }
        }
    }
    return Async::LocalRequestResult::Write(Async::ResponseCode::Complete);
}
} // namespace Notifications
} // namespace ASFW::Audio::Motu
