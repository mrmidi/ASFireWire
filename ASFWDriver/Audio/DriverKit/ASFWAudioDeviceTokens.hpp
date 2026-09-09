// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ASFWAudioDeviceTokens.hpp
// ASFWDriver
//
// DriverKit-internal configuration change action tokens.

#pragma once

#include <cstdint>

namespace ASFW::Audio::DriverKit {

inline constexpr uint64_t kZtsPeriodTokenPrefix = 0xA5F8000000000000ULL;

[[nodiscard]] constexpr uint64_t ZtsPeriodToken(uint32_t periodFrames) noexcept {
    return kZtsPeriodTokenPrefix | periodFrames;
}

[[nodiscard]] constexpr bool IsZtsPeriodToken(uint64_t token) noexcept {
    return (token & 0xffffffff00000000ULL) == kZtsPeriodTokenPrefix;
}

[[nodiscard]] constexpr uint32_t ZtsPeriodFromToken(uint64_t token) noexcept {
    return static_cast<uint32_t>(token & 0xffffffffULL);
}

} // namespace ASFW::Audio::DriverKit
