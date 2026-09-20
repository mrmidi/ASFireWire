// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuPayloadWriter.hpp - Host->device PCM placement for MOTU protocol-v2 streams.
//
// This is the MOTU counterpart to AmdtpPayloadWriter. It shares that writer's timeline
// and slot-snapshot machinery -- packet exposure, frame-to-packet mapping and the
// retired-slot checks are format-neutral -- and differs only in how a frame is laid out
// inside a data block:
//
//     AMDTP/AM824          MOTU v2
//     -----------          -------
//     4-byte quadlet slot  3-byte chunk
//     slot index * 4       kPcmByteOffset (10) + chunk index * 3
//     (no per-block head)  32-bit SPH quadlet at block offset 0
//
// That is why MOTU cannot be a PcmSlotEncoding variant: PcmSlotCodec's interleaved
// helper writes uint32 slots, and MOTU's chunks are neither quadlet-sized nor
// quadlet-aligned. Layout cross-validated with Linux amdtp-motu.c:93-187.
//
// SPH is deliberately NOT written here. It carries presentation time replayed from the
// device's own capture stream, so it belongs with the packetizer that owns per-packet
// timing -- see MotuSph.hpp for the arithmetic and RxSequenceReplay.hpp for the source.
// This writer only places PCM.

#pragma once

#include "MotuBlockCodec.hpp"
#include "MotuBlockLayout.hpp"
#include "MotuPortLayout.hpp"
#include "../AMDTP/AmdtpPacketTimeline.hpp"
#include "../AMDTP/AmdtpTypes.hpp"
#include "../../Ports/IWirePayloadCodec.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::Encoding::Motu {

struct MotuPayloadWriterCounters final {
    std::atomic<uint64_t> framesVisited{0};
    std::atomic<uint64_t> framesWritten{0};
    /// Frames whose audio frame is past the exposed end -- no packet owns them yet.
    std::atomic<uint64_t> framesWithoutPacket{0};
    /// Frames behind the exposed end whose slot no longer maps them (already retired or
    /// re-used); distinct from framesWithoutPacket so a stall is diagnosable.
    std::atomic<uint64_t> framesOutsidePacket{0};
    /// Frames written into a data block whose declared chunk count cannot hold the
    /// configured channel count -- indicates a geometry/config mismatch, not a race.
    std::atomic<uint64_t> framesTruncated{0};
    std::atomic<uint64_t> framesNonZero{0};
};

struct MotuPayloadStreamConfig final {
    /// PCM chunks this direction carries per data block (fixed baseline plus any ADAT
    /// extras), as resolved by MotuV2Protocol::PrepareDuplex.
    uint32_t pcmChunks{0};
    /// First host buffer channel this stream encodes, mirroring
    /// AmdtpStreamConfig::sourceChannelOffset.
    uint32_t sourceChannelOffset{0};
    /// Chunk behind each host channel; empty encodes in wire order.
    MotuPortMap ports{};
};

class MotuPayloadWriter final : public ::ASFW::Audio::ITxPayloadWriter {
public:
    MotuPayloadWriter() noexcept = default;

    void Configure(const MotuPayloadStreamConfig& streamConfig) noexcept;
    void BindTimeline(Protocols::Audio::AMDTP::AmdtpPacketTimeline* timeline) noexcept;

    /// Place `hostBuffer`'s frames into whichever exposed packets own them. Frames with
    /// no owning packet are counted, not written; this is a best-effort producer running
    /// against a live DMA ring, exactly as the AMDTP writer is.
    void WriteFloat32Interleaved(
        const Protocols::Audio::AMDTP::HostAudioBufferView& hostBuffer,
        uint64_t completionCursor) noexcept override;

    [[nodiscard]] const MotuPayloadWriterCounters& Counters() const noexcept;

private:
    MotuPayloadStreamConfig streamConfig_{};
    Protocols::Audio::AMDTP::AmdtpPacketTimeline* timeline_{nullptr};
    MotuPayloadWriterCounters counters_{};
};

} // namespace ASFW::Encoding::Motu
