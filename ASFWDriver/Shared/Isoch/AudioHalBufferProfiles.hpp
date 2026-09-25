#pragma once

#include <cstdint>

namespace ASFW::IsochTransport {

// HAL-facing buffer geometry of one stream rate: the active frame ring, the
// largest client IO the budgets are sized for, and the zero-timestamp period.
struct AudioHalBufferProfile final {
    const char* name;
    uint32_t frameRingFrames;
    uint32_t clientIoBudgetFrames;
    uint32_t zeroTimestampPeriodFrames;
};

// Largest blocking-mode frames-per-data-packet across the IEC 61883-6 rate
// ladder: the SYT interval is 8 @1x, 16 @2x, 32 @4x. ZTS anchors are only
// published when a boundary lands on a packet-first frame, so the period must
// be a multiple of every tier's frames-per-packet (all divide 32). A period
// that is not silently thins anchors ~4x at 4x rates
// (tools/amdtp_blocking_cadence_sim.py, 1544-period counterexample).
inline constexpr uint32_t kMaxBlockingFramesPerDataPacket = 32;

// V3 geometry (decision D2, TIMING_GEOMETRY_OWNERSHIP.md §0; hardware-validated
// on the midi branch). One ZTS period of 12288 frames per 1x rate tier:
//
// 1. 12288 = 2^12 * 3. The blocking cadence (D,D,D,N: 6000 DATA packets/s
//    against 8000 cycles at 48/96/192 kHz) carries a factor of 3, so no power
//    of two is a whole number of cadence blocks. 12288 is 512 cadence blocks
//    and 256 eight-packet completion groups at 48, 96 and 192 kHz (2048
//    cycles, 256 ms), so the ZTS boundary holds a fixed offset in the group.
//    The 44.1 kHz family has 5512.5 DATA packets/s: its phase walks at any
//    period, which the timeline tolerates (anchors land on packet-first frames).
// 2. AudioDriverKit caps the client buffer at min(zts * 3/8, 4096). 12288 is the
//    smallest period that reaches 4096 and still divides by 512, 1024 and 2048
//    client IO budgets. 8192 would cap clients at 3072.
//
// The ring equals the ZTS period: the HAL wraps the stream buffer on the ZTS
// period, so the active ring is a function of the rate. The shared memory is
// allocated once at kAllocatedFrameRingFrames and a rate change only moves the
// active ring inside it (no descriptor is replaced under CoreAudio).
//
// Cost: 3.91 zero timestamps/s at 48 kHz (5.86 at 8192), so the HAL clock
// filter takes proportionally longer to lock after a start or a reseed.
inline constexpr uint32_t kV3ZeroTimestampPeriodFrames1x = 12'288;
inline constexpr uint32_t kV3ClientIoBudgetFrames = 1'024;

/// Rate tier of the IEC 61883-6 ladder: 1 (32/44.1/48 kHz), 2 (88.2/96), 4
/// (176.4/192), or 0 for a rate outside the ladder.
[[nodiscard]] constexpr uint32_t HalRateTier(uint32_t sampleRateHz) noexcept {
    switch (sampleRateHz) {
        case 32'000:
        case 44'100:
        case 48'000:
            return 1;
        case 88'200:
        case 96'000:
            return 2;
        case 176'400:
        case 192'000:
            return 4;
        default:
            return 0;
    }
}

/// HAL buffer geometry for a stream rate. The one entry point for ring and
/// ZTS sizes: the timing resolver, the endpoint runtime's active ring, and the
/// hardware timeline's ZTS grid all read it, so they agree by construction.
/// A rate outside the ladder yields a zero profile, which is invalid.
[[nodiscard]] constexpr AudioHalBufferProfile HalBufferProfileForRate(
    uint32_t sampleRateHz) noexcept {
    const uint32_t tier = HalRateTier(sampleRateHz);
    const uint32_t period = kV3ZeroTimestampPeriodFrames1x * tier;
    switch (tier) {
        case 1: return {"audio-engine-v3-1x", period, kV3ClientIoBudgetFrames, period};
        case 2: return {"audio-engine-v3-2x", period, kV3ClientIoBudgetFrames, period};
        case 4: return {"audio-engine-v3-4x", period, kV3ClientIoBudgetFrames, period};
        default: return {"unsupported", 0, kV3ClientIoBudgetFrames, 0};
    }
}

/// Frames of shared stream memory allocated per direction: the largest active
/// ring of any supported rate. 4x rates (49152 frames) exceed it and are
/// refused by the resolver, as on the midi branch.
inline constexpr uint32_t kAllocatedFrameRingFrames =
    HalBufferProfileForRate(96'000).frameRingFrames; // 24576

/// Largest client IO buffer AudioDriverKit permits for a ZTS period.
[[nodiscard]] constexpr uint32_t AdkMaxClientIoFrames(uint32_t ztsPeriodFrames) noexcept {
    const uint32_t byPeriod = (ztsPeriodFrames * 3) / 8;
    return byPeriod < 4'096 ? byPeriod : 4'096;
}

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

[[nodiscard]] constexpr bool ProfileFitsAllocation(
    const AudioHalBufferProfile& profile) noexcept {
    return profile.frameRingFrames <= kAllocatedFrameRingFrames &&
           kAllocatedFrameRingFrames % profile.frameRingFrames == 0;
}

static_assert(IsValidAudioHalBufferProfile(HalBufferProfileForRate(44'100)));
static_assert(IsValidAudioHalBufferProfile(HalBufferProfileForRate(48'000)));
static_assert(IsValidAudioHalBufferProfile(HalBufferProfileForRate(96'000)));
static_assert(IsValidAudioHalBufferProfile(HalBufferProfileForRate(192'000)));
static_assert(!IsValidAudioHalBufferProfile(HalBufferProfileForRate(22'050)));
static_assert(kAllocatedFrameRingFrames == 24'576);
static_assert(ProfileFitsAllocation(HalBufferProfileForRate(48'000)));
static_assert(ProfileFitsAllocation(HalBufferProfileForRate(96'000)));
static_assert(!ProfileFitsAllocation(HalBufferProfileForRate(192'000)));
static_assert(HalBufferProfileForRate(48'000).frameRingFrames ==
              HalBufferProfileForRate(48'000).zeroTimestampPeriodFrames);
static_assert(AdkMaxClientIoFrames(HalBufferProfileForRate(48'000).zeroTimestampPeriodFrames) ==
                  4'096,
              "the V3 period must reach AudioDriverKit's 4096-frame client ceiling");
static_assert(AdkMaxClientIoFrames(8'192) == 3'072);

} // namespace ASFW::IsochTransport
