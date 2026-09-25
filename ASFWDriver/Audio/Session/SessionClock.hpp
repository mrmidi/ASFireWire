// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SessionClock.hpp - Monotonic milliseconds for session deadlines.

#pragma once

#include <DriverKit/IOLib.h>

#include <cstdint>

namespace ASFW::Audio::Session {

[[nodiscard]] inline uint64_t UptimeMilliseconds() noexcept {
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.denom == 0) {
        return 0;
    }
    const unsigned __int128 nanos =
        static_cast<unsigned __int128>(mach_absolute_time()) * timebase.numer / timebase.denom;
    return static_cast<uint64_t>(nanos / 1'000'000U);
}

} // namespace ASFW::Audio::Session
