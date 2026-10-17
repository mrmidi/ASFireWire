#pragma once

#include <functional>
#include <memory>
#include <utility>
#include "../Logging/Logging.hpp"
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

    // `label` names the cancellation in the [Lifecycle] trace, so a token
    // that never ends (and so a final Stop that never runs) is identifiable.
    // It must have static lifetime (callers pass string literals).
    [[nodiscard]] std::function<void()> Begin(const char* label = "unnamed") {
        IOLockLock(lock_);
        const size_t pending = ++pending_;
        IOLockUnlock(lock_);
        ASFW_LOG(Controller, "[Lifecycle] teardown token begin %{public}s pending=%zu", label, pending);
        return [self = shared_from_this(), label] { self->Complete(label); };
    }

    void FinishWhenDrained(std::function<void()> finish) {
        IOLockLock(lock_);
        finish_ = std::move(finish);
        const size_t pending = pending_;
        auto ready = pending_ == 0 ? std::move(finish_) : std::function<void()>{};
        IOLockUnlock(lock_);
        ASFW_LOG(Controller, "[Lifecycle] teardown drain requested pending=%zu", pending);
        if (ready) ready();
    }

private:
    void Complete(const char* label) {
        IOLockLock(lock_);
        const size_t pending = --pending_;
        auto ready = pending_ == 0 ? std::move(finish_) : std::function<void()>{};
        IOLockUnlock(lock_);
        ASFW_LOG(Controller, "[Lifecycle] teardown token end %{public}s pending=%zu%{public}s", label, pending,
                 ready ? " (running final stop)" : "");
        if (ready) ready();
    }
    IOLock* lock_;
    size_t pending_{0};
    std::function<void()> finish_;
};
} // namespace ASFW::Common
