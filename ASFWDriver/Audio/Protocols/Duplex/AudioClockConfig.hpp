// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioClockConfig.hpp - Protocol-neutral duplex clock request

#pragma once

#include <cstdint>

namespace ASFW::Audio {

// The duplex lifecycle is presently 48 kHz-only. Protocol adapters translate
// this generic request into their device-specific clock command or register value.
struct AudioClockConfig {
    uint32_t sampleRateHz{0};
};

[[nodiscard]] constexpr bool IsSupportedAudioClockConfig(
    const AudioClockConfig& desiredClock) noexcept {
    return desiredClock.sampleRateHz == 48000U;
}

} // namespace ASFW::Audio
