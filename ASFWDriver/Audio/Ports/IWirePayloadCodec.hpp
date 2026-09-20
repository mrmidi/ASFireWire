// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../Wire/AMDTP/AmdtpTypes.hpp"
#include "../Engine/Direct/Rx/RxCaptureChannelMap.hpp"
#include <span>
#include <cstdint>

namespace ASFW::Audio::Runtime {
class IDirectAudioBindingSource;
}

namespace ASFW::Audio {

/// Decoding seam for receive (device -> host) packet audio blocks.
class IRxPayloadCodec {
public:
    virtual ~IRxPayloadCodec() = default;

    /// Compute stride in quadlets for one data block given the wire CIP DBS.
    [[nodiscard]] virtual uint32_t StrideQuadlets(uint8_t cipDbs) const noexcept = 0;

    /// Validate whether this data block geometry can be decoded for the requested channels.
    [[nodiscard]] virtual bool ValidateGeometry(
        uint32_t channels,
        uint32_t channelOffset,
        uint32_t strideQuadlets,
        uint8_t cipDbs) const noexcept = 0;

    /// Decode one data block into the destination PCM frame buffer.
    virtual void DecodeBlock(
        std::span<const uint8_t> blockBytes,
        uint32_t channels,
        uint32_t channelOffset,
        const AudioEngine::Direct::Rx::RxCaptureChannelMap& map,
        float* frameOut,
        float* delayedOut) const noexcept = 0;
};

/// Placement seam for transmit (host -> device) PCM into packet slots.
class ITxPayloadWriter {
public:
    virtual ~ITxPayloadWriter() = default;

    virtual void WriteFloat32Interleaved(
        const Protocols::Audio::AMDTP::HostAudioBufferView& hostBuffer,
        uint64_t completionCursor) noexcept = 0;
};

/// Outcome of physical timing stamping.
enum class TxTimingStampResult : uint8_t {
    kOk = 0,
    kTimingUnavailable,
    kNotApplicable,
};

/// Physical transmit timing stamper (e.g. SPH quadlets).
class ITxDeviceTimingStamper {
public:
    virtual ~ITxDeviceTimingStamper() = default;

    virtual TxTimingStampResult StampPacket(
        const Protocols::Audio::AMDTP::TxPacketSlotView& slot,
        const Protocols::Audio::AMDTP::PreparedTxPacket& packet,
        const Protocols::Audio::AMDTP::AmdtpTimingState& timing) noexcept = 0;

    [[nodiscard]] virtual bool IsSytUnaware() const noexcept = 0;
};

/// Physical receive timing observer (e.g. SPH cache capture).
class IRxDeviceTimingObserver {
public:
    virtual ~IRxDeviceTimingObserver() = default;

    virtual void OnBatchBegin(Runtime::IDirectAudioBindingSource* bindingSource) noexcept {
        (void)bindingSource;
    }

    virtual void ObserveRawPacket(
        uint64_t epoch,
        uint16_t rxTimestamp,
        std::span<const uint8_t> rawPayload) noexcept {
        (void)epoch;
        (void)rxTimestamp;
        (void)rawPayload;
    }

    virtual void ObservePacket(
        std::span<const uint8_t> payload,
        uint32_t strideQuadlets,
        uint32_t framesDecoded,
        uint16_t cycle) noexcept = 0;

    [[nodiscard]] virtual bool IsTimingEstablished() const noexcept = 0;
    virtual void Reset() noexcept = 0;
};

} // namespace ASFW::Audio
