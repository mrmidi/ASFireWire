#include "AmdtpPayloadWriter.hpp"

#include "PcmSlotCodec.hpp"

#include <atomic>
namespace ASFW::Protocols::Audio::AMDTP {

// Design decisions (see ../../../README.md, Step 4):
//
// 1. Reference model (one buffer, absolute sample frame): PCM lands in
//    already-exposed packets located via SnapshotSlotForAudioFrame. The
//    writer never creates, publishes, or retires packets — slot lifecycle
//    stays with the timeline/packetizer/provider. It is the TX fill: the
//    driver calls it from the CoreAudio WriteEnd step with frames already in
//    the HAL ring, once each (documentation/TX_OWNERSHIP.md §1d).
// 2. Per-frame count-and-skip: a partially coverable window is never
//    rejected wholesale; each frame is individually written or counted into
//    exactly one miss bucket, so framesVisited == framesWritten +
//    framesWithoutPacket + framesOutsidePacket + framesMissedFinality holds
//    by construction. A frame whose packet is below the caller's first
//    writable packet is not written (framesMissedFinality): that packet may
//    already be with the hardware and keeps its armed silence. The counters are the diagnostic payload (the lab
//    analog of the ASFW pcmNZ/pcmZero decider).
// 3. Miss classification uses the timeline's monotonic exposure high-water
//    mark (ExposedFrameEnd): at or beyond it the packet does not exist yet
//    (framesWithoutPacket — writer ran ahead of the packetizer); below it
//    the packet existed but is no longer writable: published, retired,
//    evicted by ring reuse, or mid-rewrite when the seqlock snapshot was
//    taken (framesOutsidePacket — writer arrived late).
// 4. The host view is a window into a ring of frameCapacity frames;
//    interleavedFloat32 points at the mapped HAL ring base and reads wrap
//    modulo frameCapacity.
//    frameCapacity == 0 degrades to a flat (non-ring) buffer.
// 5. The codec returns logical quadlet values; the writer serializes them
//    big-endian into the packet image (the LE encoding pre-swaps so that BE
//    serialization yields little-endian sample bytes — see PcmSlotCodec).
// 6. Channel policy: host channels beyond pcmChannels are dropped; missing
//    host channels encode PCM zero; non-PCM slots (MIDI etc.) are never
//    touched and keep the packetizer's defaults.
// 7. Counters accumulate locally and publish once per call with relaxed
//    atomics -- no per-frame RMW traffic in the fill loop.
// 8. The fill runs on CoreAudio's IO thread while the producer arms and
//    recycles slots on the TX preparation queue. Slot fields come from a
//    PacketSlotSnapshot validated under the generation seqlock. The fill
//    writes only packets at or beyond the finality frontier, and a slot is
//    re-armed one shared-slot ring (1512 packets) later, so a reuse during
//    one frame's write would need a stall of more than 60 ms inside it.

namespace {

constexpr uint32_t kBytesPerSlot = 4;

inline void WriteBE32(uint8_t* dest, uint32_t value) noexcept {
    dest[0] = static_cast<uint8_t>(value >> 24);
    dest[1] = static_cast<uint8_t>(value >> 16);
    dest[2] = static_cast<uint8_t>(value >> 8);
    dest[3] = static_cast<uint8_t>(value);
}

} // namespace

void AmdtpPayloadWriter::Configure(const AmdtpStreamConfig& streamConfig,
                                   const AmdtpTxPolicy& txPolicy) noexcept {
    streamConfig_ = streamConfig;
    txPolicy_ = txPolicy;
}

void AmdtpPayloadWriter::BindTimeline(AmdtpPacketTimeline* timeline) noexcept {
    timeline_ = timeline;
}

void AmdtpPayloadWriter::WriteFloat32Interleaved(
    const HostAudioBufferView& hostBuffer,
    uint64_t firstWritablePacket) noexcept {
    if (timeline_ == nullptr || hostBuffer.interleavedFloat32 == nullptr ||
        hostBuffer.channels == 0 || hostBuffer.frameCount == 0) {
        return; // invalid view: nothing visited, nothing counted
    }

    uint64_t written = 0;
    uint64_t withoutPacket = 0;
    uint64_t outsidePacket = 0;
    uint64_t missedFinality = 0;
    int64_t minMarginPackets = INT64_MAX;
    float peak = 0.0f;
    uint32_t peakWord = 0;

    // Sequential frames share a packet. Avoid a full timeline scan for each
    // sample on the HAL IO thread (16 identical scans per packet at 96 kHz).
    // Revalidate generation/state before reuse; a retired or re-armed slot
    // goes through the normal lookup and miss classification.
    PacketSlotSnapshot snap{};
    bool haveSnapshot = false;
    for (uint32_t i = 0; i < hostBuffer.frameCount; ++i) {
        const uint64_t absoluteFrame = hostBuffer.firstFrame + i;

        const bool reuseSnapshot = haveSnapshot &&
            absoluteFrame >= snap.firstAudioFrame &&
            absoluteFrame - snap.firstAudioFrame < snap.framesInPacket &&
            snap.slot->generation.load(std::memory_order_acquire) == snap.generation &&
            snap.slot->state.load(std::memory_order_acquire) == PacketSlotState::ExposedForAudio;
        if (!reuseSnapshot) {
            haveSnapshot = timeline_->SnapshotSlotForAudioFrame(absoluteFrame, snap);
        }
        if (!haveSnapshot) {
            if (absoluteFrame >= timeline_->ExposedFrameEnd()) {
                ++withoutPacket;
            } else {
                ++outsidePacket;
            }
            continue;
        }

        if (firstWritablePacket != 0) {
            const auto margin = static_cast<int64_t>(snap.packetIndex - firstWritablePacket);
            if (margin < 0) {
                ++missedFinality;
                continue;
            }
            if (margin < minMarginPackets) {
                minMarginPackets = margin;
            }
        }

        const uint64_t sourceFrame =
            hostBuffer.frameCapacity != 0
                ? absoluteFrame % hostBuffer.frameCapacity
                : i;
        const float* source =
            hostBuffer.interleavedFloat32 +
            sourceFrame * hostBuffer.channels;

        // The snapshot's range check guarantees firstAudioFrame <=
        // absoluteFrame < firstAudioFrame + framesInPacket on a consistent
        // field set, so this cannot underflow.
        const uint32_t frameInPacket =
            static_cast<uint32_t>(absoluteFrame - snap.firstAudioFrame);
        const uint32_t headerBytes = streamConfig_.packetFraming ==
                                             AmdtpStreamConfig::PacketFraming::Cip
                                         ? 8U : 0U;
        uint8_t* dest = snap.packetBytes + headerBytes +
                        frameInPacket * snap.dbs * kBytesPerSlot;

        const uint32_t pcmSlots = (streamConfig_.pcmChannels < snap.dbs)
                                      ? streamConfig_.pcmChannels
                                      : snap.dbs;
        // A map sized for a different formation than the packet actually carries
        // would write past the data blocks. Fall back to the wire order (identity) instead:
        // writing in device order is recoverable, writing out of bounds into
        // another frame or beyond the packet is not.
        const bool mapUsable = txPolicy_.playbackChannelMap.FitsWithin(pcmSlots, snap.dbs);
        const auto& playbackMap = txPolicy_.playbackChannelMap;

        // This stream encodes the host channels [sourceChannelOffset,
        // sourceChannelOffset + pcmChannels) of the shared interleaved buffer —
        // the de-interleave that mirrors the RX side's channelOffset. Host
        // channels outside the buffer encode PCM zero.
        const uint32_t srcOffset = streamConfig_.sourceChannelOffset;
        for (uint32_t ch = 0; ch < pcmSlots; ++ch) {
            const uint32_t srcCh = srcOffset + ch;
            const float sample =
                (srcCh < hostBuffer.channels) ? source[srcCh] : 0.0f;
            const uint32_t slot = mapUsable ? playbackMap.SlotFor(ch) : ch;
            const uint32_t word =
                PcmSlotCodec::EncodeFloat32(sample, txPolicy_.hostToDevicePcmEncoding);
            WriteBE32(dest + slot * kBytesPerSlot, word);
            const float magnitude = sample < 0.0f ? -sample : sample;
            if (magnitude > peak) {
                peak = magnitude;
                peakWord = word;
            }
        }
        ++written;
    }

    counters_.framesVisited.fetch_add(hostBuffer.frameCount,
                                      std::memory_order_relaxed);
    counters_.framesWritten.fetch_add(written, std::memory_order_relaxed);
    counters_.framesWithoutPacket.fetch_add(withoutPacket,
                                            std::memory_order_relaxed);
    counters_.framesOutsidePacket.fetch_add(outsidePacket,
                                            std::memory_order_relaxed);
    if (minMarginPackets != INT64_MAX) {
        int64_t current =
            counters_.intervalMinFinalityMarginPackets.load(std::memory_order_relaxed);
        while (minMarginPackets < current &&
               !counters_.intervalMinFinalityMarginPackets.compare_exchange_weak(
                   current, minMarginPackets, std::memory_order_relaxed)) {
        }
    }
    counters_.framesMissedFinality.fetch_add(missedFinality,
                                                    std::memory_order_relaxed);
    if (peak > 0.0f) {
        const uint32_t q24 = peak >= 1.0f ? 0x7FFFFFU
                                          : static_cast<uint32_t>(peak * 8388607.0f);
        uint32_t current = counters_.intervalPeakWrittenQ24.load(std::memory_order_relaxed);
        while (q24 > current &&
               !counters_.intervalPeakWrittenQ24.compare_exchange_weak(
                   current, q24, std::memory_order_relaxed)) {
        }
        if (q24 > current) {
            counters_.intervalPeakWord.store(peakWord, std::memory_order_relaxed);
        }
    }
}

const AmdtpPayloadWriterCounters& AmdtpPayloadWriter::Counters() const noexcept {
    return counters_;
}

} // namespace ASFW::Protocols::Audio::AMDTP
