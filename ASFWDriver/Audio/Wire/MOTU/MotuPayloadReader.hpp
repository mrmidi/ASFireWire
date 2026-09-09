// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuPayloadReader.hpp - Device->host PCM extraction for MOTU protocol-v2 streams.
//
// The capture counterpart to MotuPayloadWriter, and the MOTU equivalent of
// DirectRxPacketDecoder::DecodeDirectRxFrame(). That decoder takes `const uint32_t*`
// because AM824 and raw-24-in-32 both put one sample in one quadlet slot; MOTU's samples
// are 3-byte chunks at byte offset 10 of the data block, so they are read from a byte
// span instead.
//
// Sample convention: MotuBlockCodec::ReadPcmChannel returns the sample in the 24
// significant MSBs of an int32 (amdtp-motu.c:116-120). An arithmetic shift right by 8
// recovers the signed 24-bit value, matching what Signed24ToFloat32 expects.
//
// Cross-validated with Linux amdtp-motu.c:93-187.

#pragma once

#include "MotuBlockCodec.hpp"
#include "MotuBlockLayout.hpp"

#include <cstdint>
#include <span>

namespace ASFW::Encoding::Motu {

/// Signed 24-bit to float, matching DirectRxPacketDecoder's normalisation so MOTU and
/// the AMDTP families produce identically scaled host audio.
[[nodiscard]] constexpr float Signed24ToFloat32(int32_t sample) noexcept {
    if (sample <= -8388608) {
        return -1.0f;
    }
    return static_cast<float>(sample) / 8388607.0f;
}

/// Decode one data block's PCM chunks into an interleaved float frame.
///
/// `block` spans the whole data block (SPH quadlet first). `pcmChunks` is how many the
/// device carries; `outPcmFrame` receives `outChannels` floats starting at
/// `channelOffset` within the block's chunks, mirroring the channelOffset split the
/// AMDTP path uses for multi-stream devices.
///
/// Chunks the block cannot supply decode as silence rather than reading past its end.
inline void DecodeMotuBlock(std::span<const uint8_t> block,
                            uint32_t pcmChunks,
                            uint32_t channelOffset,
                            float* outPcmFrame,
                            uint32_t outChannels) noexcept {
    if (outPcmFrame == nullptr) {
        return;
    }
    for (uint32_t ch = 0; ch < outChannels; ++ch) {
        const uint32_t chunk = channelOffset + ch;
        const size_t chunkEnd =
            static_cast<size_t>(kPcmByteOffset) + (chunk + 1U) * kBytesPerChunk;
        if (chunk >= pcmChunks || chunkEnd > block.size()) {
            outPcmFrame[ch] = 0.0f;
            continue;
        }
        // ReadPcmChannel yields the sample in the top 24 bits; shift down to a signed
        // 24-bit value before normalising.
        const int32_t raw = ReadPcmChannel(block, chunk);
        outPcmFrame[ch] = Signed24ToFloat32(raw >> 8);
    }
}

/// Number of whole data blocks a received payload contains.
[[nodiscard]] constexpr uint32_t DataBlocksInPayload(size_t payloadBytes,
                                                     uint32_t dbs,
                                                     uint32_t cipHeaderBytes = 8U) noexcept {
    if (dbs == 0 || payloadBytes <= cipHeaderBytes) {
        return 0;
    }
    const size_t blockBytes = static_cast<size_t>(dbs) * 4U;
    return static_cast<uint32_t>((payloadBytes - cipHeaderBytes) / blockBytes);
}

/// Address the Nth data block within a received payload.
[[nodiscard]] inline std::span<const uint8_t> BlockAt(std::span<const uint8_t> payload,
                                                      uint32_t dbs,
                                                      uint32_t index,
                                                      uint32_t cipHeaderBytes = 8U) noexcept {
    const size_t blockBytes = static_cast<size_t>(dbs) * 4U;
    const size_t start = static_cast<size_t>(cipHeaderBytes) + index * blockBytes;
    if (dbs == 0 || start + blockBytes > payload.size()) {
        return {};
    }
    return payload.subspan(start, blockBytes);
}

} // namespace ASFW::Encoding::Motu
