#pragma once

// FakeSessionScheduler — deterministic virtual-clock backing for ISessionScheduler.
// Host-test only: no real time. Tests drive timeouts by calling Advance().

#include "ASFWDriver/Protocols/SBP2/Session/ISessionScheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace ASFW::Testing {

class FakeSessionScheduler final : public ASFW::Protocols::SBP2::ISessionScheduler {
public:
    using Token = ASFW::Protocols::SBP2::SchedulerToken;

    [[nodiscard]] Token ScheduleAfter(uint64_t delayNs,
                                      std::function<void()> fn) override {
        const Token token = ++nextToken_;
        entries_.push_back(Entry{token, nowNs_ + delayNs, std::move(fn), false, false});
        return token;
    }

    void Cancel(Token token) override {
        if (token == ASFW::Protocols::SBP2::kInvalidSchedulerToken) {
            return;
        }
        for (auto& e : entries_) {
            if (e.token == token && !e.fired) {
                e.canceled = true;
                // Release captures immediately. Production Cancel() erases its
                // pending callback, so retaining them in the virtual-clock fake
                // would hide lifetime bugs and keep test objects alive.
                e.fn = {};
            }
        }
    }

    // Advance the virtual clock by deltaNs, firing every due, non-canceled callback
    // in ascending deadline order (ties broken by insertion order). The clock is
    // stepped to each callback's own deadline *before* it runs, so a callback that
    // schedules follow-up work with a relative delay (e.g. reconnect, busy-timeout
    // replay) computes its deadline from its own fire time — matching a real timer,
    // not from the end of the Advance() window. Newly-due callbacks fire within the
    // same Advance(). The callback is moved out before invocation so a callback that
    // mutates the entry list cannot invalidate the function being run.
    void Advance(uint64_t deltaNs) {
        const uint64_t target = nowNs_ + deltaNs;
        for (;;) {
            std::size_t bestIdx = entries_.size();
            for (std::size_t i = 0; i < entries_.size(); ++i) {
                Entry& e = entries_[i];
                if (e.canceled || e.fired || e.deadlineNs > target) {
                    continue;
                }
                if (bestIdx == entries_.size() ||
                    e.deadlineNs < entries_[bestIdx].deadlineNs) {
                    bestIdx = i;
                }
            }
            if (bestIdx == entries_.size()) {
                break;
            }
            nowNs_ = entries_[bestIdx].deadlineNs;  // clock == deadline during callback
            entries_[bestIdx].fired = true;
            auto fn = std::move(entries_[bestIdx].fn);
            fn();
        }
        nowNs_ = target;
    }

    [[nodiscard]] uint64_t NowNs() const noexcept { return nowNs_; }

    [[nodiscard]] std::size_t PendingCount() const noexcept {
        std::size_t n = 0;
        for (const auto& e : entries_) {
            if (!e.canceled && !e.fired) {
                ++n;
            }
        }
        return n;
    }

private:
    struct Entry {
        Token token;
        uint64_t deadlineNs;
        std::function<void()> fn;
        bool canceled;
        bool fired;
    };

    Token nextToken_{ASFW::Protocols::SBP2::kInvalidSchedulerToken};
    uint64_t nowNs_{0};
    std::vector<Entry> entries_;
};

} // namespace ASFW::Testing
