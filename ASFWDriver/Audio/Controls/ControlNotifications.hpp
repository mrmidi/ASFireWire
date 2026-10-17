// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AvcControlChange.hpp"
#include <DriverKit/IOLib.h>
#include <algorithm>
#include <array>
namespace ASFW::Audio::Controls {
/// Control-plane fan-out. Mirrors DiceNotificationRouter's drain-on-clear
/// contract: Unregister waits for any callback and prevents future calls.
/// Callbacks run under this lock and MUST only enqueue one-way notifications:
/// no synchronous queue dispatch, hardware I/O, or registry re-entry.
class ControlNotifications final {
public:
    using Callback = void (*)(void*, const ParameterChanged&);
    static ControlNotifications& Shared() {
        static ControlNotifications notifications;
        return notifications;
    }
    ControlNotifications() : lock_(IOLockAlloc()) {}
    ~ControlNotifications() { if (lock_) IOLockFree(lock_); }
    ControlNotifications(const ControlNotifications&) = delete;
    ControlNotifications& operator=(const ControlNotifications&) = delete;

    [[nodiscard]] bool Register(void* owner, Callback callback) {
        if (!lock_ || !owner || !callback) return false;
        IOLockLock(lock_);
        auto slot = std::ranges::find_if(slots_, [owner](const auto& s) { return s.owner == owner; });
        if (slot == slots_.end()) slot = std::ranges::find(slots_, nullptr, &Slot::owner);
        const bool available = slot != slots_.end();
        if (available) *slot = {owner, callback};
        IOLockUnlock(lock_);
        return available;
    }
    void Unregister(void* owner) {
        if (!lock_) return;
        IOLockLock(lock_);
        for (auto& slot : slots_) if (slot.owner == owner) slot = {};
        IOLockUnlock(lock_);
    }
    [[nodiscard]] uint64_t NextSequence() {
        if (!lock_) return 0;
        IOLockLock(lock_);
        const auto sequence = ++sequence_;
        IOLockUnlock(lock_);
        return sequence;
    }
    void Publish(ParameterChanged change) {
        if (!lock_) return;
        IOLockLock(lock_);
        if (!change.sequence) change.sequence = ++sequence_;
        for (const auto& slot : slots_) if (slot.callback) slot.callback(slot.owner, change);
        IOLockUnlock(lock_);
    }
private:
    struct Slot { void* owner{nullptr}; Callback callback{nullptr}; };
    IOLock* lock_;
    std::array<Slot, 64> slots_{};
    uint64_t sequence_{0};
};
} // namespace ASFW::Audio::Controls
