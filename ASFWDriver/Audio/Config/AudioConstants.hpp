#pragma once

#include "../../Shared/Isoch/AudioTimingGeometry.hpp"

#include <cstdint>

namespace ASFW::Isoch::Config {

/// Maximum AM824 slots per isochronous data block (CIP DBS).
/// Wire-level container size: PCM audio + MIDI + control slots combined.
inline constexpr uint32_t kMaxAmdtpDbs = 32;

/// Maximum host-facing PCM channel count the driver can handle.
/// Must be <= kMaxAmdtpDbs since PCM channels occupy a subset of DBS slots.
inline constexpr uint32_t kMaxPcmChannels = kMaxAmdtpDbs;

static_assert(kMaxPcmChannels <= kMaxAmdtpDbs,
              "PCM channel cap cannot exceed AMDTP DBS — PCM slots are a subset of DBS");

// Shared queue / buffer sizing.
inline constexpr uint32_t kTxQueueCapacityFrames = 4096;
inline constexpr uint32_t kRxQueueCapacityFrames = 4096;
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

// Target gap (writtenEnd - consumer cursor) the isoch TX consumer maintains.
inline constexpr uint32_t kOutputConsumerLeadFrames = 384; // ~0.75 period (~8ms @48k)

// Deadband: rebase the consumer cursor only when |lead - target| exceeds this.
inline constexpr uint32_t kOutputCursorResyncDeadbandFrames = 64; // ~0.125 period

static_assert(kTxQueueCapacityFrames != 0 && ((kTxQueueCapacityFrames & (kTxQueueCapacityFrames - 1)) == 0),
              "TX queue capacity must be power-of-two");
static_assert(kRxQueueCapacityFrames != 0 && ((kRxQueueCapacityFrames & (kRxQueueCapacityFrames - 1)) == 0),
              "RX queue capacity must be power-of-two");
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

static_assert(kOutputConsumerLeadFrames < kAudioOutputRingFrames,
              "Consumer lead must stay within the output ring");
static_assert(kOutputConsumerLeadFrames + kOutputCursorResyncDeadbandFrames < kAudioOutputRingFrames,
              "Lead plus deadband must stay within the output ring");

} // namespace ASFW::Isoch::Config
