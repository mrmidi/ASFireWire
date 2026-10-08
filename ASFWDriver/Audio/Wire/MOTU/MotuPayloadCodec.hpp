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
                                ::ASFW::Encoding::Motu::MotuPortMap ports = {}, bool v3 = false, uint32_t messageChunks = 2, uint32_t pcmByteOffset = 10) noexcept
        : v3_(v3), pcmChunks_(pcmChunks), ports_(ports), messageChunks_(messageChunks), pcmByteOffset_(pcmByteOffset) {}

    void Configure(uint32_t pcmChunks,
                   ::ASFW::Encoding::Motu::MotuPortMap ports = {}) noexcept {
        pcmChunks_ = pcmChunks;
        ports_ = ports;
    }

    [[nodiscard]] std::optional<Isoch::CIPHeader> DecodeHeader(uint32_t q0BE, uint32_t q1BE) const noexcept override {
        if (const auto strict = IRxPayloadCodec::DecodeHeader(q0BE, q1BE)) return strict;
        // 828mk3 capture: PR #172 documentation/MOTU_828MK3.md:46-56.
        // EOH1 is clear and 0x22ffffff is a vendor word, not FMT/FDF/SYT.
        if (!v3_ || OSSwapBigToHostInt32(q1BE) != 0x22ffffff ||
            !(OSSwapBigToHostInt32(q0BE) & 0x400)) return std::nullopt;
        return Isoch::CIPHeader::Decode(q0BE, OSSwapHostToBigInt32(0x8222ffff));
    }

    [[nodiscard]] uint32_t StrideQuadlets(uint8_t cipDbs) const noexcept override {
        return pcmChunks_ != 0
            ? ::ASFW::Encoding::Motu::DataBlockQuadlets(pcmChunks_, messageChunks_)
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
            static_cast<size_t>(pcmByteOffset_) +
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
            blockBytes, pcmChunks_, channelOffset, frameOut, channels, ports_, pcmByteOffset_);

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
    bool v3_{false};
    uint32_t messageChunks_{2};
    uint32_t pcmByteOffset_{10};
    uint32_t pcmChunks_{0};
    ::ASFW::Encoding::Motu::MotuPortMap ports_{};
};

} // namespace ASFW::Audio::Wire
