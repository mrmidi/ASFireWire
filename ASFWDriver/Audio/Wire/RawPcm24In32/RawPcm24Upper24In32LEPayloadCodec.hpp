// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../../Ports/IWirePayloadCodec.hpp"

namespace ASFW::Audio::Wire {

/// Headerless RME-style PCM: one little-endian 32-bit quadlet per channel,
/// signed 24-bit audio in bits 31:8. The low byte is ignored on capture.
/// Sample packing follows Linux Fireface `write_pcm_s32`/`read_pcm_s32`, which
/// copies LE32 playback words and masks capture to 0xffffff00
/// (references/linux-sound-firewire-stack/firewire/fireface/amdtp-ff.c:31-87).
class RawPcm24Upper24In32LEPayloadCodec final : public IRxPayloadCodec {
public:
    explicit RawPcm24Upper24In32LEPayloadCodec(uint32_t slots = 0) noexcept
        : slots_(slots) {}

    void Configure(uint32_t slots) noexcept { slots_ = slots; }

    [[nodiscard]] uint32_t StrideQuadlets(uint8_t) const noexcept override {
        return slots_;
    }

    [[nodiscard]] bool ValidateGeometry(uint32_t channels, uint32_t,
                                        uint32_t strideQuadlets,
                                        uint8_t) const noexcept override {
        return channels != 0 && strideQuadlets >= channels &&
               strideQuadlets == slots_;
    }

    void DecodeBlock(std::span<const uint8_t> blockBytes,
                     uint32_t channels, uint32_t,
                     const AudioEngine::Direct::Rx::RxCaptureChannelMap& map,
                     float* frameOut, float* delayedOut) const noexcept override {
        if (frameOut == nullptr) return;
        const bool mapUsable = map.FitsWithin(channels, slots_);
        for (uint32_t ch = 0; ch < channels; ++ch) {
            float* destination = frameOut;
            if (map.IsDelayed(ch)) {
                if (delayedOut == nullptr) continue;
                destination = delayedOut;
            }
            const uint32_t slot = mapUsable ? map.SlotFor(ch) : ch;
            const size_t offset = static_cast<size_t>(slot) * 4U;
            if (offset + 4U > blockBytes.size()) return;
            const uint32_t word = static_cast<uint32_t>(blockBytes[offset]) |
                                  (static_cast<uint32_t>(blockBytes[offset + 1]) << 8U) |
                                  (static_cast<uint32_t>(blockBytes[offset + 2]) << 16U) |
                                  (static_cast<uint32_t>(blockBytes[offset + 3]) << 24U);
            const uint32_t raw24 = word >> 8U;
            const int32_t sample = (raw24 & 0x00800000U) != 0
                                       ? static_cast<int32_t>(raw24) - 0x01000000
                                       : static_cast<int32_t>(raw24);
            destination[ch] = sample <= -8388608
                                  ? -1.0f
                                  : static_cast<float>(sample) / 8388607.0f;
        }
    }

private:
    uint32_t slots_{0};
};

} // namespace ASFW::Audio::Wire
