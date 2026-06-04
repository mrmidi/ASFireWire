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

// Output (TX/playback) shared ring depth. Decoupled from the IO period: the HAL
// writes one period (kAudioIoPeriodFrames) per WriteEnd, but the shared ring
// must be several periods deep so the isoch consumer can trail the HAL by a
// stable lead without starving between WriteEnd bumps. See FW-26 #6/#8.
inline constexpr uint32_t kAudioOutputRingFrames = 4096;   // 8 IO periods

// Target gap (writtenEnd - consumer cursor) the isoch TX consumer maintains.
inline constexpr uint32_t kOutputConsumerLeadFrames = 768; // ~1.5 periods (~16ms @48k)

// Deadband: rebase the consumer cursor only when |lead - target| exceeds this.
inline constexpr uint32_t kOutputCursorResyncDeadbandFrames = 256; // ~0.5 period

static_assert(kTxQueueCapacityFrames != 0 && ((kTxQueueCapacityFrames & (kTxQueueCapacityFrames - 1)) == 0),
              "TX queue capacity must be power-of-two");
static_assert(kRxQueueCapacityFrames != 0 && ((kRxQueueCapacityFrames & (kRxQueueCapacityFrames - 1)) == 0),
              "RX queue capacity must be power-of-two");
static_assert(kAudioRingBufferFrames != 0 && ((kAudioRingBufferFrames & (kAudioRingBufferFrames - 1)) == 0),
              "Audio ring buffer frame count must be power-of-two");
static_assert(kAudioOutputRingFrames != 0 && ((kAudioOutputRingFrames & (kAudioOutputRingFrames - 1)) == 0),
              "Output ring frame count must be power-of-two");
static_assert((kAudioOutputRingFrames % kAudioIoPeriodFrames) == 0,
              "Output ring must be an integer number of IO periods");
static_assert(kOutputConsumerLeadFrames > kAudioIoPeriodFrames,
              "Consumer lead must exceed one IO period to survive WriteEnd bursts");
static_assert(kOutputConsumerLeadFrames + kOutputCursorResyncDeadbandFrames < kAudioOutputRingFrames,
              "Lead plus deadband must stay within the output ring");

} // namespace ASFW::Isoch::Config

