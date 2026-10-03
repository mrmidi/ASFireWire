// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuV3TxSilenceDiagnostics.hpp - Where host->device silence comes from, for MOTU
// protocol-v3.
//
// The 828 Mk3 pass criterion is its meters and what you hear. When it stays silent,
// these instruments split the question by layer, per writer call:
//
//   host feeds silence     framesSourceZero, [TxInputZero]
//   servo holds the mute   the writer's FramesIntentionallyMuted, `muted=` below
//   frames never placed    the placement's partition counters, [TxChunkMap]
//   PCM zero on the wire   payloadZeroPackets, [TxPayloadZero]
//
// One check is not about silence: the two message chunks between the SPH quadlet and
// the PCM must stay zero, since nothing in ASFW sends V3 messages (no MIDI) and the
// AM824 silence the packetizer arms elsewhere would land there.
// `msgNZ=` on [TxChunkMap] is that count, cumulative for the stream.
//
// They live beside the V3 writer, so the shared MotuPayloadWriter stays untouched.
// A chunk past the block is reported by the placement's framesTruncated, and the
// exposure-window counters retired with TX_OWNERSHIP.md T8 have no equivalent here.
//
// "Signal" means a sample that encodes to a non-zero 24-bit value: anything quieter is
// silence on the wire too, and counting it as signal would make a packet look wrongly
// zeroed.
//
// Called by MotuV3PayloadWriter on the thread that fills TX payloads; every counter has
// that one writer. Log lines are ring-only and rate-limited.

#pragma once

#include "MotuBlockLayout.hpp"
#include "MotuPayloadWriter.hpp"
#include "MotuPortLayout.hpp"
#include "../AMDTP/AmdtpPacketTimeline.hpp"
#include "../AMDTP/AmdtpTypes.hpp"
#include "../AMDTP/PcmSlotCodec.hpp"
#include "../../../Logging/Logging.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Wire {

struct MotuV3TxSilenceCounters final {
    /// Host frames whose configured channels carry signal / carry none. Every frame
    /// of every call, placed or not: this is the source, before the wire.
    std::atomic<uint64_t> framesSourceNonZero{0};
    std::atomic<uint64_t> framesSourceZero{0};
    /// Silent runs of at least kZeroRunFrames inside a call that also carried signal:
    /// a hole in the music, not an idle stream.
    std::atomic<uint64_t> inputZeroRuns{0};
    /// Packets placed entirely by one call whose whole PCM area is zero.
    std::atomic<uint64_t> payloadZeroPackets{0};
    /// The subset that was not muted and whose source carried signal -- PCM lost
    /// between the host buffer and the packet. Zero on a healthy stream.
    std::atomic<uint64_t> payloadZeroPacketsWithSignal{0};
    /// Packets placed entirely by one call with a non-zero byte in either message
    /// chunk of any block. Zero on a healthy stream.
    std::atomic<uint64_t> payloadMessageNonZeroPackets{0};
};

/// The placement counters' change across one writer call.
struct MotuV3PlacementDelta final {
    uint64_t written{0};
    uint64_t withoutPacket{0};
    uint64_t outsidePacket{0};
    uint64_t missedFinality{0};
    uint64_t truncated{0};
};

class MotuV3TxSilenceDiagnostics final {
public:
    /// One normal DATA packet, so ordinary zero crossings never count as a run.
    static constexpr uint32_t kZeroRunFrames = 8;

    void Configure(const ::ASFW::Encoding::Motu::MotuPayloadStreamConfig& config) noexcept {
        config_ = config;
        ports_ = ::ASFW::Encoding::Motu::EffectivePortMap(config.ports, config.pcmChunks);
        // Unconditional freshness proof: the anomaly lines below are conditional, so
        // this one shows the V3 writer reached the active TX configuration.
        ASFW_LOG_RING_ONLY(DirectAudio, ::ASFW::Logging::LogLevel::Notice,
                           "[TxZeroTrace] v3 pcmCh=%u srcOffset=%u", config.pcmChunks,
                           config.sourceChannelOffset);
    }

    void BindTimeline(const Protocols::Audio::AMDTP::AmdtpPacketTimeline* timeline) noexcept {
        timeline_ = timeline;
    }

