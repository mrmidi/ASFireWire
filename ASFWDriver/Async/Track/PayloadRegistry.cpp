#include "PayloadRegistry.hpp"

#ifdef ASFW_HOST_TEST
    #include <thread>
    #include <chrono>
    static inline void sleep_ms(uint32_t ms) { if (ms) std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
#else
    static inline void sleep_ms(uint32_t ms) { if (ms) IOSleep(ms); }
#endif

namespace ASFW::Async {

PayloadRegistry::PayloadRegistry() {
    lock_ = ::IOLockAlloc();
}

PayloadRegistry::~PayloadRegistry() {
    CancelAll(CancelMode::Synchronous);
    if (lock_) { ::IOLockFree(lock_); lock_ = nullptr; }
}

void PayloadRegistry::Attach(uint32_t handle, std::shared_ptr<void> payload, uint32_t epoch) {
    if (!lock_) return;
    ::IOLockLock(lock_);
    map_[handle] = Entry{ std::move(payload), epoch };
    ::IOLockUnlock(lock_);
}

std::shared_ptr<void> PayloadRegistry::Detach(uint32_t handle) {
    if (!lock_) return nullptr;
    ::IOLockLock(lock_);
    auto it = map_.find(handle);
    if (it == map_.end()) { ::IOLockUnlock(lock_); return nullptr; }
    auto p = it->second.payload;
    map_.erase(it);
    ::IOLockUnlock(lock_);
    return p;
}

void PayloadRegistry::CancelAll(CancelMode mode) {
    if (!lock_) return;
    ::IOLockLock(lock_);
    map_.clear();
    ::IOLockUnlock(lock_);
    // For synchronous mode, do a bounded wait loop to allow any async consumers
    // to observe the cleared state. In DriverKit we avoid condition variables.
    if (mode == CancelMode::Synchronous) {
        // small sleep to allow background work to drain
        sleep_ms(10);
    }
}

void PayloadRegistry::CancelByEpoch(uint32_t epoch, CancelMode mode) {
    if (!lock_) return;
    ::IOLockLock(lock_);
    for (auto it = map_.begin(); it != map_.end(); ) {
        if (it->second.epoch <= epoch) {
            it = map_.erase(it);
        } else {
            ++it;
        }
    }
    ::IOLockUnlock(lock_);
    if (mode == CancelMode::Synchronous) {
        sleep_ms(10);
    }
}

bool PayloadRegistry::Drain(uint32_t timeoutMs) {
    if (!lock_) return true;
    const uint32_t stepMs = 5;
    uint32_t waited = 0;
    while (true) {
        ::IOLockLock(lock_);
        bool empty = map_.empty();
        ::IOLockUnlock(lock_);
        if (empty) return true;
        if (waited >= timeoutMs) return false;
        sleep_ms(stepMs);
        waited += stepMs;
    }
}

void PayloadRegistry::SetEpoch(uint32_t epoch) {
    if (!lock_) return;
    ::IOLockLock(lock_);
    epoch_ = epoch;
    ::IOLockUnlock(lock_);
}

uint32_t PayloadRegistry::GetEpoch() const {
    if (!lock_) return epoch_;
    ::IOLockLock(lock_);
    uint32_t v = epoch_;
    ::IOLockUnlock(lock_);
    return v;
}

} // namespace ASFW::Async
