// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RegistrationPreparation.hpp"
#include "NativeServiceOperations.hpp"
#include <functional>

namespace Pro40Lab {
enum class LifecycleState { initial, running, awaitingUnregister, complete, blocked };
// Control-plane owner only. All methods except requestStop run on the supplied
// serial queue. The supervisor must poll on that queue to notice idle expiry.
// An in-flight OS call is never cancelled by a deadline or reported as reaped.
template<class Service>
class RegistrationLifecycle final : public std::enable_shared_from_this<RegistrationLifecycle<Service>> {
public:
    using Clock = RegistrationPreparation::Clock;
    using Collector = std::function<CollectedRegistrationEvidence()>;
    using Observer = std::function<NSDictionary *()>;
    using Now = std::function<Clock::time_point()>;
private:
    dispatch_queue_t queue_;
    RegistrationPreparation owner_;
    ServiceOperations<Service> operations_;
    Observer observe_;
    Now now_;
    Clock::time_point start_{};
    std::atomic<bool> stop_{false};
    LifecycleState state_ = LifecycleState::initial;
    bool callbackPending_ = false;
    bool current() {
        auto t = now_();
        return !stop_.load() && t >= start_ && t - start_ < std::chrono::seconds(5);
    }
    void block() { owner_.abandon(); state_ = LifecycleState::blocked; }
    void returned(NSDictionary *result) {
        dispatch_assert_queue(queue_);
        callbackPending_ = false;
        // Late completion only retires its retained context; no reopened attempt.
        if (state_ != LifecycleState::awaitingUnregister) return;
        if (!current()) { block(); return; }
        if (!owner_.recordUnregisterResult(result)) { block(); return; }
        if (!current()) { block(); return; }
        NSDictionary *removal = observe_();
        if (!current()) { block(); return; }
        if (!owner_.recordRemoval(removal)) { block(); return; }
        state_ = LifecycleState::complete; // truthful even if final fsync ran late
    }
    RegistrationLifecycle(Service *service, dispatch_queue_t queue, Observer observe, Now now)
        : queue_(queue), operations_(service, queue), observe_(std::move(observe)), now_(std::move(now)) {}
public:
    static std::shared_ptr<RegistrationLifecycle> create(Service *service, dispatch_queue_t queue,
        Observer observe, Now now = [] { return Clock::now(); }) {
        if (!service || !queue || !observe || !now) return {};
        return std::shared_ptr<RegistrationLifecycle>(new RegistrationLifecycle(service, queue, std::move(observe), std::move(now)));
    }
    void requestStop() noexcept { stop_.store(true); }
    [[nodiscard]] LifecycleState state() const { dispatch_assert_queue(queue_); return state_; }
    [[nodiscard]] bool readyToRetire() const {
        dispatch_assert_queue(queue_);
        return !callbackPending_ && (state_ == LifecycleState::complete || state_ == LifecycleState::blocked);
    }
    void poll() {
        dispatch_assert_queue(queue_);
        if ((state_ == LifecycleState::running || state_ == LifecycleState::awaitingUnregister) && !current()) block();
    }
#ifdef PRO40_JOURNAL_TESTING
    void failWriteAt(int stage) { dispatch_assert_queue(queue_); owner_.failWriteAt(stage); }
#endif
    [[nodiscard]] PreparationResult start(const std::string& path, NSString *attempt, Collector collect) {
        dispatch_assert_queue(queue_);
        if (state_ != LifecycleState::initial || !collect) return PreparationResult::refused;
        state_ = LifecycleState::running;
        start_ = now_();
        auto prepared = owner_.run(path, attempt, collect, [&] {
            NSDictionary *result = operations_.registerOnce([&] { return current(); });
            if (!result || !owner_.recordRegistrationResult(result)) { block(); return; }
            if (!current()) { block(); return; }
            state_ = LifecycleState::awaitingUnregister;
            auto retained = this->shared_from_this();
            callbackPending_ = true;
            bool issued = operations_.unregisterOnce([&] {
                return current() && owner_.authorizeUnregister() && current();
            }, ^(NSDictionary *r) { retained->returned(r); });
            if (!issued) { callbackPending_ = false; block(); }
        }, now_, [&] { return stop_.load(); });
        if (prepared != PreparationResult::entered) block();
        return prepared;
    }
};
using NativeRegistrationLifecycle = RegistrationLifecycle<SMAppService>;
}
