#include "DriverKitSessionScheduler.hpp"

#include "../../../Common/TimingUtils.hpp"
#include "../../../Logging/Logging.hpp"

#ifndef ASFW_HOST_TEST
#include <net.mrmidi.ASFW.ASFWDriver/ASFWDriver.h>
#endif

#include <algorithm>
#include <utility>

namespace ASFW::Protocols::SBP2 {

namespace {

class IOLockGuard {
public:
    explicit IOLockGuard(IOLock* lock) : lock_(lock) {
        if (lock_) {
            IOLockLock(lock_);
        }
    }

    ~IOLockGuard() {
        if (lock_) {
            IOLockUnlock(lock_);
        }
    }

    IOLockGuard(const IOLockGuard&) = delete;
    IOLockGuard& operator=(const IOLockGuard&) = delete;

private:
    IOLock* lock_{nullptr};
};

} // namespace

DriverKitSessionScheduler::DriverKitSessionScheduler() {
    lock_ = IOLockAlloc();
}

DriverKitSessionScheduler::~DriverKitSessionScheduler() {
    Reset();
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

kern_return_t DriverKitSessionScheduler::Prepare(::ASFWDriver& service,
                                                 OSSharedPtr<IODispatchQueue> workQueue) {
    if (!workQueue) {
        return kIOReturnNotReady;
    }

    Reset();
    workQueue_ = std::move(workQueue);

#ifdef ASFW_HOST_TEST
    (void)service;
    return kIOReturnSuccess;
#else
    IOTimerDispatchSource* rawTimer = nullptr;
    auto kr = IOTimerDispatchSource::Create(workQueue_.get(), &rawTimer);
    if (kr != kIOReturnSuccess || rawTimer == nullptr) {
        workQueue_.reset();
        return kr != kIOReturnSuccess ? kr : kIOReturnNoResources;
    }
    timer_ = OSSharedPtr(rawTimer, OSNoRetain);

    OSAction* rawAction = nullptr;
    kr = service.CreateActionSBP2SessionTimerFired(0, &rawAction);
    if (kr != kIOReturnSuccess || rawAction == nullptr) {
        timer_.reset();
        workQueue_.reset();
        return kr != kIOReturnSuccess ? kr : kIOReturnError;
    }
    action_ = OSSharedPtr(rawAction, OSNoRetain);

    kr = timer_->SetHandler(action_.get());
    if (kr != kIOReturnSuccess) {
        Reset();
        return kr;
    }

    kr = timer_->SetEnableWithCompletion(true, nullptr);
    if (kr != kIOReturnSuccess) {
        Reset();
        return kr;
    }

    (void)ASFW::Timing::initializeHostTimebase();
    return kIOReturnSuccess;
#endif
}

void DriverKitSessionScheduler::Reset() noexcept {
    {
        IOLockGuard guard(lock_);
        pending_.clear();
    }

    if (timer_) {
        (void)timer_->SetEnableWithCompletion(false, nullptr);
    }
    action_.reset();
    timer_.reset();
    workQueue_.reset();
}

SchedulerToken DriverKitSessionScheduler::ScheduleAfter(uint64_t delayNs,
                                                        std::function<void()> fn) {
    if (!fn) {
        return kInvalidSchedulerToken;
    }

#ifdef ASFW_HOST_TEST
    if (!workQueue_) {
        fn();
        return kInvalidSchedulerToken;
    }
    const SchedulerToken token = nextToken_++;
    workQueue_->DispatchAsyncAfter(delayNs, std::move(fn));
    return token;
#else
    if (!timer_) {
        return kInvalidSchedulerToken;
    }

    SchedulerToken token;
    uint64_t earliest;
    OSSharedPtr<IOTimerDispatchSource> timer;
    {
        IOLockGuard guard(lock_);
        token = nextToken_++;
        if (token == kInvalidSchedulerToken) {
            token = nextToken_++;
        }
        pending_.emplace(token, PendingCallback{
                                    .deadlineTicks = DeadlineTicksFromNow(delayNs),
                                    .fn = std::move(fn),
                                });
        earliest = EarliestDeadlineLocked();
        timer = timer_;
    }
    ArmTimerUnlocked(timer.get(), earliest);
    return token;
#endif
}

void DriverKitSessionScheduler::Cancel(SchedulerToken token) {
    if (token == kInvalidSchedulerToken) {
        return;
    }

    uint64_t earliest = 0;
    OSSharedPtr<IOTimerDispatchSource> timer;
    bool changed = false;
    {
        IOLockGuard guard(lock_);
        if (pending_.erase(token) > 0) {
            changed = true;
            earliest = EarliestDeadlineLocked();
            timer = timer_;
        }
    }
    if (changed) {
        ArmTimerUnlocked(timer.get(), earliest);
    }
}

void DriverKitSessionScheduler::HandleTimerFired() noexcept {
    std::vector<std::function<void()>> due;
    uint64_t earliest = 0;
    OSSharedPtr<IOTimerDispatchSource> timer;

    {
        IOLockGuard guard(lock_);
        const uint64_t now = mach_absolute_time();
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->second.deadlineTicks <= now) {
                due.push_back(std::move(it->second.fn));
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        earliest = EarliestDeadlineLocked();
        timer = timer_;
    }
    ArmTimerUnlocked(timer.get(), earliest);

    for (auto& fn : due) {
        if (fn) {
            fn();
        }
    }
}

uint64_t DriverKitSessionScheduler::EarliestDeadlineLocked() const noexcept {
    if (pending_.empty()) {
        return 0;
    }
    const auto next = std::min_element(
        pending_.begin(), pending_.end(),
        [](const auto& a, const auto& b) {
            return a.second.deadlineTicks < b.second.deadlineTicks;
        });
    return next == pending_.end() ? 0 : next->second.deadlineTicks;
}

void DriverKitSessionScheduler::ArmTimerUnlocked(IOTimerDispatchSource* timer,
                                                 uint64_t deadlineTicks) noexcept {
#ifdef ASFW_HOST_TEST
    (void)timer;
    (void)deadlineTicks;
#else
    if (timer == nullptr || deadlineTicks == 0) {
        return;
    }
    // lock_ MUST NOT be held here. WakeAtTime is RPC-dispatched to the timer's
    // queue (ASFWDriver-Default); that queue's completion handlers re-enter the
    // scheduler and take lock_. Holding lock_ across this call deadlocked the
    // user-client teardown queue against the driver queue and tripped the 60s
    // IOKit registry busy-timeout kernel panic (2026-06-22).
    (void)timer->WakeAtTime(kIOTimerClockMachAbsoluteTime, deadlineTicks, 0);
#endif
}

uint64_t DriverKitSessionScheduler::DeadlineTicksFromNow(uint64_t delayNs) const noexcept {
    (void)ASFW::Timing::initializeHostTimebase();
    uint64_t deltaTicks = ASFW::Timing::nanosToHostTicks(delayNs);
    if (deltaTicks == 0) {
        deltaTicks = 1;
    }
    return mach_absolute_time() + deltaTicks;
}

} // namespace ASFW::Protocols::SBP2
