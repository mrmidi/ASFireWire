// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RegistrationSupervisor.hpp"
#include <functional>
namespace Pro40Lab {
inline NSString *preparationName(PreparationResult r) {
    switch (r) {
        case PreparationResult::refused: return @"refused";
        case PreparationResult::journalUnavailable: return @"journal-unavailable";
        case PreparationResult::stoppedBeforeIntent: return @"stopped-before-intent";
        case PreparationResult::expiredBeforeIntent: return @"expired-before-intent";
        case PreparationResult::invalidEvidence: return @"invalid-evidence";
        case PreparationResult::intentFailed: return @"intent-failed";
        case PreparationResult::stoppedAfterIntent: return @"stopped-after-intent";
        case PreparationResult::expiredAfterIntent: return @"expired-after-intent";
        case PreparationResult::entered: return @"entered";
    }
}
// Serial control-queue heartbeat. It retains supervision until callback obligations
// resolve; a missing callback produces blocked-pending, never a fabricated finish.
// OS calls can block this queue. Timer cadence is not a hard operation deadline.
template<class Service>
class RegistrationEventLoop final : public std::enable_shared_from_this<RegistrationEventLoop<Service>> {
public:
    using Lifecycle = RegistrationLifecycle<Service>;
    using Report = std::function<void(NSDictionary *)>;
private:
    dispatch_queue_t queue_;
    dispatch_source_t timer_ = nil;
    std::shared_ptr<Lifecycle> owner_;
    RegistrationSupervisor<Service> supervisor_;
    Report report_;
    PreparationResult preparation_ = PreparationResult::refused;
    bool started_ = false, finished_ = false;
    NSString *lastPhase_ = nil, *attempt_ = nil;
    std::string path_;
    RegistrationEventLoop(dispatch_queue_t queue, std::shared_ptr<Lifecycle> owner, Report report)
        : queue_(queue), owner_(std::move(owner)), supervisor_(owner_), report_(std::move(report)) {}
    void tick() {
        dispatch_assert_queue(queue_);
        if (finished_) return;
        owner_->poll();
        auto state = owner_->state();
        bool retired = supervisor_.pollAndRetire();
        NSString *phase = state == LifecycleState::complete ? @"complete" :
            state == LifecycleState::blocked ? (retired ? @"blocked" : @"blocked-pending-callback") :
            @"awaiting-unregister";
        if (retired) {
            finished_ = true;
            dispatch_source_cancel(timer_);
            timer_ = nil; // break timer/handler/self cycle after this delivery
            owner_.reset();
        }
        if (![phase isEqual:lastPhase_]) {
            lastPhase_ = phase;
            report_(@{@"report_version": @1, @"mode": @"registration-experiment", @"phase": phase,
                @"attempt": attempt_, @"journal_path": [NSString stringWithUTF8String:path_.c_str()] ?: @"",
                @"preparation": preparationName(preparation_), @"supervision_finished": @(retired),
                @"registration_reconciled": @(state == LifecycleState::complete),
                @"callback_pending": @((state == LifecycleState::blocked && !retired) || state == LifecycleState::awaitingUnregister),
                @"helper_clean_exit_proven": @NO, @"audio_admitted": @NO});
        }
    }
public:
    static std::shared_ptr<RegistrationEventLoop> create(dispatch_queue_t queue,
        std::shared_ptr<Lifecycle> owner, Report report) {
        if (!queue || !owner || !report) return {};
        return std::shared_ptr<RegistrationEventLoop>(new RegistrationEventLoop(queue, std::move(owner), std::move(report)));
    }
    // Serial queue only. Cross-thread callers may instead keep the lifecycle's
    // handle and use its atomic requestStop directly.
    void requestStop() { dispatch_assert_queue(queue_); if (owner_) owner_->requestStop(); }
    [[nodiscard]] bool start(const std::string& path, NSString *attempt, typename Lifecycle::Collector collect) {
        dispatch_assert_queue(queue_);
        if (started_) return false;
        started_ = true;
        attempt_ = [attempt copy] ?: @"";
        path_ = path;
        timer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue_);
        if (!timer_) return false; // no journal or API operation has started
        auto retained = this->shared_from_this();
        dispatch_source_set_event_handler(timer_, ^{ retained->tick(); });
        dispatch_source_set_timer(timer_, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC),
                                 100 * NSEC_PER_MSEC, 10 * NSEC_PER_MSEC);
        dispatch_resume(timer_);
        preparation_ = owner_->start(path, attempt, std::move(collect));
        tick();
        return true;
    }
};
using NativeRegistrationEventLoop = RegistrationEventLoop<SMAppService>;
}
