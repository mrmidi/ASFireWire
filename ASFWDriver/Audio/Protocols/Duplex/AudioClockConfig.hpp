// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioClockConfig.hpp - Protocol-neutral duplex clock request

#pragma once

#include <cstdint>

namespace ASFW::Audio {

// Protocol adapters translate this generic request into their device-specific
// clock command or register value.
struct AudioClockConfig {
    uint32_t sampleRateHz{0};
};

// Validated 1x rates (32/44.1/48 kHz). 2x/4x rates change frames-per-packet
// and per-stream channel geometry and are not yet validated end-to-end, so
// they are rejected here (protocol adapters additionally gate by device
// capabilities, e.g. DICE CLOCKCAPABILITIES).
[[nodiscard]] constexpr bool IsSupportedAudioClockConfig(
    const AudioClockConfig& desiredClock) noexcept {
    return desiredClock.sampleRateHz == 32000U ||
           desiredClock.sampleRateHz == 44100U ||
           desiredClock.sampleRateHz == 48000U;
}

} // namespace ASFW::Audio
