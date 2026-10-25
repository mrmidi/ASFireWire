#include "RxAudioPacketProcessor.hpp"
#include "DirectRxPacketDecoder.hpp"
#include "../../../Wire/CIP/CIPHeader.hpp"
#include "../../../../Isoch/Receive/IsochRxTiming.hpp"
#include "../../../Ports/IWirePayloadCodec.hpp"

#include <algorithm>
#include <cstring>
#include <span>

namespace ASFW::AudioEngine::Direct::Rx {

static constexpr size_t kIsochHeaderSize = 8; // Timestamp (4) + 1394 Isoch Header (4)

namespace {

// IEC 61883-6: AM824 is FMT 0x10, and a packet whose FDF is 0xFF is NO-DATA
// (data-block count zero) even when payload bytes follow the header. Linux
// treats it the same way (amdtp-stream.c:768-770: payload_length == 0 or
// fmt == AM and fdf == NO_DATA -> data_blocks = 0). Reading MIDI out of what
// trails a NO-DATA header would be reading leftover buffer.
constexpr uint8_t kFormatAm824 = 0x10;
constexpr uint8_t kAm824NoDataFdf = 0xFF;

void MarkMidiGap(const RxMidiExtraction& midi, RxAudioPacketProcessorResult& result) noexcept {
    if (!midi.Enabled()) return;
    for (uint8_t port = 0; port < midi.PortCount(); ++port) {
        midi.sink->MarkMidiDiscontinuity(port);
    }
    result.midiGap = true;
}

} // namespace

RxAudioPacketProcessorResult RxAudioPacketProcessor::ProcessPacket(
    const uint8_t* payload,
    size_t length,
    uint64_t absoluteFrame,
    uint32_t channels,
    const ::ASFW::Audio::IRxPayloadCodec& codec,
    uint32_t channelOffset,
    bool publishTimeline,
    const RxCaptureChannelMap& captureMap,
    bool primeDelayLine,
    const ::ASFW::Encoding::AudioPacketFraming framing,
    const RxMidiExtraction& midi) noexcept {
    RxAudioPacketProcessorResult result{};

    const bool headerless = framing == ::ASFW::Encoding::AudioPacketFraming::kHeaderless;
    if (length < kIsochHeaderSize + (headerless ? 0U : 8U)) {
        result.status = DirectRxWriteStatus::kShortPacket;
        MarkMidiGap(midi, result);
        return result;
    }

    result.hasReceiveCycleTimestamp =
        ASFW::Isoch::Rx::DecodeReceiveTimestamp(
            payload, length, result.receiveCycleTimestamp);

    const uint8_t* data = payload + kIsochHeaderSize;
    size_t payloadBytes = length - kIsochHeaderSize;
    uint8_t cipDBS = 0;
    uint8_t cipFormat = 0;
    if (!headerless) {
        uint32_t quadlets[2]{};
        std::memcpy(quadlets, data, sizeof(quadlets));
        const auto cip = codec.DecodeHeader(quadlets[0], quadlets[1]);
        if (!cip) {
            result.status = DirectRxWriteStatus::kInvalidCipHeader;
            MarkMidiGap(midi, result);
            return result;
        }
        cipFormat = cip->format;
        result.hasValidCip = true;
        result.syt = cip->syt;
        result.fdf = cip->fdf;
        result.dbc = cip->dataBlockCounter;
        result.dbs = cip->dataBlockSize;
        cipDBS = cip->dataBlockSize;
        data += 8;
        payloadBytes -= 8;
    }

    // A CIP header without payload carries no audio events regardless of its
    // DBS value. The M-Audio 1814 idles with header-only packets that say
    // DBS=2 (captured `02020000 9002ffff`) while its stream is DBS=11. Linux
    // AMDTP handles zero payload before checking DBS (amdtp-stream.c:769-780).
    if (payloadBytes == 0) {
        result.status = DirectRxWriteStatus::kAvailable;
        return result;
    }

    const uint32_t strideQuadlets = codec.StrideQuadlets(cipDBS);
    result.strideQuadlets = strideQuadlets;

    const size_t strideBytes = static_cast<size_t>(strideQuadlets) * 4u;
    if (strideBytes == 0) {
        result.status = DirectRxWriteStatus::kZeroDataBlockSize;
        MarkMidiGap(midi, result);
        return result;
    }

    // Headerless streams have no DBS field to bound each sample frame, so their
    // payload must consist of complete codec-defined frames.
    if (headerless && payloadBytes % strideBytes != 0) {
        result.status = DirectRxWriteStatus::kGeometryMismatch;
        return result;
    }

    const size_t eventCount = payloadBytes / strideBytes;
    result.framesDecoded = static_cast<uint32_t>(eventCount);

    if (eventCount == 0) {
        result.status = DirectRxWriteStatus::kAvailable;
        return result;
    }

    // MIDI comes off the wire BEFORE the PCM binding check below: the writer is
    // legitimately unbound whenever no CoreAudio client holds the capture buffer,
    // and extracting after that early return would make MIDI work only while an
    // audio client happened to be open. The event count is the wire payload's,
    // never the number of PCM frames that were written.
    //
    // Everything is validated before the first byte is read: a CIP-framed
    // AM824 packet that is not NO-DATA, whose data-block size is the one the
    // MIDI slot was projected for, and whose payload is whole data blocks. A
    // packet that fails any of these other than for being NO-DATA lost its
    // bytes, so the sink is told the stream is discontinuous.
    // A MOTU packet is never AM824 and has no NO-DATA form beyond an empty
    // payload, handled above. Its blocks are the codec's stride, which the
    // CIP DBS does not describe for every model (MotuRxPayloadCodec).
    if (midi.Enabled() && !headerless && midi.motuSlot) {
        if (cipFormat != kFormatAm824 && (payloadBytes % strideBytes) == 0) {
            ASFW::Encoding::Motu::MotuMidiDemuxCounters discard{};
            auto& counters = midi.motuCounters != nullptr ? *midi.motuCounters : discard;
            const uint64_t before = counters.bytesDelivered;
            const uint64_t packetHostTicks =
                result.hasReceiveCycleTimestamp
                    ? ASFW::Isoch::Rx::PacketHostTicks(result.receiveCycleTimestamp,
                                                       midi.drainCycleTimer, midi.drainHostTicks)
                    : midi.drainHostTicks;
            ASFW::Encoding::Motu::DemuxMotuMidi(data, static_cast<uint32_t>(eventCount), strideBytes,
                                                *midi.motuSlot, packetHostTicks, *midi.sink, counters);
            result.midiBytesDelivered = static_cast<uint32_t>(counters.bytesDelivered - before);
        } else {
            MarkMidiGap(midi, result);
        }
    } else if (midi.Enabled() && !headerless) {
        const bool noData = cipFormat == kFormatAm824 && result.fdf == kAm824NoDataFdf;
        if (!noData) {
            const bool whole = (payloadBytes % strideBytes) == 0;
            if (cipFormat == kFormatAm824 && whole &&
                strideQuadlets == midi.geometry.dbs &&
                cipDBS == midi.geometry.dbs) {
                ASFW::Encoding::MpxMidiDemuxCounters discard{};
                ASFW::Encoding::MpxMidiDemuxCounters& counters =
                    midi.counters != nullptr ? *midi.counters : discard;
                const uint64_t before = counters.bytesDelivered;
                const uint64_t packetHostTicks =
                    result.hasReceiveCycleTimestamp
                        ? ASFW::Isoch::Rx::PacketHostTicks(
                              result.receiveCycleTimestamp, midi.drainCycleTimer,
                              midi.drainHostTicks)
                        : midi.drainHostTicks;
                ASFW::Encoding::DemuxMpxMidi(data, static_cast<uint32_t>(eventCount),
                                             result.dbc, midi.geometry, packetHostTicks,
                                             *midi.sink, counters);
                result.midiBytesDelivered =
                    static_cast<uint32_t>(counters.bytesDelivered - before);
            } else {
                MarkMidiGap(midi, result);
            }
        }
    }

    // If unarmed: parse timing/counters, and drop PCM
    if (!writer_.IsBound()) {
        result.status = DirectRxWriteStatus::kInvalidBinding;
        return result;
    }

    if (!codec.ValidateGeometry(channels, channelOffset, strideQuadlets, cipDBS)) {
        result.status = DirectRxWriteStatus::kGeometryMismatch;
        return result;
    }

    const bool mapUsable = captureMap.FitsWithin(channels, strideQuadlets);
    const RxCaptureChannelMap& effectiveMap =
        mapUsable ? captureMap : RxCaptureChannelMap{};
    if (!mapUsable) {
        result.mapRejected = true;
    }
    const uint32_t delayFrames = effectiveMap.HasDelay() ? effectiveMap.delayFrames : 0;

    if (primeDelayLine && delayFrames != 0) {
        for (uint32_t i = 0; i < delayFrames; ++i) {
            float* frameOut = writer_.Frame(absoluteFrame + i);
            if (frameOut) {
                SilenceDelayedChannels(channels, effectiveMap, frameOut + channelOffset);
            }
        }
    }

    for (size_t i = 0; i < eventCount; ++i) {
        float* frameOut = writer_.Frame(absoluteFrame + i);
        if (!frameOut) {
            result.status = DirectRxWriteStatus::kInvalidRange;
            return result;
        }

        float* delayedOut = nullptr;
        if (delayFrames != 0) {
            delayedOut = writer_.Frame(absoluteFrame + i + delayFrames);
            if (delayedOut) {
                delayedOut += channelOffset;
            }
        }

        codec.DecodeBlock(
            std::span<const uint8_t>(data + i * strideBytes, strideBytes),
            channels, channelOffset, effectiveMap,
            frameOut + channelOffset, delayedOut);
    }

    if (publishTimeline) {
        const uint64_t producedEnd = absoluteFrame + eventCount;
        writer_.PublishProducedEnd(producedEnd, static_cast<uint32_t>(eventCount));
    }

    result.status = DirectRxWriteStatus::kAvailable;
    return result;
}

} // namespace ASFW::AudioEngine::Direct::Rx
