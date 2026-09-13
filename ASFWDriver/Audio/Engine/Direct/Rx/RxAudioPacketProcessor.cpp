#include "RxAudioPacketProcessor.hpp"
#include "DirectRxPacketDecoder.hpp"
#include "../../../Wire/CIP/CIPHeader.hpp"
#include "../../../../Isoch/Receive/IsochRxTiming.hpp"

#include <algorithm>
#include <cstring>

namespace ASFW::AudioEngine::Direct::Rx {

static constexpr size_t kIsochHeaderSize = 8; // Timestamp (4) + 1394 Isoch Header (4)

RxAudioPacketProcessorResult RxAudioPacketProcessor::ProcessPacket(const uint8_t* payload,
                                                                   size_t length,
                                                                   uint64_t absoluteFrame,
                                                                   uint32_t channels,
                                                                   uint32_t am824Slots,
                                                                   ASFW::Encoding::AudioWireFormat format,
                                                                   uint32_t channelOffset,
                                                                   bool publishTimeline,
                                                                   const RxCaptureChannelMap& captureMap,
                                                                   bool primeDelayLine,
                                                                   const RxMidiExtraction& midi) noexcept {
    RxAudioPacketProcessorResult result{};

    if (length < kIsochHeaderSize + 8) {
        result.status = DirectRxWriteStatus::kShortPacket;
        if (midi.Enabled() && midi.sink) {
            for (uint8_t p = 0; p < midi.geometry.portCount; ++p) {
                midi.sink->MarkMidiDiscontinuity(p);
            }
        }
        return result;
    }

    result.hasReceiveCycleTimestamp =
        ASFW::Isoch::Rx::DecodeReceiveTimestamp(
            payload, length, result.receiveCycleTimestamp);

    const uint8_t* cipStart = payload + kIsochHeaderSize;
    const auto* quadlets = reinterpret_cast<const uint32_t*>(cipStart);
    
    // Decode CIP Header (quadlets[0] and quadlets[1])
    const auto cip = ASFW::Isoch::CIPHeader::Decode(quadlets[0], quadlets[1]);
    if (!cip) {
        result.status = DirectRxWriteStatus::kInvalidCipHeader;
        if (midi.Enabled() && midi.sink) {
            for (uint8_t p = 0; p < midi.geometry.portCount; ++p) {
                midi.sink->MarkMidiDiscontinuity(p);
            }
        }
        return result;
    }

    result.hasValidCip = true;
    result.syt = cip->syt;
    result.fdf = cip->fdf;
    result.dbs = cip->dataBlockSize;
    result.dbc = cip->dataBlockCounter;

    const size_t payloadBytes = length - kIsochHeaderSize - 8;
    const size_t dbsBytes = static_cast<size_t>(cip->dataBlockSize) * 4u;
    if (dbsBytes == 0) {
        result.status = DirectRxWriteStatus::kZeroDataBlockSize;
        return result;
    }

    // IEC 61883-6 §5.2 / §5.3: An AM824 packet with FDF=0xFF is NO-DATA (data-block count = 0),
    // even if trailing payload bytes are present.
    // Cross-validated with Linux amdtp-stream.c:768-770:
    // payload_length == 0 || (fmt == CIP_FMT_AM && fdf == AMDTP_FDF_NO_DATA) => data_blocks = 0
    const bool isNoData = cip->IsNoData() || payloadBytes == 0;
    const size_t eventCount = isNoData ? 0 : (payloadBytes / dbsBytes);
    result.ragged = !isNoData && ((payloadBytes % dbsBytes) != 0);
    result.framesDecoded = static_cast<uint32_t>(eventCount);

    if (eventCount == 0) {
        // NO-DATA (header-only or payload-bearing with FDF=0xFF) carries no data blocks at all.
        // Deriving MIDI from whatever follows the header would be reading leftover buffer.
        result.status = DirectRxWriteStatus::kAvailable;
        return result;
    }

    // MIDI comes off the wire BEFORE the PCM binding check below. In MIDI-only
    // operation no CoreAudio client is open, so the writer is legitimately
    // unbound and that path returns early -- extracting after it would mean
    // MIDI worked only while audio happened to be running.
    //
    // Validate CIP format is AM824 (0x10) and not NO-DATA before demuxing: a packet
    // with another format or NO-DATA disposition could otherwise deliver coincidental
    // 0x81 labels as spurious MIDI.
    if (midi.Enabled() && !result.ragged &&
        cip->format == ASFW::Isoch::CIPHeader::kFormatAM824 &&
        !cip->IsNoData() &&
        midi.geometry.dbs == cip->dataBlockSize &&
        midi.geometry.midiSlotIndex < cip->dataBlockSize) {
        ASFW::Encoding::MpxMidiDemuxCounters discard{};
        ASFW::Encoding::MpxMidiDemuxCounters& counters =
            midi.counters ? *midi.counters : discard;
        const uint64_t before = counters.bytesDelivered;
        ASFW::Encoding::DemuxMpxMidi(
            reinterpret_cast<const uint8_t*>(&quadlets[2]),
            static_cast<uint32_t>(eventCount), cip->dataBlockCounter,
            midi.geometry, *midi.sink, counters);
        result.midiBytesDelivered =
            static_cast<uint32_t>(counters.bytesDelivered - before);
    }

    // If unarmed: parse timing/counters, and drop PCM
    if (!writer_.IsBound()) {
        result.status = DirectRxWriteStatus::kInvalidBinding;
        return result;
    }

    // Geometry validation
    if (channels == 0 ||
        cip->dataBlockSize < channels ||
        am824Slots != cip->dataBlockSize) {
        result.status = DirectRxWriteStatus::kGeometryMismatch;
        return result;
    }

    // A map sized for a different formation than the packet actually carries
    // would read past the data blocks. Fall back to the wire order instead:
    // presenting the device's own channel order is recoverable, reading out of
    // bounds is not.
    const bool mapUsable = captureMap.FitsWithin(channels, cip->dataBlockSize);
    const RxCaptureChannelMap& effectiveMap =
        mapUsable ? captureMap : RxCaptureChannelMap{};
    if (!mapUsable) {
        result.mapRejected = true;
    }
    const uint32_t delayFrames = effectiveMap.HasDelay() ? effectiveMap.delayFrames : 0;

    // The head of a delay line has no predecessor to source its channels from.
    // Silence it once per epoch so a restart cannot replay the previous run's
    // audio on those channels for the length of the delay.
    if (primeDelayLine && delayFrames != 0) {
        for (uint32_t i = 0; i < delayFrames; ++i) {
            float* frameOut = writer_.Frame(absoluteFrame + i);
            if (frameOut) {
                SilenceDelayedChannels(channels, effectiveMap, frameOut + channelOffset);
            }
        }
    }

    // If armed: decode quadlets directly to ADK input memory
    const uint32_t* dataBlocks = &quadlets[2];
    for (size_t i = 0; i < eventCount; ++i) {
        float* frameOut = writer_.Frame(absoluteFrame + i);
        if (!frameOut) {
            result.status = DirectRxWriteStatus::kInvalidRange;
            return result;
        }

        const uint32_t* frameIn = dataBlocks + (i * cip->dataBlockSize);
        // Write this stream's slice at its channel offset into the interleaved
        // frame; the writer stride covers the buffer's full channel width.
        if (effectiveMap.IsIdentity()) {
            DecodeDirectRxFrame(frameIn, channels, cip->dataBlockSize, format,
                                frameOut + channelOffset);
            continue;
        }

        // Delayed channels go to a later frame. The producer cursor still ends
        // at `absoluteFrame + eventCount`, so those channels trail the frontier
        // by `delayFrames` until the following packets fill them in — which is
        // invisible as long as the delay stays well inside the HAL's input
        // cursor offset, and 16 frames against 128 has ample room.
        float* delayedOut = nullptr;
        if (delayFrames != 0) {
            delayedOut = writer_.Frame(absoluteFrame + i + delayFrames);
            if (delayedOut) {
                delayedOut += channelOffset;
            }
        }
        DecodeDirectRxFrameMapped(frameIn, channels, format, effectiveMap,
                                  frameOut + channelOffset, delayedOut);
    }

    // Only the master stream advances the producer cursor/frame counters; a
    // secondary slice writes PCM into the same frames without re-publishing the
    // timeline (the two streams are frame-locked by the device clock).
    if (publishTimeline) {
        const uint64_t producedEnd = absoluteFrame + eventCount;
        writer_.PublishProducedEnd(producedEnd, static_cast<uint32_t>(eventCount));
    }

    result.status = DirectRxWriteStatus::kAvailable;
    return result;
}

} // namespace ASFW::AudioEngine::Direct::Rx
