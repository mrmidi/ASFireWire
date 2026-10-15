// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../AsyncTypes.hpp"
#include "../../Scheduling/ITimerScheduler.hpp"
#include <DriverKit/IOLib.h>
#include <algorithm>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace ASFW::Async {

enum class CommandRepeat { Never, Timeout, Failure };

// Software budgets are distinct from OHCI's hardware retries. Apple defaults
// to three retries and a 125 ms command timeout (IOFWCommand.h:37,
// IOFWAsyncCommand.cpp:94-100,425-468). Final BUSY is reported as timeout.
struct TransactionRetryPolicy {
    unsigned busyRetries{3};
    uint64_t busyTimeoutNs{125'000'000};
    CommandRepeat repeat{CommandRepeat::Never};
    unsigned commandAttempts{1};
    uint64_t repeatDelayNs{0};
    uint64_t coalescedDelayNs{0};
};

// Payload-opaque, callback-driven recovery. T must own any response data needed
// after the transport callback returns. No bus/scheduler/user call holds lock_.
template <typename T>
class TransactionRetry final : public std::enable_shared_from_this<TransactionRetry<T>> {
public:
    using Callback = std::function<void(AsyncStatus, T)>;
    using Submit = std::function<AsyncHandle(Callback)>;
    using CancelAttempt = std::function<void(AsyncHandle)>;
    using Current = std::function<bool()>;

    [[nodiscard]] static std::shared_ptr<TransactionRetry> Create(
        Scheduling::ITimerScheduler* scheduler, TransactionRetryPolicy policy,
        Submit submit, CancelAttempt cancel, Current current, Callback callback) {
        return std::shared_ptr<TransactionRetry>(new TransactionRetry(
            scheduler, policy, std::move(submit), std::move(cancel),
            std::move(current), std::move(callback)));
    }

    ~TransactionRetry() { if (lock_) IOLockFree(lock_); }

    void Begin() {
        const auto self = this->shared_from_this();
        {
            Guard guard(lock_);
            if (done_ || started_) return;
            started_ = true;
        }
        if (!lock_) Finish(AsyncStatus::kHardwareError, {});
        else Attempt();
    }

    // Another request for this exact command receives a fresh observation.
    // Multiple queued requests share one resubmission, not the in-flight result.
    [[nodiscard]] bool Coalesce(Callback& callback) {
        Guard guard(lock_);
        if (done_ || !started_ || !scheduler_) return false;
        pending_.push_back(std::move(callback));
        return true;
    }

    void Cancel(AsyncStatus status = AsyncStatus::kAborted) {
        const auto self = this->shared_from_this();
        Finish(status, {}, true);
    }
    [[nodiscard]] bool Done() const { Guard guard(lock_); return done_; }

private:
    struct Guard {
        explicit Guard(IOLock* lock) : lock_(lock) { if (lock_) IOLockLock(lock_); }
        ~Guard() { if (lock_) IOLockUnlock(lock_); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        IOLock* lock_;
    };
    enum class Phase { Sending, Waiting };

    TransactionRetry(Scheduling::ITimerScheduler* scheduler, TransactionRetryPolicy policy,
                     Submit submit, CancelAttempt cancel, Current current, Callback callback)
        : scheduler_(scheduler), policy_(policy), submit_(std::move(submit)),
          cancel_(std::move(cancel)), current_(std::move(current)) {
        callbacks_.push_back(std::move(callback));
    }

    void Attempt() {
        const auto submittedNs = scheduler_ ? scheduler_->NowNs() : 0;
        unsigned attempt;
        {
            Guard guard(lock_);
            if (done_) return;
            timer_ = Scheduling::kInvalidTimerToken;
            phase_ = Phase::Sending;
            attempt = ++attempts_;
            submittedNs_ = submittedNs;
        }
        if (!current_()) { Finish(AsyncStatus::kStaleGeneration, {}); return; }
        {
            Guard guard(lock_);
            if (done_) return;
        }
        const auto weak = this->weak_from_this();
        const auto handle = submit_([weak, attempt](AsyncStatus status, T value) {
            if (const auto self = weak.lock()) self->Completed(attempt, status, std::move(value));
        });
        bool cancelHandle, admissionFailed = false;
        {
            Guard guard(lock_);
            cancelHandle = cancelled_;
            if (!done_ && phase_ == Phase::Sending && attempts_ == attempt) {
                handle_ = handle;
                admissionFailed = !handle;
            }
        }
        if (cancelHandle && handle) cancel_(handle);
        if (admissionFailed) Completed(attempt, AsyncStatus::kHardwareError, {});
    }

    void Completed(unsigned attempt, AsyncStatus status, T value) {
        const auto nowNs = scheduler_ ? scheduler_->NowNs() : 0;
        uint64_t delay = 0;
        bool retry = false;
        {
            Guard guard(lock_);
            if (done_ || attempts_ != attempt || phase_ != Phase::Sending) return;
            handle_ = {};
            if (scheduler_ && status == AsyncStatus::kBusyRetryExhausted) {
                const auto elapsed = nowNs - submittedNs_;
                delay = elapsed < policy_.busyTimeoutNs ? policy_.busyTimeoutNs - elapsed : 0;
                status = AsyncStatus::kTimeout;
                if (busyRetries_ < policy_.busyRetries) {
                    ++busyRetries_;
                    retry = true;
                }
            }
            if (!retry && scheduler_ && status != AsyncStatus::kSuccess &&
                status != AsyncStatus::kAborted && status != AsyncStatus::kStaleGeneration &&
                commands_ < policy_.commandAttempts &&
                (policy_.repeat == CommandRepeat::Failure ||
                 (policy_.repeat == CommandRepeat::Timeout && status == AsyncStatus::kTimeout))) {
                ++commands_;
                busyRetries_ = 0;
                delay += policy_.repeatDelayNs;
                retry = true;
            }
            phase_ = Phase::Waiting; // Reject duplicate completions, including during Finish.
        }
        if (retry || delay) Schedule(delay, attempt, retry, status, std::move(value));
        else Finish(status, std::move(value));
    }

