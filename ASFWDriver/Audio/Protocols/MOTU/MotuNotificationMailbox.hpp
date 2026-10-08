// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include "MotuRegisters.hpp"
#include "../../../Discovery/DeviceRouteToken.hpp"
#include "../../../Async/Rx/LocalRequestDispatch.hpp"
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
    explicit NotificationMailbox(Discovery::DeviceRouteToken route) : route_(route) {}
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
    void Deliver(uint32_t generation, uint16_t source, uint32_t bits) {
        std::shared_ptr<ClockChangeWait> wait;
        lock_.Lock();
        if (route_ && route_.generation.value == generation &&
            (route_.nodeId & 0x3f) == (source & 0x3f) && (bits & 2)) wait = pending_;
        lock_.Unlock();
        if (wait) wait->Notify();
    }
private:
    ControlLock lock_;
    Discovery::DeviceRouteToken route_;
    std::shared_ptr<ClockChangeWait> pending_;
};

// Lifetime-owned slots, like EFC's mailbox. The dispatch holds only a weak
// reference; delivery upgrades it before touching state. No protocol pointer
// crosses the queues. Each slot owns one aligned quadlet in Linux's documented
// region (motu-transaction.c:37-67,95-121).
namespace Notifications {
inline constexpr size_t kSlots = 64;
inline auto& Slots() { static std::array<std::weak_ptr<NotificationMailbox>, kSlots> slots; return slots; }
inline auto& Lock() { static ControlLock lock; return lock; }
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
        held->Deliver(ctx.generation, ctx.sourceID, bits);
    }
    return Async::LocalRequestResult::Write(Async::ResponseCode::Complete);
}
} // namespace Notifications
} // namespace ASFW::Audio::Motu
