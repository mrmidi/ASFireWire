// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../../Ports/IWirePayloadCodec.hpp"
#include "../AM824/AM824Decoder.hpp"
#include <cstdint>

namespace ASFW::Audio::Wire {

class RawPcm24In32RxPayloadCodec final : public IRxPayloadCodec {
public:
    explicit RawPcm24In32RxPayloadCodec(uint32_t am824Slots = 0, bool trustConfiguredStride = false) noexcept
        : am824Slots_(am824Slots), trustConfiguredStride_(trustConfiguredStride) {}

    void Configure(uint32_t am824Slots, bool trustConfiguredStride) noexcept {
        am824Slots_ = am824Slots;
        trustConfiguredStride_ = trustConfiguredStride;
    }

    [[nodiscard]] uint32_t StrideQuadlets(uint8_t cipDbs) const noexcept override {
        return trustConfiguredStride_ ? am824Slots_ : static_cast<uint32_t>(cipDbs);
    }

    [[nodiscard]] bool ValidateGeometry(
        uint32_t channels,
        uint32_t channelOffset,
        uint32_t strideQuadlets,
        uint8_t cipDbs) const noexcept override {
        (void)channelOffset;
        if (channels == 0) return false;
        if (strideQuadlets < channels) return false;
        if (!trustConfiguredStride_ && am824Slots_ != static_cast<uint32_t>(cipDbs)) return false;
        return true;
    }

    void DecodeBlock(
        std::span<const uint8_t> blockBytes,
        uint32_t channels,
        uint32_t channelOffset,
        const AudioEngine::Direct::Rx::RxCaptureChannelMap& map,
        float* frameOut,
        float* delayedOut) const noexcept override {
        (void)channelOffset;
        const auto* frameIn = reinterpret_cast<const uint32_t*>(blockBytes.data());
        for (uint32_t ch = 0; ch < channels; ++ch) {
            float* destination = frameOut;
            if (map.IsDelayed(ch)) {
                if (delayedOut == nullptr) continue;
                destination = delayedOut;
            }
            const uint32_t quadlet = frameIn[map.SlotFor(ch)];
            destination[ch] = DecodeSlotToFloat32(quadlet);
        }
    }

    [[nodiscard]] static float DecodeSlotToFloat32(uint32_t quadlet) noexcept {
        const uint32_t hostQuadlet = OSSwapBigToHostInt32(quadlet);
        const uint32_t labeledHostQuadlet = 0x40000000u | (hostQuadlet & 0x00FFFFFFu);
        auto sample = ASFW::Isoch::AM824Decoder::DecodeSample(OSSwapHostToBigInt32(labeledHostQuadlet));
        if (!sample) return 0.0f;
        if (*sample <= -8388608) return -1.0f;
        return static_cast<float>(*sample) / 8388607.0f;
    }

private:
    uint32_t am824Slots_{0};
    bool trustConfiguredStride_{false};
};

} // namespace ASFW::Audio::Encoding