    /// After the placement wrote `hostBuffer` (as silence when `muted`).
    void Observe(const Protocols::Audio::AMDTP::HostAudioBufferView& hostBuffer,
                 uint64_t firstWritablePacket, bool muted,
                 const MotuV3PlacementDelta& placed) noexcept {
        if (timeline_ == nullptr || hostBuffer.interleavedFloat32 == nullptr ||
            hostBuffer.channels == 0 || hostBuffer.frameCount == 0 ||
            config_.pcmChunks == 0) {
            return; // the placement skipped this call too
        }

        uint64_t sourceNonZero = 0;
        uint64_t runStart = 0;
        uint64_t runFrames = 0;
        uint64_t longestStart = 0;
        uint64_t longestFrames = 0;
        for (uint32_t i = 0; i < hostBuffer.frameCount; ++i) {
            const uint64_t absoluteFrame = hostBuffer.firstFrame + i;
            if (SourceFrameHasSignal(hostBuffer, absoluteFrame)) {
                ++sourceNonZero;
                runFrames = 0;
                continue;
            }
            if (runFrames == 0) {
                runStart = absoluteFrame;
            }
            if (++runFrames > longestFrames) {
                longestFrames = runFrames;
                longestStart = runStart;
            }
        }
        const uint64_t sourceZero = hostBuffer.frameCount - sourceNonZero;

        uint64_t zeroPackets = 0;
        uint64_t zeroPacketsWithSignal = 0;
        uint64_t messageNonZeroPackets = 0;
        uint64_t firstZeroWithSignalPacket = 0;
        const uint64_t endFrame = hostBuffer.firstFrame + hostBuffer.frameCount;
        for (uint64_t frame = hostBuffer.firstFrame; frame < endFrame;) {
            Protocols::Audio::AMDTP::PacketSlotSnapshot snap{};
            if (!timeline_->SnapshotSlotForAudioFrame(frame, snap) ||
                snap.framesInPacket == 0) {
                ++frame;
                continue;
            }
            const uint64_t packetEnd = snap.firstAudioFrame + snap.framesInPacket;
            if (PacketPlacedByThisCall(snap, hostBuffer.firstFrame, endFrame,
                                       firstWritablePacket)) {
                const bool pcmZero = PacketPcmIsZero(snap);
                const bool messageNonZero = PacketMessageIsNonZero(snap);
                // Reuse recheck: a slot rewritten under
                // the read says nothing about what this call placed.
                std::atomic_thread_fence(std::memory_order_acquire);
                if ((pcmZero || messageNonZero) &&
                    snap.slot->generation.load(std::memory_order_relaxed) == snap.generation) {
                    if (pcmZero) {
                        ++zeroPackets;
                        if (!muted && PacketSourceHasSignal(hostBuffer, snap)) {
                            if (zeroPacketsWithSignal++ == 0) {
                                firstZeroWithSignalPacket = snap.packetIndex;
                            }
                        }
                    }
                    if (messageNonZero) {
                        ++messageNonZeroPackets;
                    }
                }
            }
            frame = packetEnd > frame ? packetEnd : frame + 1;
        }

        const bool inputHole = sourceNonZero != 0 && longestFrames >= kZeroRunFrames;
        counters_.framesSourceNonZero.fetch_add(sourceNonZero, std::memory_order_relaxed);
        counters_.framesSourceZero.fetch_add(sourceZero, std::memory_order_relaxed);
        counters_.inputZeroRuns.fetch_add(inputHole ? 1U : 0U, std::memory_order_relaxed);
        counters_.payloadZeroPackets.fetch_add(zeroPackets, std::memory_order_relaxed);
        counters_.payloadZeroPacketsWithSignal.fetch_add(zeroPacketsWithSignal,
                                                         std::memory_order_relaxed);
        const uint64_t messageNonZeroTotal =
            counters_.payloadMessageNonZeroPackets.fetch_add(messageNonZeroPackets,
                                                             std::memory_order_relaxed) +
            messageNonZeroPackets;

        // What this call saw, every ~2 s. Kept under LogRecord's 232-byte message.
        ASFW_LOG_RING_ONLY_RL(
            DirectAudio, "tx-chunk-map", 2000u, ::ASFW::Logging::LogLevel::Notice,
            "[TxChunkMap] frames=%u written=%llu withoutPkt=%llu outsidePkt=%llu "
            "missedFin=%llu trunc=%llu muted=%u srcNZ=%llu zeroPkt=%llu msgNZ=%llu "
            "first=%llu exposedEnd=%llu",
            hostBuffer.frameCount, placed.written, placed.withoutPacket,
            placed.outsidePacket, placed.missedFinality, placed.truncated,
            muted ? 1U : 0U, sourceNonZero, zeroPackets, messageNonZeroTotal,
            hostBuffer.firstFrame,
            timeline_->ExposedFrameEnd());

        if (inputHole) {
            ASFW_LOG_RING_ONLY_RL(
                DirectAudio, "tx-input-zero-run", 250u, ::ASFW::Logging::LogLevel::Notice,
                "[TxInputZero] range=[%llu,%llu) frames=%llu srcNZ=%llu written=%llu",
                longestStart, longestStart + longestFrames, longestFrames, sourceNonZero,
                placed.written);
        }
        if (zeroPacketsWithSignal != 0) {
            ASFW_LOG_RING_ONLY_RL(
                DirectAudio, "tx-payload-zero", 250u, ::ASFW::Logging::LogLevel::Error,
                "[TxPayloadZero] packets=%llu withSignal=%llu firstPkt=%llu srcNZ=%llu "
                "srcZero=%llu written=%llu",
                zeroPackets, zeroPacketsWithSignal, firstZeroWithSignalPacket,
                sourceNonZero, sourceZero, placed.written);
        }
    }

