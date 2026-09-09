// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuPayloadWriter.cpp - see MotuPayloadWriter.hpp.

#include "MotuPayloadWriter.hpp"

#include "../AMDTP/PcmSlotCodec.hpp"

namespace ASFW::Encoding::Motu {

namespace {

using ASFW::Protocols::Audio::AMDTP::PacketSlotSnapshot;
using ASFW::Protocols::Audio::AMDTP::PcmSlotCodec;

/// CIP header precedes the first data block, same as every other 61883-6 stream.
constexpr uint32_t kCipHeaderBytes = 8;

/// MotuBlockCodec::WritePcmSample takes the sample in the 24 significant MSBs of an
/// int32 (amdtp-motu.c:150-156), while PcmSlotCodec::Float32ToSigned24 returns it in the
/// low 24 bits. Shift bridges the two conventions.
[[nodiscard]] inline int32_t Float32ToMotuSample(float sample) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(PcmSlotCodec::Float32ToSigned24(sample))
                                << 8);
}

} // namespace

void MotuPayloadWriter::Configure(const MotuPayloadStreamConfig& streamConfig) noexcept {
    streamConfig_ = streamConfig;
}

void MotuPayloadWriter::BindTimeline(
    Protocols::Audio::AMDTP::AmdtpPacketTimeline* timeline) noexcept {
    timeline_ = timeline;
}

const MotuPayloadWriterCounters& MotuPayloadWriter::Counters() const noexcept {
    return counters_;
}

void MotuPayloadWriter::WriteFloat32Interleaved(
    const Protocols::Audio::AMDTP::HostAudioBufferView& hostBuffer,
    uint64_t completionCursor) noexcept {
    (void)completionCursor;

    if (timeline_ == nullptr || hostBuffer.interleavedFloat32 == nullptr ||
        hostBuffer.channels == 0 || hostBuffer.frameCount == 0 ||
        streamConfig_.pcmChunks == 0) {
        return; // invalid view or unconfigured geometry: nothing visited, nothing counted
    }

    uint64_t visited = 0;
    uint64_t written = 0;
    uint64_t withoutPacket = 0;
    uint64_t outsidePacket = 0;
    uint64_t truncated = 0;
    uint64_t nonZeroFrames = 0;

    for (uint32_t i = 0; i < hostBuffer.frameCount; ++i) {
        const uint64_t absoluteFrame = hostBuffer.firstFrame + i;
        ++visited;

        PacketSlotSnapshot snap{};
        if (!timeline_->SnapshotSlotForAudioFrame(absoluteFrame, snap)) {
            // Past the exposed end means "no packet owns this yet" (normal lead);
            // behind it means the slot was retired or re-used under us.
            if (absoluteFrame >= timeline_->ExposedFrameEnd()) {
                ++withoutPacket;
            } else {
                ++outsidePacket;
            }
            continue;
        }

        const uint64_t sourceFrame = hostBuffer.frameCapacity != 0
                                         ? absoluteFrame % hostBuffer.frameCapacity
                                         : i;
        const float* const source =
            hostBuffer.interleavedFloat32 + sourceFrame * hostBuffer.channels;

        // The snapshot's range check guarantees firstAudioFrame <= absoluteFrame <
        // firstAudioFrame + framesInPacket, so this cannot underflow.
        const uint32_t frameInPacket = static_cast<uint32_t>(absoluteFrame - snap.firstAudioFrame);

        // dbs is the data block size in quadlets, which is exactly what
        // DataBlockQuadlets() produces for this chunk count -- one SPH quadlet plus the
        // message and PCM chunks, padded to quadlet alignment.
        const uint32_t blockBytes = snap.dbs * 4U;
        uint8_t* const block = snap.packetBytes + kCipHeaderBytes + frameInPacket * blockBytes;

        // Refuse to write past the block. A short block means the configured chunk count
        // disagrees with the packet geometry; writing anyway would corrupt the next
        // block's SPH.
        const uint32_t requiredBytes =
            kPcmByteOffset + streamConfig_.pcmChunks * kBytesPerChunk;
        if (requiredBytes > blockBytes) {
            ++truncated;
            continue;
        }

        const uint32_t srcOffset = streamConfig_.sourceChannelOffset;
        bool frameNonZero = false;
        for (uint32_t chunk = 0; chunk < streamConfig_.pcmChunks; ++chunk) {
            const uint32_t srcCh = srcOffset + chunk;
            // Chunks beyond the host buffer encode PCM zero -- MOTU's fixed chunk count
            // always covers every physical port, including ones CoreAudio is not
            // driving, and those must carry silence rather than stale bytes.
            const float sample = (srcCh < hostBuffer.channels) ? source[srcCh] : 0.0f;
            WritePcmSample(
                std::span<uint8_t>(block + kPcmByteOffset + chunk * kBytesPerChunk,
                                   kBytesPerChunk),
                Float32ToMotuSample(sample));
            if (sample != 0.0f) {
                frameNonZero = true;
            }
        }

        ++written;
        if (frameNonZero) {
            ++nonZeroFrames;
        }
    }

    counters_.framesVisited.fetch_add(visited, std::memory_order_relaxed);
    counters_.framesWritten.fetch_add(written, std::memory_order_relaxed);
    counters_.framesWithoutPacket.fetch_add(withoutPacket, std::memory_order_relaxed);
    counters_.framesOutsidePacket.fetch_add(outsidePacket, std::memory_order_relaxed);
    counters_.framesTruncated.fetch_add(truncated, std::memory_order_relaxed);
    counters_.framesNonZero.fetch_add(nonZeroFrames, std::memory_order_relaxed);
}

} // namespace ASFW::Encoding::Motu
