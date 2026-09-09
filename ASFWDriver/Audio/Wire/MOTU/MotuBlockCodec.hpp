// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuBlockCodec.hpp - Encode/decode of MOTU v2 data-block payloads.
//
// A data block is byte-addressed past its leading SPH quadlet: message bytes
// at offsets 4..9, then 24-bit big-endian PCM samples packed 3 bytes per
// channel from byte offset 10, zero-padded to quadlet alignment. Host samples
// use the 24-significant-MSBs-of-int32 convention. Cross-validated with Linux
// amdtp-motu.c:93-187 (PCM), :211-254 (MIDI), :373-393 (SPH write).

#pragma once

#include "MotuBlockLayout.hpp"
#include "MotuSph.hpp"
#include <cstdint>
#include <optional>
#include <span>

namespace ASFW::Encoding::Motu {

/// Write the SPH quadlet at the head of a data block (big-endian on the wire).
inline void WriteSph(std::span<uint8_t> block, uint32_t sph) noexcept {
    block[0] = static_cast<uint8_t>(sph >> 24);
    block[1] = static_cast<uint8_t>(sph >> 16);
    block[2] = static_cast<uint8_t>(sph >> 8);
    block[3] = static_cast<uint8_t>(sph);
}

[[nodiscard]] constexpr uint32_t ReadSph(
    std::span<const uint8_t> block) noexcept {
    return (static_cast<uint32_t>(block[0]) << 24) |
           (static_cast<uint32_t>(block[1]) << 16) |
           (static_cast<uint32_t>(block[2]) << 8) |
           static_cast<uint32_t>(block[3]);
}

/// Pack one host sample (24 significant MSBs of int32) into its 3-byte
/// big-endian chunk (amdtp-motu.c:150-156).
inline void WritePcmSample(std::span<uint8_t> chunk, int32_t sample) noexcept {
    const auto v = static_cast<uint32_t>(sample);
    chunk[0] = static_cast<uint8_t>(v >> 24);
    chunk[1] = static_cast<uint8_t>(v >> 16);
    chunk[2] = static_cast<uint8_t>(v >> 8);
}

/// Unpack one 3-byte chunk to a host sample (amdtp-motu.c:116-120).
[[nodiscard]] constexpr int32_t ReadPcmSample(
    std::span<const uint8_t> chunk) noexcept {
    return static_cast<int32_t>((static_cast<uint32_t>(chunk[0]) << 24) |
                                (static_cast<uint32_t>(chunk[1]) << 16) |
                                (static_cast<uint32_t>(chunk[2]) << 8));
}

/// Write one frame's PCM into a data block (channels in order from byte
/// offset 10). `block` must span the whole data block.
inline void WritePcmFrame(std::span<uint8_t> block,
                          std::span<const int32_t> samples) noexcept {
    size_t offset = kPcmByteOffset;
    for (const int32_t sample : samples) {
        WritePcmSample(block.subspan(offset, kBytesPerChunk), sample);
        offset += kBytesPerChunk;
    }
}

inline void WritePcmSilence(std::span<uint8_t> block,
                            uint32_t pcmChunks) noexcept {
    size_t offset = kPcmByteOffset;
    for (uint32_t c = 0; c < pcmChunks; ++c) {
        block[offset] = 0;
        block[offset + 1] = 0;
        block[offset + 2] = 0;
        offset += kBytesPerChunk;
    }
}

[[nodiscard]] inline int32_t ReadPcmChannel(std::span<const uint8_t> block,
                                            uint32_t channel) noexcept {
    return ReadPcmSample(
        block.subspan(kPcmByteOffset + channel * kBytesPerChunk,
                      kBytesPerChunk));
}

/// Place one MIDI byte in a data block's second-quadlet slot, or mark the
/// slot empty (amdtp-motu.c:219-234).
inline void WriteMidi2ndQ(std::span<uint8_t> block,
                          std::optional<uint8_t> midiByte) noexcept {
    if (midiByte.has_value()) {
        block[kMidiFlagByteOffset2ndQ] = kMidiFlagPresent;
        block[kMidiByteOffset2ndQ] = *midiByte;
    } else {
        block[kMidiFlagByteOffset2ndQ] = 0;
        block[kMidiByteOffset2ndQ] = 0;
    }
}

/// Extract the MIDI byte from a data block, if flagged present
/// (amdtp-motu.c:245-251).
[[nodiscard]] inline std::optional<uint8_t> ReadMidi2ndQ(
    std::span<const uint8_t> block) noexcept {
    if ((block[kMidiFlagByteOffset2ndQ] & kMidiFlagPresent) == 0) {
        return std::nullopt;
    }
    return block[kMidiByteOffset2ndQ];
}

} // namespace ASFW::Encoding::Motu
