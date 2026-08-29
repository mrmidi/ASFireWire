#pragma once

#include <cstdint>

namespace ASFW::Audio::Shared {

struct AudioHalBufferProfile final {
    const char* name;
    uint32_t frameRingFrames;
    uint32_t clientIoBudgetFrames;
    uint32_t zeroTimestampPeriodFrames;
};

// V3 exposes one HAL contract for every backend. Device protocol families may
// vary their wire cadence and transfer delay, but never CoreAudio ring geometry.
inline constexpr AudioHalBufferProfile kAudioHalBufferProfileV3{
    "audio-engine-v3",
    8'192,
    512,
    8'192,
};

// Largest blocking-mode frames-per-data-packet in V3: 8 @1x, 16 @2x,
// 32 @4x. Boundaries inside packets are projected by HardwareSampleTimeline;
// divisibility still keeps the shared HAL geometry exact at every rate.
inline constexpr uint32_t kMaxBlockingFramesPerDataPacket = 32;

[[nodiscard]] constexpr bool IsValidAudioHalBufferProfile(
    const AudioHalBufferProfile& profile) noexcept {
    return profile.name != nullptr &&
           profile.frameRingFrames != 0 &&
           profile.clientIoBudgetFrames != 0 &&
           profile.zeroTimestampPeriodFrames != 0 &&
           profile.frameRingFrames % profile.clientIoBudgetFrames == 0 &&
           profile.frameRingFrames % profile.zeroTimestampPeriodFrames == 0 &&
           profile.zeroTimestampPeriodFrames %
                   kMaxBlockingFramesPerDataPacket ==
               0;
}

static_assert(IsValidAudioHalBufferProfile(kAudioHalBufferProfileV3));
inline constexpr AudioHalBufferProfile kActiveAudioHalBufferProfile =
    kAudioHalBufferProfileV3;

} // namespace ASFW::Audio::Shared
