// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../../Discovery/DeviceRouteToken.hpp"
#include <DriverKit/IOLib.h>
#include <algorithm>
#include <array>
#include <cstdint>

namespace ASFW::Protocols::AVC {

/// A confirmed CURRENT value, published after the immutable graph is replaced.
/// Native AV/C facts only; Audio/Controls projects the semantic parameter id.
struct FeatureChange {
    Discovery::DeviceRouteToken route;
    uint64_t revision{0};
    uint64_t sequence{0};
    uint8_t subunit{0}, block{0}, channel{0};
    bool mute{false};
    int32_t value{0}; // 0/1 or signed 1/256 dB
};

/// Control-plane fan-out. Mirrors DiceNotificationRouter's drain-on-clear
/// contract: Unregister waits for any callback and prevents future calls.
/// Callbacks run under this lock and MUST only enqueue one-way notifications:
/// no synchronous queue dispatch, hardware I/O, or registry re-entry.
class FeatureChangeNotifications final {
public:
    using Callback = void (*)(void*, const FeatureChange&);
    static FeatureChangeNotifications& Shared() {
        static FeatureChangeNotifications notifications;
        return notifications;
    }
    FeatureChangeNotifications() : lock_(IOLockAlloc()) {}
    ~FeatureChangeNotifications() { if (lock_) IOLockFree(lock_); }
    FeatureChangeNotifications(const FeatureChangeNotifications&) = delete;
    FeatureChangeNotifications& operator=(const FeatureChangeNotifications&) = delete;

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
    void Publish(FeatureChange change) {
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
} // namespace ASFW::Protocols::AVC