    void Schedule(uint64_t delay, unsigned attempt, bool retry, AsyncStatus status, T value) {
        unsigned epoch;
        {
            Guard guard(lock_);
            if (done_ || attempts_ != attempt || phase_ != Phase::Waiting) return;
            epoch = ++timerEpoch_;
        }
        if (!current_()) { Finish(AsyncStatus::kStaleGeneration, {}); return; }
        if (!scheduler_) { Finish(status, std::move(value)); return; }
        const auto weak = this->weak_from_this();
        const auto token = scheduler_->ScheduleAfter(delay,
            [weak, attempt, epoch, retry, status, value = std::move(value)]() mutable {
                const auto self = weak.lock();
                if (!self) return;
                {
                    Guard guard(self->lock_);
                    if (self->done_ || self->attempts_ != attempt || self->timerEpoch_ != epoch ||
                        self->phase_ != Phase::Waiting) return;
                    self->timer_ = Scheduling::kInvalidTimerToken;
                }
                if (!self->current_()) self->Finish(AsyncStatus::kStaleGeneration, {});
                else if (retry) self->Attempt();
                else self->Finish(status, std::move(value));
            });
        bool cancelToken;
        {
            Guard guard(lock_);
            cancelToken = done_ || attempts_ != attempt || timerEpoch_ != epoch || phase_ != Phase::Waiting;
            if (!cancelToken) timer_ = token;
        }
        if (cancelToken && token) scheduler_->Cancel(token);
        if (!token && !cancelToken) Finish(AsyncStatus::kHardwareError, {});
    }

    void Finish(AsyncStatus status, T value, bool cancel = false) {
        std::vector<Callback> callbacks;
        AsyncHandle handle;
        Scheduling::TimerToken timer;
        bool coalesced;
        unsigned attempt;
        {
            Guard guard(lock_);
            if (done_) return;
            cancelled_ = cancel;
            timer = std::exchange(timer_, Scheduling::kInvalidTimerToken);
            handle = std::exchange(handle_, AsyncHandle{});
            callbacks = std::move(callbacks_);
            coalesced = !pending_.empty() && scheduler_ && !cancel &&
                status != AsyncStatus::kAborted && status != AsyncStatus::kStaleGeneration;
            if (coalesced) {
                callbacks_ = std::move(pending_);
                busyRetries_ = 0;
                commands_ = 1;
                phase_ = Phase::Waiting;
            } else {
                for (auto& callback : pending_) callbacks.push_back(std::move(callback));
                pending_.clear();
                done_ = true;
            }
            attempt = attempts_;
        }
        if (timer) scheduler_->Cancel(timer);
        if (cancel && handle) cancel_(handle);
        if (coalesced) Schedule(policy_.coalescedDelayNs, attempt, true, status, {});
        for (auto& callback : callbacks) if (callback) callback(status, value);
    }

    Scheduling::ITimerScheduler* scheduler_;
    TransactionRetryPolicy policy_;
    Submit submit_;
    CancelAttempt cancel_;
    Current current_;
    std::vector<Callback> callbacks_, pending_;
    IOLock* lock_{IOLockAlloc()};
    Scheduling::TimerToken timer_{Scheduling::kInvalidTimerToken};
    AsyncHandle handle_{};
    uint64_t submittedNs_{0};
    unsigned attempts_{0}, busyRetries_{0}, commands_{1}, timerEpoch_{0};
    Phase phase_{Phase::Sending};
    bool started_{false}, done_{false}, cancelled_{false};
};

// A protocol's teardown cancels all logical commands, including armed timers.
template <typename T>
class TransactionRetryOwner final {
public:
    using Operation = TransactionRetry<T>;
    ~TransactionRetryOwner() { CancelAll(true); if (lock_) IOLockFree(lock_); }

    void Begin(const std::shared_ptr<Operation>& operation) {
        bool cancel;
        {
            Guard guard(lock_);
            cancel = closed_ || !lock_;
            std::erase_if(operations_, [](const auto& op) { return op->Done(); });
            if (!cancel) operations_.push_back(operation);
        }
        if (cancel) operation->Cancel();
        else operation->Begin();
    }
    void CancelAll(bool close = false, AsyncStatus status = AsyncStatus::kAborted) {
        std::vector<std::shared_ptr<Operation>> operations;
        {
            Guard guard(lock_);
            closed_ = closed_ || close;
            operations.swap(operations_);
        }
        for (const auto& op : operations) op->Cancel(status);
    }
    void Reopen() { Guard guard(lock_); closed_ = false; }
private:
    struct Guard {
        explicit Guard(IOLock* lock) : lock_(lock) { if (lock_) IOLockLock(lock_); }
        ~Guard() { if (lock_) IOLockUnlock(lock_); }
        IOLock* lock_;
    };
    IOLock* lock_{IOLockAlloc()};
    bool closed_{false};
    std::vector<std::shared_ptr<Operation>> operations_;
};

} // namespace ASFW::Async

