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

// The rates this build can stream: 1x only (32/44.1/48 kHz). 2x/4x change
// frames-per-packet and the device's stream layout and are parked. A device may
// still ANNOUNCE them -- DICE lists every CLOCK_CAPABILITIES rate, as the TCAT
// kexts do -- and this is the gate that refuses the pick before any bus
// traffic (ASFWAudioNub::RequestSampleRateChange). Protocol adapters
// additionally gate by device capabilities.
[[nodiscard]] constexpr bool IsSupportedAudioClockConfig(
    const AudioClockConfig& desiredClock) noexcept {
    return desiredClock.sampleRateHz == 32000U ||
           desiredClock.sampleRateHz == 44100U ||
           desiredClock.sampleRateHz == 48000U;
}

// FW-255 exposes the M-Audio special profile at 48 kHz only. Keep this gate
// separate so its scope can expand with the dedicated multi-rate work.
[[nodiscard]] constexpr bool IsSupportedMAudioSpecialClockConfig(
    const AudioClockConfig& desiredClock) noexcept {
    return desiredClock.sampleRateHz == 48000U;
}

} // namespace ASFW::Audio
