// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceWaitClock.hpp - The only way the DICE bring-up lets time pass.
//
// The bring-up is linear and blocking (documentation/AUDIO_SESSION_REDESIGN.md
// S1). It runs on the caller's queue - the audio nub's queue or
// com.asfw.audio.dice - never on the driver's Default queue, where bus
// completions arrive. It waits in two ways: a poll interval between register
// reads, and a short backoff while one bus transaction is in flight. Both go
// through this interface so host tests can run them on virtual time.

#pragma once

#include <DriverKit/IOLib.h>

#include <cstdint>

namespace ASFW::Audio::DICE {

class DiceWaitClock {
public:
    virtual ~DiceWaitClock() = default;

    // Sleep for one poll interval (10 ms in the bring-up).
    virtual void SleepMs(uint32_t ms) noexcept = 0;

    // Wait briefly while a bus transaction is in flight. `attempt` counts from 0.
    virtual void Backoff(uint32_t attempt) noexcept = 0;

    // Monotonic milliseconds, for the per-transaction safety deadline.
    [[nodiscard]] virtual uint64_t NowMs() const noexcept = 0;
};

// Production clock. Backoff follows the project's OHCI polling doctrine:
// escalating delays from 5 us to 255 us, then 1 ms sleeps.
class DriverKitWaitClock final : public DiceWaitClock {
public:
    [[nodiscard]] static DriverKitWaitClock& Shared() noexcept {
        static DriverKitWaitClock clock;
        return clock;
    }

    void SleepMs(uint32_t ms) noexcept override { IOSleep(ms); }

    void Backoff(uint32_t attempt) noexcept override {
        if (attempt < 6) {
            IODelay(5U << attempt);  // 5, 10, 20, 40, 80, 160 us
        } else if (attempt == 6) {
            IODelay(255);
        } else {
            IOSleep(1);
        }
    }

    [[nodiscard]] uint64_t NowMs() const noexcept override {
        mach_timebase_info_data_t timebase{};
        if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.denom == 0) {
            return 0;
        }
        const unsigned __int128 nanos =
            static_cast<unsigned __int128>(mach_absolute_time()) * timebase.numer / timebase.denom;
        return static_cast<uint64_t>(nanos / 1'000'000U);
    }
};

} // namespace ASFW::Audio::DICE
