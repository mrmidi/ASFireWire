// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FakeDiceWaitClock.hpp - Virtual-time DiceWaitClock for host tests.
//
// Every wait advances the shared FakeTimerScheduler, so events a test schedules
// on it (a late notification, a teardown request) fire at the same virtual
// times whether the code under test waits synchronously or asynchronously.

#pragma once

#include "Audio/Protocols/DICE/Core/DiceWaitClock.hpp"
#include "FakeTimerScheduler.hpp"

#include <cstdint>

namespace ASFW::Testing {

class FakeDiceWaitClock final : public ::ASFW::Audio::DICE::DiceWaitClock {
public:
    explicit FakeDiceWaitClock(FakeTimerScheduler& timer) noexcept : timer_(timer) {}

    void SleepMs(uint32_t ms) noexcept override {
        timer_.Advance(uint64_t{ms} * 1'000'000ULL);
    }

    // The simulated bus completes synchronously, so a backoff only happens
    // when a completion is missing. Advancing 1 ms lets the safety deadline
    // trip instead of spinning forever.
    void Backoff(uint32_t attempt) noexcept override {
        (void)attempt;
        ++backoffs_;
        timer_.Advance(1'000'000ULL);
    }

    [[nodiscard]] uint64_t NowMs() const noexcept override {
        return timer_.NowNs() / 1'000'000ULL;
    }

    [[nodiscard]] uint64_t Backoffs() const noexcept { return backoffs_; }

private:
    FakeTimerScheduler& timer_;
    uint64_t backoffs_{0};
};

} // namespace ASFW::Testing
