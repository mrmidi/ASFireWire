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
// SPH is deliberately NOT written here. It carries the presentation time of the SPH
// regulator, so it belongs with the timing stamper that runs as each packet is prepared
// (MotuTxTimingStamper, MotuDeviceTiming.hpp). This writer only places PCM, or silence
// while the regulator holds the output muted.

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
    /// Frames not written because their packet was below the first writable
    /// packet (the finality frontier): that packet keeps its armed silence.
    std::atomic<uint64_t> framesMissedFinality{0};
    /// Frames written as silence because the output was muted (SetPcmMuted).
    /// Included in framesWritten: the frame reached its packet, as zero.
    std::atomic<uint64_t> framesMuted{0};
};

struct MotuPayloadStreamConfig final {
    /// PCM chunks this direction carries per data block (fixed baseline plus any ADAT
    /// extras), as resolved by MotuProtocol::PrepareDuplex.
    uint32_t pcmChunks{0};
    /// First host buffer channel this stream encodes, mirroring
    /// AmdtpStreamConfig::sourceChannelOffset.
    uint32_t sourceChannelOffset{0};
    /// Chunk behind each host channel; empty encodes in wire order.
    MotuPortMap ports{};
    uint32_t pcmByteOffset{10};
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
        uint64_t firstWritablePacket) noexcept override;

    /// Write silence instead of the host's samples, into the same packets. The
    /// packets stay DATA: their cadence is the device's own, replayed from the
    /// receive stream, and only their content changes. The transmit producer
    /// sets this from the synthesizer's mute state before
    /// each packet it prepares; the gate holds the output silent until the
    /// regulator has locked.
    void SetPcmMuted(bool muted) noexcept;
    [[nodiscard]] bool IsPcmMuted() const noexcept;

    [[nodiscard]] const MotuPayloadWriterCounters& Counters() const noexcept;

private:
    MotuPayloadStreamConfig streamConfig_{};
    Protocols::Audio::AMDTP::AmdtpPacketTimeline* timeline_{nullptr};
    MotuPayloadWriterCounters counters_{};
    // Written on the transmit preparation queue, read by the IO thread's fill.
    std::atomic<bool> muted_{false};
};

} // namespace ASFW::Encoding::Motu
