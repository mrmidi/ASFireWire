#pragma once

#include "../../Shared/Isoch/AudioTimingGeometry.hpp"
#include "../Wire/AMDTP/AmdtpRateGeometry.hpp"

#include <cstdint>

namespace ASFW::Isoch::Config {

/// DBS / PCM channel ceilings. Wire facts: the authority is Encoding
/// (AmdtpRateGeometry.hpp); these are aliases for Isoch::Config callers
/// (TIMING_GEOMETRY_OWNERSHIP.md, G-21).
inline constexpr uint32_t kMaxAmdtpDbs = ASFW::Encoding::kMaxAmdtpDbs;
inline constexpr uint32_t kMaxPcmChannels = ASFW::Encoding::kMaxPcmChannels;

// Frame ring: an exact integer number of ZTS periods and max HAL IO periods
// (asserted in AudioTimingGeometry.hpp) so anchor grid, IO chunks, and ring
// wrap can never drift out of phase.
inline constexpr uint32_t kAudioRingBufferFrames =
    ASFW::IsochTransport::AudioTimingGeometry::kFrameRingFrames;
inline constexpr uint32_t kAudioIoPeriodFrames =
    ASFW::IsochTransport::AudioTimingGeometry::kHalIoPeriodFrames;

// Output (TX/playback) shared ring depth — same geometry as the input ring.
inline constexpr uint32_t kAudioOutputRingFrames =
    ASFW::IsochTransport::AudioTimingGeometry::kFrameRingFrames;

// Note: the frame rings are NOT required to be powers of two. The blocking
// cadence advances 24 frames per 4-packet block, so any ring that is an
// integer number of interrupt groups has a factor of 3 — indexing must use
// modulo, never bitmasks.
static_assert(kAudioRingBufferFrames != 0, "Audio ring must be non-empty");
static_assert(kAudioOutputRingFrames != 0, "Output ring must be non-empty");
static_assert(kAudioRingBufferFrames == kAudioOutputRingFrames,
              "Input and output must share one frame-ring geometry");
static_assert((kAudioRingBufferFrames % kAudioIoPeriodFrames) == 0,
              "Frame ring must be an integer number of IO periods");
static_assert((kAudioRingBufferFrames %
               ASFW::IsochTransport::AudioTimingGeometry::kFrameAlignment) == 0,
              "Frame ring must be divisible by 32 frames");

} // namespace ASFW::Isoch::Config
