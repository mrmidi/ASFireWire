// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../../Ports/IWirePayloadCodec.hpp"
#include "MotuBlockCodec.hpp"
#include "MotuBlockLayout.hpp"
#include "MotuPayloadReader.hpp"
#include "MotuPortLayout.hpp"

#include <cstdint>
#include <span>

namespace ASFW::Audio::Wire {

class MotuRxPayloadCodec final : public IRxPayloadCodec {
public:
    MotuRxPayloadCodec() noexcept = default;

    explicit MotuRxPayloadCodec(uint32_t pcmChunks,
                                ::ASFW::Encoding::Motu::MotuPortMap ports = {}) noexcept
        : pcmChunks_(pcmChunks), ports_(ports) {}

    void Configure(uint32_t pcmChunks,
                   ::ASFW::Encoding::Motu::MotuPortMap ports = {}) noexcept {
        pcmChunks_ = pcmChunks;
        ports_ = ports;
    }

    [[nodiscard]] uint32_t StrideQuadlets(uint8_t cipDbs) const noexcept override {
        return pcmChunks_ != 0
            ? ::ASFW::Encoding::Motu::DataBlockQuadlets(pcmChunks_)
            : static_cast<uint32_t>(cipDbs);
    }

    [[nodiscard]] bool ValidateGeometry(
        uint32_t channels,
        uint32_t channelOffset,
        uint32_t strideQuadlets,
        uint8_t cipDbs) const noexcept override {
        (void)cipDbs;
        if (channels == 0 || pcmChunks_ == 0 || channelOffset + channels > pcmChunks_) {
            return false;
        }
        const size_t dbsBytes = static_cast<size_t>(strideQuadlets) * 4U;
        const size_t requiredBytes =
            static_cast<size_t>(::ASFW::Encoding::Motu::kPcmByteOffset) +
            static_cast<size_t>(pcmChunks_) * ::ASFW::Encoding::Motu::kBytesPerChunk;
        if (requiredBytes > dbsBytes) {
            return false;
        }
        return true;
    }

    void DecodeBlock(
        std::span<const uint8_t> blockBytes,
        uint32_t channels,
        uint32_t channelOffset,
        const AudioEngine::Direct::Rx::RxCaptureChannelMap& map,
        float* frameOut,
        float* delayedOut) const noexcept override {
        ::ASFW::Encoding::Motu::DecodeMotuBlock(
            blockBytes, pcmChunks_, channelOffset, frameOut, channels, ports_);

        if (delayedOut != nullptr && map.HasDelay()) {
            for (uint32_t ch = 0; ch < channels; ++ch) {
                if (map.IsDelayed(ch)) {
                    delayedOut[ch] = frameOut[ch];
                    frameOut[ch] = 0.0f;
                }
            }
        }
    }

private:
    uint32_t pcmChunks_{0};
    ::ASFW::Encoding::Motu::MotuPortMap ports_{};
};

} // namespace ASFW::Audio::Wire
