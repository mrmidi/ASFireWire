// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuBlockLayout.hpp - Data-block geometry for MOTU FireWire protocol-v2
// devices (828mk2 first; the family covers Traveler, UltraLite, 896HD, 8pre).
//
// MOTU v2 streams carry standard IEC 61883-1 CIP headers (fmt 0x02,
// FDF 0x22, SPH bit set); only the payload framing below is vendor-specific.
// Wire truth cross-validated with Linux sound/firewire/motu:
//   - amdtp-motu.c:69-90   (SPH quadlet + 3-byte chunks + quadlet padding)
//   - motu-protocol-v2.c:227-272 (pcm_byte_offset 10, 2 msg chunks, ADAT adds)
//   - motu-protocol-v2.c:274-282 (828mk2 fixed chunks {14,14,0})
//   - motu-stream.c:117-131 (MIDI flag/byte offsets for the *_MIDI_2ND_Q flavor)
//   - motu.c:16-26         (rate table; mode = rate index >> 1)

#pragma once

#include <cstdint>

namespace ASFW::Encoding::Motu {

/// Sampling rates by clock-rate index. Mode = index >> 1 selects the
/// per-mode PCM chunk count.
inline constexpr uint32_t kClockRates[] = {44100, 48000, 88200, 96000,
                                           176400, 192000};
inline constexpr uint32_t kClockRateCount = 6;
inline constexpr uint32_t kModeCount = 3;

/// v2 payload framing constants (motu-protocol-v2.c:234-238).
inline constexpr uint32_t kMsgChunks = 2;
inline constexpr uint32_t kPcmByteOffset = 10;
inline constexpr uint32_t kBytesPerChunk = 3;

/// MIDI slot within a data block, "second quadlet" flavor used by the 828mk2
/// in both directions (motu-stream.c:117-131): flag byte at offset 4,
/// MIDI byte at offset 6, flag value 0x01.
inline constexpr uint32_t kMidiFlagByteOffset2ndQ = 4;
inline constexpr uint32_t kMidiByteOffset2ndQ = 6;
inline constexpr uint8_t kMidiFlagPresent = 0x01;

/// MIDI pacing: at most one byte per data block, throttled to physical MIDI
/// wire rate with margin (amdtp-motu.c:27-31,87-88).
inline constexpr uint32_t kMidiBytesPerSecond = 3093;

/// Per-mode fixed PCM chunk counts for the 828mk2 (motu-protocol-v2.c:274-282).
/// Mode 2 (176.4/192k) is unsupported by this model.
inline constexpr uint32_t k828mk2FixedPcmChunks[kModeCount] = {14, 14, 0};

[[nodiscard]] constexpr int32_t RateToIndex(uint32_t rate) noexcept {
    for (uint32_t i = 0; i < kClockRateCount; ++i) {
        if (kClockRates[i] == rate) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

[[nodiscard]] constexpr uint32_t IndexToMode(uint32_t rateIndex) noexcept {
    return rateIndex >> 1;
}

/// Extra PCM chunks contributed by an optical port in ADAT mode
/// (motu-protocol-v2.c:253-269). Single-optical-port models (828mk2): +8 at
/// mode 0, +4 at mode 1. (The dual-port 8pre differs; out of scope here.)
[[nodiscard]] constexpr uint32_t AdatExtraChunks(uint32_t mode) noexcept {
    if (mode == 0) {
        return 8;
    }
    if (mode == 1) {
        return 4;
    }
    return 0;
}

/// Data block size: one SPH quadlet, then (msg + pcm) 3-byte chunks padded to
/// quadlet alignment (amdtp-motu.c:69-74).
[[nodiscard]] constexpr uint32_t DataBlockQuadlets(uint32_t pcmChunks) noexcept {
    const uint32_t chunkBytes = (kMsgChunks + pcmChunks) * kBytesPerChunk;
    return 1 + (chunkBytes + 3) / 4;
}

[[nodiscard]] constexpr uint32_t DataBlockBytes(uint32_t pcmChunks) noexcept {
    return DataBlockQuadlets(pcmChunks) * 4;
}

/// Data blocks between MIDI-carrying blocks at a given rate
/// (amdtp-motu.c:87-88).
[[nodiscard]] constexpr uint32_t MidiDataBlockInterval(uint32_t rate) noexcept {
    return rate / kMidiBytesPerSecond;
}

} // namespace ASFW::Encoding::Motu
