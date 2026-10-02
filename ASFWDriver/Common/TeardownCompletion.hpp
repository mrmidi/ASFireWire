#pragma once

#include <functional>
#include <memory>
#include <utility>
#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOLib.h>
#endif

namespace ASFW::Common {

// Service-lifetime fence: source/action cancellations can begin before Stop
// (provider revocation or suspend). Invoke final Stop only after all finish.
// Never wait synchronously on the service queue: it delivers the completions.
class TeardownCompletion final : public std::enable_shared_from_this<TeardownCompletion> {
public:
    TeardownCompletion() : lock_(IOLockAlloc()) {}
    ~TeardownCompletion() { IOLockFree(lock_); }
    TeardownCompletion(const TeardownCompletion&) = delete;
    TeardownCompletion& operator=(const TeardownCompletion&) = delete;

    [[nodiscard]] std::function<void()> Begin() {
        IOLockLock(lock_);
        ++pending_;
        IOLockUnlock(lock_);
        return [self = shared_from_this()] { self->Complete(); };
    }

    void FinishWhenDrained(std::function<void()> finish) {
        IOLockLock(lock_);
        finish_ = std::move(finish);
        auto ready = pending_ == 0 ? std::move(finish_) : std::function<void()>{};
        IOLockUnlock(lock_);
        if (ready) ready();
    }

private:
    void Complete() {
        IOLockLock(lock_);
        --pending_;
        auto ready = pending_ == 0 ? std::move(finish_) : std::function<void()>{};
        IOLockUnlock(lock_);
        if (ready) ready();
    }
    IOLock* lock_;
    size_t pending_{0};
    std::function<void()> finish_;
};
} // namespace ASFW::Common
