#include "AmdtpPayloadWriter.hpp"

#include "PcmSlotCodec.hpp"

namespace ASFW::Protocols::Audio::AMDTP {

// Design decisions (see ../../../README.md, Step 4):
//
// 1. Reference model (one buffer, absolute sample frame): PCM lands in
//    already-exposed packets located via FindSlotForAudioFrame. The writer
//    never creates, publishes, or retires packets — slot lifecycle stays
//    with the timeline/packetizer/provider.
// 2. Per-frame count-and-skip: a partially coverable window is never
//    rejected wholesale; each frame is individually written or counted into
//    exactly one miss bucket, so
//    framesVisited == framesWritten + framesWithoutPacket + framesOutsidePacket
//    holds by construction. The counters are the diagnostic payload (the lab
//    analog of the ASFW pcmNZ/pcmZero decider).
// 3. Miss classification uses the timeline's monotonic exposure high-water
//    mark (ExposedFrameEnd): at or beyond it the packet does not exist yet
//    (framesWithoutPacket — writer ran ahead of the packetizer); below it
//    the packet existed but is no longer writable: published, retired, or
//    evicted by ring reuse (framesOutsidePacket — writer arrived late).
// 4. The host view is a window into a ring of frameCapacity frames;
//    interleavedFloat32 points at the window start, which sits at ring
//    offset firstFrame % frameCapacity, and reads wrap modulo the capacity.
//    frameCapacity == 0 degrades to a flat (non-ring) buffer.
// 5. The codec returns logical quadlet values; the writer serializes them
//    big-endian into the packet image (the LE encoding pre-swaps so that BE
//    serialization yields little-endian sample bytes — see PcmSlotCodec).
// 6. Channel policy: host channels beyond pcmChannels are dropped; missing
//    host channels encode PCM zero; non-PCM slots (MIDI etc.) are never
//    touched and keep the packetizer's defaults.
// 7. Counters accumulate locally and publish once per call with relaxed
//    atomics — no per-frame RMW traffic in the IO path.

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
    const HostAudioBufferView& hostBuffer) noexcept {
    if (timeline_ == nullptr || hostBuffer.interleavedFloat32 == nullptr ||
        hostBuffer.channels == 0 || hostBuffer.frameCount == 0) {
        return; // invalid view: nothing visited, nothing counted
    }

    const uint64_t windowOffset =
        (hostBuffer.frameCapacity != 0)
            ? hostBuffer.firstFrame % hostBuffer.frameCapacity
            : 0;

    uint64_t written = 0;
    uint64_t withoutPacket = 0;
    uint64_t outsidePacket = 0;

    for (uint32_t i = 0; i < hostBuffer.frameCount; ++i) {
        const uint64_t absoluteFrame = hostBuffer.firstFrame + i;

        PacketTimelineSlot* slot =
            timeline_->FindSlotForAudioFrame(absoluteFrame);
        if (slot == nullptr) {
            if (absoluteFrame >= timeline_->ExposedFrameEnd()) {
                ++withoutPacket;
            } else {
                ++outsidePacket;
            }
            continue;
        }

        const float* source = hostBuffer.interleavedFloat32;
        if (hostBuffer.frameCapacity != 0) {
            const uint64_t ringFrame =
                (windowOffset + i) % hostBuffer.frameCapacity;
            source += (static_cast<int64_t>(ringFrame) -
                       static_cast<int64_t>(windowOffset)) *
                      static_cast<int64_t>(hostBuffer.channels);
        } else {
            source += static_cast<uint64_t>(i) * hostBuffer.channels;
        }

        const uint32_t frameInPacket =
            static_cast<uint32_t>(absoluteFrame - slot->firstAudioFrame);
        uint8_t* dest = slot->packetBytes + kCipHeaderBytes +
                        frameInPacket * slot->dbs * kBytesPerSlot;

        const uint32_t pcmSlots = (streamConfig_.pcmChannels < slot->dbs)
                                      ? streamConfig_.pcmChannels
                                      : slot->dbs;
        for (uint32_t ch = 0; ch < pcmSlots; ++ch) {
            const float sample = (ch < hostBuffer.channels) ? source[ch] : 0.0f;
            WriteBE32(dest + ch * kBytesPerSlot,
                      PcmSlotCodec::EncodeFloat32(
                          sample, txPolicy_.hostToDevicePcmEncoding));
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
}

const AmdtpPayloadWriterCounters& AmdtpPayloadWriter::Counters() const noexcept {
    return counters_;
}

} // namespace ASFW::Protocols::Audio::AMDTP
