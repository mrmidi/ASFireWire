#include "AmdtpPayloadWriter.hpp"

#include "PcmSlotCodec.hpp"

#include <atomic>
#include <cstring>
namespace ASFW::Protocols::Audio::AMDTP {

// Design decisions (see ../../../README.md, Step 4):
//
// 1. Reference model (one buffer, absolute sample frame): PCM lands in
//    already-exposed packets located via SnapshotSlotForAudioFrame. The
//    writer never creates, publishes, or retires packets — slot lifecycle
//    stays with the timeline/packetizer/provider.
// 2. Per-frame count-and-skip: a partially coverable window is never
//    rejected wholesale; each frame is individually written or counted into
//    exactly one miss bucket, so
//    framesVisited == framesWritten + framesWithoutPacket + framesOutsidePacket
//    holds by construction. The counters are the diagnostic payload (the lab
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
//    atomics — no per-frame RMW traffic in the IO path.
// 8. RT-vs-pump discipline: this runs on the ADK real-time IO thread while
//    the pump rewrites slots on the work queue. All slot fields come from a
//    PacketSlotSnapshot validated under the generation seqlock; the live
//    slot pointer is touched only to recheck the generation after the
//    payload bytes are written. A failed recheck means the slot was reused
//    mid-write — the bytes landed in a valid (but newer) packet image, so
//    the frame is counted in framesRacedReuse (an overlap diagnostic on top
//    of framesWritten, not a fourth miss bucket).

namespace {

constexpr uint32_t kCipHeaderBytes = 8;
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
    uint64_t completionCursor) noexcept {
    if (timeline_ == nullptr || hostBuffer.interleavedFloat32 == nullptr ||
        hostBuffer.channels == 0 || hostBuffer.frameCount == 0) {
        return; // invalid view: nothing visited, nothing counted
    }

    const uint32_t retiredCursor =
        static_cast<uint32_t>(completionCursor);
    const uint64_t writeEndFrame =
        hostBuffer.firstFrame + hostBuffer.frameCount;
    const uint64_t exposedFrameEnd = timeline_->ExposedFrameEnd();
    if (writeEndFrame > exposedFrameEnd) {
        counters_.underExposureCalls.fetch_add(1, std::memory_order_relaxed);
        counters_.underExposureFrames.fetch_add(
            writeEndFrame - exposedFrameEnd, std::memory_order_relaxed);
    }

    uint64_t written = 0;
    uint64_t withoutPacket = 0;
    uint64_t outsidePacket = 0;
    uint64_t racedReuse = 0;
    uint64_t wroteIntoTransmitted = 0;
    uint64_t nonZeroFrames = 0;
    uint64_t nonZeroSlots = 0;
    float localMaxAbs = 0.0f;

    for (uint32_t i = 0; i < hostBuffer.frameCount; ++i) {
        const uint64_t absoluteFrame = hostBuffer.firstFrame + i;

        PacketSlotSnapshot snap{};
        if (!timeline_->SnapshotSlotForAudioFrame(absoluteFrame, snap)) {
            if (absoluteFrame >= timeline_->ExposedFrameEnd()) {
                ++withoutPacket;
            } else {
                ++outsidePacket;
            }
            continue;
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
        const uint32_t headerBytes = streamConfig_.includeCipHeader ? kCipHeaderBytes : 0U;
        uint8_t* dest = snap.packetBytes + headerBytes +
                        frameInPacket * snap.dbs * kBytesPerSlot;

        const uint32_t pcmSlots = (streamConfig_.pcmChannels < snap.dbs)
                                      ? streamConfig_.pcmChannels
                                      : snap.dbs;
        // This stream encodes the host channels [sourceChannelOffset,
        // sourceChannelOffset + pcmChannels) of the shared interleaved buffer —
        // the de-interleave that mirrors the RX side's channelOffset. Host
        // channels outside the buffer encode PCM zero.
        const uint32_t srcOffset = streamConfig_.sourceChannelOffset;
        bool frameNonZero = false;
        for (uint32_t ch = 0; ch < pcmSlots; ++ch) {
            const uint32_t srcCh = srcOffset + ch;
            const float sample =
                (srcCh < hostBuffer.channels) ? source[srcCh] : 0.0f;
            WriteBE32(dest + ch * kBytesPerSlot,
                      PcmSlotCodec::EncodeFloat32(
                          sample, txPolicy_.hostToDevicePcmEncoding));
            if (sample != 0.0f) {
                frameNonZero = true;
                ++nonZeroSlots;
                const float absSample = (sample >= 0.0f) ? sample : -sample;
                if (absSample > localMaxAbs) {
                    localMaxAbs = absSample;
                }
            }
        }
        if (frameNonZero) {
            ++nonZeroFrames;
        }
        ++written;

        if (snap.packetIndex < retiredCursor) {
            ++wroteIntoTransmitted;
        }

        std::atomic_thread_fence(std::memory_order_acquire);
        if (snap.slot->generation.load(std::memory_order_relaxed) !=
            snap.generation) {
            ++racedReuse; // slot reused mid-write: bytes hit the newer image
        }
    }

    counters_.framesVisited.fetch_add(hostBuffer.frameCount,
                                      std::memory_order_relaxed);
    counters_.framesWritten.fetch_add(written, std::memory_order_relaxed);
    counters_.framesWithoutPacket.fetch_add(withoutPacket,
                                            std::memory_order_relaxed);
    counters_.framesOutsidePacket.fetch_add(outsidePacket,
                                            std::memory_order_relaxed);
    counters_.framesRacedReuse.fetch_add(racedReuse,
                                         std::memory_order_relaxed);
    counters_.framesWroteIntoTransmitted.fetch_add(wroteIntoTransmitted,
                                                    std::memory_order_relaxed);
    counters_.framesNonZero.fetch_add(nonZeroFrames,
                                      std::memory_order_relaxed);
    counters_.slotsNonZero.fetch_add(nonZeroSlots,
                                     std::memory_order_relaxed);
    // IEEE 754 float32: for non-negative values, the uint32 bit pattern is
    // monotonically increasing, so a simple max on the bit representation
    // is correct. Only one RT writer thread exists, so the CAS never contends.
    if (localMaxAbs > 0.0f) {
        uint32_t desired;
        std::memcpy(&desired, &localMaxAbs, sizeof(desired));
        uint32_t expected =
            counters_.maxAbsSampleBits.load(std::memory_order_relaxed);
        while (desired > expected) {
            if (counters_.maxAbsSampleBits.compare_exchange_weak(
                    expected, desired, std::memory_order_relaxed)) {
                break;
            }
        }
    }
}

const AmdtpPayloadWriterCounters& AmdtpPayloadWriter::Counters() const noexcept {
    return counters_;
}

} // namespace ASFW::Protocols::Audio::AMDTP