    [[nodiscard]] const MotuV3TxSilenceCounters& Counters() const noexcept {
        return counters_;
    }

private:
    [[nodiscard]] static bool SampleIsSignal(float sample) noexcept {
        return Protocols::Audio::AMDTP::PcmSlotCodec::Float32ToSigned24(sample) != 0;
    }

    [[nodiscard]] bool SourceFrameHasSignal(
        const Protocols::Audio::AMDTP::HostAudioBufferView& hostBuffer,
        uint64_t absoluteFrame) const noexcept {
        // Same frame and channel addressing as the placement (MotuPayloadWriter.cpp).
        const uint64_t sourceFrame = hostBuffer.frameCapacity != 0
                                         ? absoluteFrame % hostBuffer.frameCapacity
                                         : absoluteFrame - hostBuffer.firstFrame;
        const float* const source =
            hostBuffer.interleavedFloat32 + sourceFrame * hostBuffer.channels;
        for (uint32_t hostCh = 0; hostCh < config_.pcmChunks; ++hostCh) {
            const uint32_t srcCh = config_.sourceChannelOffset + hostCh;
            if (srcCh < hostBuffer.channels && SampleIsSignal(source[srcCh])) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool PacketSourceHasSignal(
        const Protocols::Audio::AMDTP::HostAudioBufferView& hostBuffer,
        const Protocols::Audio::AMDTP::PacketSlotSnapshot& snap) const noexcept {
        for (uint32_t frame = 0; frame < snap.framesInPacket; ++frame) {
            if (SourceFrameHasSignal(hostBuffer, snap.firstAudioFrame + frame)) {
                return true;
            }
        }
        return false;
    }

    /// Only a packet this call wrote from its first frame to its last says anything
    /// about this call: its other frames came from another call or never came.
    [[nodiscard]] bool PacketPlacedByThisCall(
        const Protocols::Audio::AMDTP::PacketSlotSnapshot& snap, uint64_t firstFrame,
        uint64_t endFrame, uint64_t firstWritablePacket) const noexcept {
        if (snap.firstAudioFrame < firstFrame ||
            snap.firstAudioFrame + snap.framesInPacket > endFrame) {
            return false;
        }
        if (firstWritablePacket != 0 &&
            static_cast<int64_t>(snap.packetIndex - firstWritablePacket) < 0) {
            return false; // behind the finality frontier: the placement skipped it
        }
        const uint32_t required = ::ASFW::Encoding::Motu::kPcmByteOffset +
                                  config_.pcmChunks * ::ASFW::Encoding::Motu::kBytesPerChunk;
        return required <= snap.dbs * 4U; // otherwise counted as truncated
    }

    [[nodiscard]] bool PacketPcmIsZero(
        const Protocols::Audio::AMDTP::PacketSlotSnapshot& snap) const noexcept {
        constexpr uint32_t kCipHeaderBytes = 8;
        const uint32_t blockBytes = snap.dbs * 4U;
        for (uint32_t frame = 0; frame < snap.framesInPacket; ++frame) {
            const uint8_t* const block =
                snap.packetBytes + kCipHeaderBytes + frame * blockBytes;
            for (uint32_t hostCh = 0; hostCh < config_.pcmChunks; ++hostCh) {
                const uint8_t* const sample =
                    block + ::ASFW::Encoding::Motu::kPcmByteOffset +
                    ::ASFW::Encoding::Motu::ChunkForHostChannel(ports_, hostCh) *
                        ::ASFW::Encoding::Motu::kBytesPerChunk;
                if (sample[0] != 0 || sample[1] != 0 || sample[2] != 0) {
                    return false;
                }
            }
        }
        return true;
    }

    /// The message chunks sit between the SPH quadlet and the first PCM chunk.
    [[nodiscard]] static bool PacketMessageIsNonZero(
        const Protocols::Audio::AMDTP::PacketSlotSnapshot& snap) noexcept {
        constexpr uint32_t kCipHeaderBytes = 8;
        constexpr uint32_t kMessageEnd = ::ASFW::Encoding::Motu::kPcmByteOffset;
        constexpr uint32_t kMessageBegin =
            kMessageEnd - ::ASFW::Encoding::Motu::kMsgChunks *
                              ::ASFW::Encoding::Motu::kBytesPerChunk;
        const uint32_t blockBytes = snap.dbs * 4U;
        if (blockBytes < kMessageEnd) {
            return false; // counted as truncated by the placement
        }
        for (uint32_t frame = 0; frame < snap.framesInPacket; ++frame) {
            const uint8_t* const block =
                snap.packetBytes + kCipHeaderBytes + frame * blockBytes;
            for (uint32_t i = kMessageBegin; i < kMessageEnd; ++i) {
                if (block[i] != 0) {
                    return true;
                }
            }
        }
        return false;
    }

    ::ASFW::Encoding::Motu::MotuPayloadStreamConfig config_{};
    ::ASFW::Encoding::Motu::MotuPortMap ports_{};
    const Protocols::Audio::AMDTP::AmdtpPacketTimeline* timeline_{nullptr};
    MotuV3TxSilenceCounters counters_{};
};

} // namespace ASFW::Audio::Wire
