// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FakeTimerScheduler.hpp — Deterministic virtual-clock backing for ITimerScheduler.
// Host-test only: no real time. Tests drive timeouts by calling Advance().

#pragma once

#include "ASFWDriver/Scheduling/ITimerScheduler.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace ASFW::Testing {

class FakeTimerScheduler final : public ASFW::Scheduling::ITimerScheduler {
public:
    using Token = ASFW::Scheduling::TimerToken;

    [[nodiscard]] Token ScheduleAfter(uint64_t delayNs, std::function<void()> fn) override {
        const Token token = ++nextToken_;
        entries_.push_back(Entry{token, nowNs_ + delayNs, std::move(fn), false, false});
        return token;
    }

    void Cancel(Token token) override {
        if (token == ASFW::Scheduling::kInvalidTimerToken) return;
        for (auto& e : entries_) {
            if (e.token == token && !e.fired) {
                e.canceled = true;
                e.fn = {};
            }
        }
    }

    // Advance virtual clock by deltaNs, firing due callbacks in deadline order.
    void Advance(uint64_t deltaNs) {
        const uint64_t target = nowNs_ + deltaNs;
        for (;;) {
            std::size_t bestIdx = entries_.size();
            for (std::size_t i = 0; i < entries_.size(); ++i) {
                Entry& e = entries_[i];
                if (e.canceled || e.fired || e.deadlineNs > target) continue;
                if (bestIdx == entries_.size() || e.deadlineNs < entries_[bestIdx].deadlineNs) {
                    bestIdx = i;
                }
            }
            if (bestIdx == entries_.size()) break;
            nowNs_ = entries_[bestIdx].deadlineNs;
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
            if (!e.canceled && !e.fired) ++n;
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

    Token nextToken_{ASFW::Scheduling::kInvalidTimerToken};
    uint64_t nowNs_{0};
    std::vector<Entry> entries_;
};

} // namespace ASFW::Testing
