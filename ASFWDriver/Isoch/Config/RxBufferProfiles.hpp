#pragma once

#include <cstdint>

namespace ASFW::Isoch::Config {

#define ASFW_RX_PROFILE_A 1  // Conservative: matches legacy 2048-frame fill target
#define ASFW_RX_PROFILE_B 2  // Low-latency: ~5-8 ms input latency at 48 kHz
#define ASFW_RX_PROFILE_C 3  // Aggressive low-latency: ~3-5 ms, may click on slow machines

#ifndef ASFW_RX_TUNING_PROFILE
#define ASFW_RX_TUNING_PROFILE ASFW_RX_PROFILE_B
#endif

struct RxBufferProfile {
    const char* name;
    uint32_t startupFillTargetFrames;       // RX queue fill before first CoreAudio read
    uint32_t startupDrainThresholdFrames;   // excess above target before draining
    uint32_t safetyOffsetFrames;            // input-specific HAL safety offset
    uint32_t inputLatencyFrames;            // reported device input latency
};

// Profile A: Conservative (current behavior, safe fallback)
inline constexpr RxBufferProfile kRxProfileA{
    "A",
    2048,  // startupFillTargetFrames
    256,   // startupDrainThresholdFrames
    64,    // safetyOffsetFrames
    24     // inputLatencyFrames
};

// Profile B: Low-latency (~5-8 ms @ 48 kHz)
inline constexpr RxBufferProfile kRxProfileB{
    "B",
    256,   // startupFillTargetFrames
    128,   // startupDrainThresholdFrames
    48,    // safetyOffsetFrames
    32     // inputLatencyFrames
};

// Profile C: Aggressive low-latency (~3-5 ms @ 48 kHz)
inline constexpr RxBufferProfile kRxProfileC{
    "C",
    128,   // startupFillTargetFrames
    64,    // startupDrainThresholdFrames
    32,    // safetyOffsetFrames
    24     // inputLatencyFrames
};

constexpr bool IsValidRxProfile(const RxBufferProfile& profile) noexcept {
    return profile.startupFillTargetFrames > 0 &&
           profile.startupDrainThresholdFrames > 0 &&
           profile.safetyOffsetFrames > 0 &&
           profile.inputLatencyFrames > 0;
}

static_assert(IsValidRxProfile(kRxProfileA), "RX Profile A is invalid");
static_assert(IsValidRxProfile(kRxProfileB), "RX Profile B is invalid");
static_assert(IsValidRxProfile(kRxProfileC), "RX Profile C is invalid");

#if ASFW_RX_TUNING_PROFILE == ASFW_RX_PROFILE_A
inline constexpr RxBufferProfile kRxBufferProfile = kRxProfileA;
#elif ASFW_RX_TUNING_PROFILE == ASFW_RX_PROFILE_B
inline constexpr RxBufferProfile kRxBufferProfile = kRxProfileB;
#elif ASFW_RX_TUNING_PROFILE == ASFW_RX_PROFILE_C
inline constexpr RxBufferProfile kRxBufferProfile = kRxProfileC;
#else
#error "Invalid ASFW_RX_TUNING_PROFILE value. Use ASFW_RX_PROFILE_A/B/C."
#endif

static_assert(IsValidRxProfile(kRxBufferProfile), "Selected RX buffer profile is invalid");

}  // namespace ASFW::Isoch::Config
