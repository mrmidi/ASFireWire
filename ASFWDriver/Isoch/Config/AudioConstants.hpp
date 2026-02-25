#pragma once

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
inline constexpr uint32_t kAudioRingBufferFrames = 4096;
inline constexpr uint32_t kAudioIoPeriodFrames = 512;

static_assert(kTxQueueCapacityFrames != 0 && ((kTxQueueCapacityFrames & (kTxQueueCapacityFrames - 1)) == 0),
              "TX queue capacity must be power-of-two");
static_assert(kRxQueueCapacityFrames != 0 && ((kRxQueueCapacityFrames & (kRxQueueCapacityFrames - 1)) == 0),
              "RX queue capacity must be power-of-two");
static_assert(kAudioRingBufferFrames != 0 && ((kAudioRingBufferFrames & (kAudioRingBufferFrames - 1)) == 0),
              "Audio ring buffer frame count must be power-of-two");

} // namespace ASFW::Isoch::Config

