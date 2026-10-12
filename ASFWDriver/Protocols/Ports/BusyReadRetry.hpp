// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../../Async/AsyncTypes.hpp"
#include "../../Scheduling/ITimerScheduler.hpp"
#include <DriverKit/IOLib.h>
#include <functional>
#include <memory>
#include <utility>

namespace ASFW::Protocols::Ports {

// Explicit higher-level recovery for a read refused after OHCI hardware retries.
// Linux sound/firewire/lib.c:15,57-65 uses three total attempts, 20 ms apart.
// Timeout is excluded: replay after acceptance is a separate policy decision.
// Cancellation may come from teardown on another queue. Never call the bus,
// scheduler or user callback while holding the state lock (RPC/reentrancy).
class BusyReadRetry final : public std::enable_shared_from_this<BusyReadRetry> {
  public:
    using Callback = std::function<void(Async::AsyncStatus, uint32_t)>;
    using Submit = std::function<Async::AsyncHandle(Callback)>;
    using CancelAttempt = std::function<void(Async::AsyncHandle)>;
    using RouteCurrent = std::function<bool()>;

    // Publish ownership before Begin: a bus completion may run inline and
    // re-enter the owner to start another operation.
    [[nodiscard]] static std::shared_ptr<BusyReadRetry>
    Create(Scheduling::ITimerScheduler* scheduler, Submit submit, CancelAttempt cancel,
           RouteCurrent current, Callback callback) {
        return std::shared_ptr<BusyReadRetry>(
            new BusyReadRetry(scheduler, std::move(submit), std::move(cancel), std::move(current),
                              std::move(callback)));
    }

    void Begin() {
        const auto self = shared_from_this(); // reentrant completion can replace the caller owner
        {
            Guard guard(lock_);
            if (done_ || started_)
                return;
            started_ = true;
        }
        if (!lock_)
            Finish(Async::AsyncStatus::kHardwareError, 0);
        else
            Attempt();
    }

    ~BusyReadRetry() {
        if (lock_)
            IOLockFree(lock_);
    }

    void Cancel() {
        const auto self = shared_from_this(); // completion may release the last caller owner
        Finish(Async::AsyncStatus::kAborted, 0, true);
    }
    [[nodiscard]] bool Done() const noexcept {
        Guard guard(lock_);
        return done_;
    }

  private:
    struct Guard {
        explicit Guard(IOLock* lock) : lock_(lock) {
            if (lock_)
                IOLockLock(lock_);
        }
        ~Guard() {
            if (lock_)
                IOLockUnlock(lock_);
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        IOLock* lock_;
    };
    enum class Phase { Sending, Backoff };

    BusyReadRetry(Scheduling::ITimerScheduler* scheduler, Submit submit, CancelAttempt cancel,
                  RouteCurrent current, Callback callback)
        : scheduler_(scheduler), submit_(std::move(submit)), cancel_(std::move(cancel)),
          current_(std::move(current)), callback_(std::move(callback)) {}

    void Attempt() {
        unsigned attempt;
        {
            Guard guard(lock_);
            if (done_)
                return;
            timer_ = Scheduling::kInvalidTimerToken;
            phase_ = Phase::Sending;
            attempt = ++attempts_;
        }
        if (!current_()) {
            Finish(Async::AsyncStatus::kStaleGeneration, 0);
            return;
        }
        {
            Guard guard(lock_);
            if (done_)
                return;
        }
        const auto weak = weak_from_this();
        const auto handle = submit_([weak, attempt](Async::AsyncStatus status, uint32_t value) {
            if (const auto self = weak.lock())
                self->Completed(attempt, status, value);
        });
        bool cancelHandle;
        bool admissionFailed = false;
        {
            Guard guard(lock_);
            cancelHandle = cancelled_;
            // Completions may run inline. Do not restore an already-completed
            // handle, or mistake an inline BUSY for failed admission.
            if (!done_ && phase_ == Phase::Sending && attempts_ == attempt) {
                handle_ = handle;
                admissionFailed = !handle;
            }
        }
        if (cancelHandle && handle)
            cancel_(handle);
        if (admissionFailed)
            Finish(Async::AsyncStatus::kHardwareError, 0);
    }

    void Completed(unsigned attempt, Async::AsyncStatus status, uint32_t value) {
        bool retry;
        {
            Guard guard(lock_);
            if (done_ || attempts_ != attempt || phase_ != Phase::Sending)
                return;
            handle_ = {};
            retry = status == Async::AsyncStatus::kBusyRetryExhausted && attempt < 3 &&
                    scheduler_ != nullptr;
            if (retry)
                phase_ = Phase::Backoff;
        }
        if (!retry) {
            Finish(status, value);
            return;
        }
        if (!current_()) {
            Finish(Async::AsyncStatus::kStaleGeneration, 0);
            return;
        }
        const auto weak = weak_from_this();
        const auto token = scheduler_->ScheduleAfter(20'000'000ULL, [weak, attempt] {
            if (const auto self = weak.lock()) {
                {
                    Guard guard(self->lock_);
                    if (self->done_ || self->attempts_ != attempt || self->phase_ != Phase::Backoff)
                        return;
                }
                self->Attempt();
            }
        });
        bool cancelToken;
        {
            Guard guard(lock_);
            cancelToken = done_ || attempts_ != attempt || phase_ != Phase::Backoff;
            if (!cancelToken)
                timer_ = token;
        }
        // Cancel may race with ScheduleAfter returning its token, or the timer
        // may already have fired. Never leave a newly-created retry armed then.
        if (cancelToken && token)
            scheduler_->Cancel(token);
        if (!token && !cancelToken)
            Finish(Async::AsyncStatus::kHardwareError, 0);
    }

    void Finish(Async::AsyncStatus status, uint32_t value, bool cancel = false) {
        Callback callback;
        Async::AsyncHandle handle;
        Scheduling::TimerToken timer;
        {
            Guard guard(lock_);
            if (done_)
                return;
            done_ = true;
            cancelled_ = cancel;
            timer = std::exchange(timer_, Scheduling::kInvalidTimerToken);
            handle = std::exchange(handle_, Async::AsyncHandle{});
            callback = std::move(callback_);
        }
        if (timer)
            scheduler_->Cancel(timer);
        if (cancel && handle)
            cancel_(handle);
        if (callback)
            callback(status, value);
    }

    Scheduling::ITimerScheduler* scheduler_;
    Submit submit_;
    CancelAttempt cancel_;
    RouteCurrent current_;
    Callback callback_;
    IOLock* lock_{IOLockAlloc()};
    Scheduling::TimerToken timer_{Scheduling::kInvalidTimerToken};
    Async::AsyncHandle handle_{};
    unsigned attempts_{0};
    Phase phase_{Phase::Sending};
    bool started_{false};
    bool done_{false};
    bool cancelled_{false};
};

} // namespace ASFW::Protocols::Ports
