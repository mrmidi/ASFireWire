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
// 12288 rather than 8192 since 2026-09-08. Two independent reasons:
//
// 1. 8192 = 2^13 carries no factor of 3, and the AMDTP cadence does --
//    6000 DATA packets/s against 8000 cycles/s is D,D,D,N over 4
//    packets. So no power of two is a whole number of cadence blocks at
//    any rate, and the zero-timestamp boundary walked the completion
//    group instead of holding a fixed offset in it. 12288 = 2^12 * 3 is
//    a whole number of blocks at 48/96/192 kHz (see the static_asserts
//    in AudioTimingGeometry.hpp, which is where both halves are visible).
//
// 2. AudioDriverKit derives the maximum client buffer as
//    min(zeroTimestampPeriodFrames * 3/8, 4096), so 8192 capped clients
//    at 3072 frames. 12288 is the smallest legal period that reaches the
//    4096 ceiling AND stays divisible by a 512, 1024 or 2048 client IO
//    budget, so raising that budget later does not reopen the geometry.
//
// The cost is anchor rate: 3.91 zero timestamps/s at 48k instead of 5.86,
// so the HAL clock filter takes proportionally longer to lock after a
// start or a reseed. 6144 is the alternative if that trade is unwanted --
// 7.81/s, but the client ceiling drops to 2304.
inline constexpr AudioHalBufferProfile kAudioHalBufferProfileV3{
    "audio-engine-v3",
    12'288,
    1'024,
    12'288,
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
