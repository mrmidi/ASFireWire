#include "RxAudioPacketProcessor.hpp"
#include "DirectRxPacketDecoder.hpp"
#include "../../../Wire/MOTU/MotuPayloadReader.hpp"
#include "../../../Wire/CIP/CIPHeader.hpp"
#include "../../../../Isoch/Receive/IsochRxTiming.hpp"
#include "../../../Ports/IWirePayloadCodec.hpp"

#include <algorithm>
#include <cstring>
#include <span>

namespace ASFW::AudioEngine::Direct::Rx {

static constexpr size_t kIsochHeaderSize = 8; // Timestamp (4) + 1394 Isoch Header (4)

RxAudioPacketProcessorResult RxAudioPacketProcessor::ProcessPacket(
    const uint8_t* payload,
    size_t length,
    uint64_t absoluteFrame,
    uint32_t channels,
    const ::ASFW::Audio::IRxPayloadCodec& codec,
    uint32_t channelOffset,
    bool publishTimeline,
    const RxCaptureChannelMap& captureMap,
    bool primeDelayLine) noexcept {
    RxAudioPacketProcessorResult result{};

    if (length < kIsochHeaderSize + 8) {
        result.status = DirectRxWriteStatus::kShortPacket;
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
        return result;
    }

    result.hasValidCip = true;
    result.syt = cip->syt;
    result.fdf = cip->fdf;
    result.dbc = cip->dataBlockCounter;

    const uint32_t strideQuadlets = codec.StrideQuadlets(cip->dataBlockSize);
    result.dbs = cip->dataBlockSize;
    result.strideQuadlets = strideQuadlets;

    const size_t payloadBytes = length - kIsochHeaderSize - 8;
    const size_t dbsBytes = static_cast<size_t>(strideQuadlets) * 4u;
    if (dbsBytes == 0) {
        result.status = DirectRxWriteStatus::kZeroDataBlockSize;
        return result;
    }

    const size_t eventCount = payloadBytes / dbsBytes;
    result.framesDecoded = static_cast<uint32_t>(eventCount);

    if (eventCount == 0) {
        result.status = DirectRxWriteStatus::kAvailable;
        return result;
    }

    // If unarmed: parse timing/counters, and drop PCM
    if (!writer_.IsBound()) {
        result.status = DirectRxWriteStatus::kInvalidBinding;
        return result;
    }

    if (!codec.ValidateGeometry(channels, channelOffset, strideQuadlets, cip->dataBlockSize)) {
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

    const uint32_t* dataBlocks = &quadlets[2];
    const auto* blockBytes = reinterpret_cast<const uint8_t*>(dataBlocks);
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
            std::span<const uint8_t>(blockBytes + i * dbsBytes, dbsBytes),
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

RxAudioPacketProcessorResult RxAudioPacketProcessor::ProcessPacket(const uint8_t* payload,
                                                                   size_t length,
                                                                   uint64_t absoluteFrame,
                                                                   uint32_t channels,
                                                                   uint32_t am824Slots,
                                                                   ASFW::Encoding::AudioWireFormat format,
                                                                   uint32_t channelOffset,
                                                                   bool publishTimeline,
                                                                   bool trustConfiguredStride,
                                                                   uint32_t motuPcmChunks,
                                                                   ::ASFW::Encoding::Motu::MotuPortMap motuPorts,
                                                                   const RxCaptureChannelMap& captureMap,
                                                                   bool primeDelayLine) noexcept {
    RxAudioPacketProcessorResult result{};

    if (length < kIsochHeaderSize + 8) {
        result.status = DirectRxWriteStatus::kShortPacket;
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
        return result;
    }

    result.hasValidCip = true;
    result.syt = cip->syt;
    result.fdf = cip->fdf;
    result.dbc = cip->dataBlockCounter;

    const bool isMotu = (format == ASFW::Encoding::AudioWireFormat::kMotuV2);

    // Two independent reasons the CIP header's dbs is not the stride authority here.
    //
    // MOTU: the UltraLite and 8pre stamp a wrong DBS; Linux sets CIP_WRONG_DBS for
    // exactly those two models (amdtp-motu.c:458-463) and divides the payload by the
    // configured data_block_quadlets instead (amdtp-stream.c:1475-1476). Trusting the
    // header split each 8-block packet into 5 at the wrong stride -- capture ran at 5/8
    // of the real rate, samples and SPH timestamps were read from misaligned offsets,
    // and transmit replayed 5-block packets to a device expecting 8, audible as
    // crackling. For well-behaved models the configured value equals the header's, so it
    // is used for every MOTU stream rather than keying on the model.
    //
    // Loud OXFW: the same class of defect on the capture side, expressed as the
    // configured AM824 slot count (snd-oxfw SND_OXFW_QUIRK_WRONG_DBS; amdtp-stream.c:766-769
    // substitutes data_block_quadlets).
    const uint32_t strideQuadlets =
        (isMotu && motuPcmChunks != 0)
            ? ASFW::Encoding::Motu::DataBlockQuadlets(motuPcmChunks)
            : (trustConfiguredStride ? am824Slots : cip->dataBlockSize);

    // Report both, unconditionally: what the wire claimed and what we trusted. Consumers
    // pick the one they mean, so no consumer has to know which family this is.
    result.dbs = cip->dataBlockSize;
    result.strideQuadlets = strideQuadlets;

    const size_t payloadBytes = length - kIsochHeaderSize - 8;
    const size_t dbsBytes = static_cast<size_t>(strideQuadlets) * 4u;
    if (dbsBytes == 0) {
        result.status = DirectRxWriteStatus::kZeroDataBlockSize;
        return result;
    }

    const size_t eventCount = payloadBytes / dbsBytes;
    result.framesDecoded = static_cast<uint32_t>(eventCount);

    if (eventCount == 0) {
        result.status = DirectRxWriteStatus::kAvailable;
        return result;
    }

    // If unarmed: parse timing/counters, and drop PCM
    if (!writer_.IsBound()) {
        result.status = DirectRxWriteStatus::kInvalidBinding;
        return result;
    }

    // Geometry validation.
    //
    // The quadlet-slot families put one sample in one slot, so the stride must cover the
    // channel count and match the negotiated slot count -- unless the wrong-DBS quirk is
    // active, in which case the header dbs is not part of the contract. MOTU packs 3-byte
    // chunks past an SPH quadlet, so its dbs is SMALLER than the channel count for any
    // stream wider than about four channels -- a 14-chunk stream has dbs 13. Applying the
    // slot rule to it rejects every packet, so it is checked against its own geometry.
    if (channels == 0) {
        result.status = DirectRxWriteStatus::kGeometryMismatch;
        return result;
    }
    if (isMotu) {
        const size_t requiredBytes = static_cast<size_t>(ASFW::Encoding::Motu::kPcmByteOffset) +
                                     static_cast<size_t>(motuPcmChunks) *
                                         ASFW::Encoding::Motu::kBytesPerChunk;
        if (motuPcmChunks == 0 || channelOffset + channels > motuPcmChunks ||
            requiredBytes > dbsBytes) {
            result.status = DirectRxWriteStatus::kGeometryMismatch;
            return result;
        }
    } else if (strideQuadlets < channels ||
               (!trustConfiguredStride && am824Slots != cip->dataBlockSize)) {
        result.status = DirectRxWriteStatus::kGeometryMismatch;
        return result;
    }

    // A map sized for a different formation than the packet actually carries
    // would read past the data blocks. Fall back to the wire order instead:
    // presenting the device's own channel order is recoverable, reading out of
    // bounds is not.
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

    // If armed: decode directly to ADK input memory.
    const uint32_t* dataBlocks = &quadlets[2];
    const auto* blockBytes = reinterpret_cast<const uint8_t*>(dataBlocks);
    for (size_t i = 0; i < eventCount; ++i) {
        float* frameOut = writer_.Frame(absoluteFrame + i);
        if (!frameOut) {
            result.status = DirectRxWriteStatus::kInvalidRange;
            return result;
        }

        // Write this stream's slice at its channel offset into the interleaved
        // frame; the writer stride covers the buffer's full channel width.
        if (isMotu) {
            // MOTU chunks are byte-addressed, so the block is handed over as a span
            // rather than a quadlet pointer. channelOffset selects this stream's chunks
            // within the block, matching the AMDTP path's multi-stream split.
            ASFW::Encoding::Motu::DecodeMotuBlock(
                std::span<const uint8_t>(blockBytes + i * dbsBytes, dbsBytes),
                motuPcmChunks, channelOffset, frameOut + channelOffset, channels, motuPorts);
        } else {
            const uint32_t* frameIn = dataBlocks + (i * strideQuadlets);
            if (effectiveMap.IsIdentity()) {
                DecodeDirectRxFrame(frameIn, channels, strideQuadlets, format,
                                    frameOut + channelOffset);
            } else {
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
        }
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
