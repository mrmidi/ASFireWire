#pragma once

#include <cstdint>

namespace ASFW::Isoch::Config {

/// Profile identifiers. Override build-time default with -DASFW_RX_TUNING_PROFILE=0 (A), =1 (B), or =2 (C).
enum class RxProfileId : uint8_t { A = 0, B = 1, C = 2 };

#if defined(ASFW_RX_TUNING_PROFILE)
inline constexpr uint8_t kRxTuningProfileRaw = ASFW_RX_TUNING_PROFILE;
#else
inline constexpr uint8_t kRxTuningProfileRaw = 1;  // default=B
#endif
static_assert(kRxTuningProfileRaw <= 2, "Invalid ASFW_RX_TUNING_PROFILE — use 0 (A), 1 (B), or 2 (C)");
inline constexpr RxProfileId kActiveRxProfileId = static_cast<RxProfileId>(kRxTuningProfileRaw);

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

constexpr RxBufferProfile SelectRxProfile(RxProfileId id) noexcept {
    switch (id) {
        case RxProfileId::A: return kRxProfileA;
        case RxProfileId::C: return kRxProfileC;
        default:             return kRxProfileB;
    }
}
inline constexpr RxBufferProfile kRxBufferProfile = SelectRxProfile(kActiveRxProfileId);

static_assert(IsValidRxProfile(kRxBufferProfile), "Selected RX buffer profile is invalid");

/// Runtime-selectable profile pointer (defaults to the compile-time kRxBufferProfile).
/// Callers wishing to support hot-switching should use GetActiveRxProfile() instead of
/// kRxBufferProfile directly.
inline const RxBufferProfile* gActiveRxProfile = &kRxBufferProfile;

[[nodiscard]] inline const RxBufferProfile& GetActiveRxProfile() noexcept {
    return *gActiveRxProfile;
}

inline void SetActiveRxProfile(RxProfileId id) noexcept {
    switch (id) {
        case RxProfileId::A: gActiveRxProfile = &kRxProfileA; break;
        case RxProfileId::B: gActiveRxProfile = &kRxProfileB; break;
        case RxProfileId::C: gActiveRxProfile = &kRxProfileC; break;
    }
}

}  // namespace ASFW::Isoch::Config
