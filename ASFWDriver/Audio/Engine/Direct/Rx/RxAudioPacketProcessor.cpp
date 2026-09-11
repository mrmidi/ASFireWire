#include "RxAudioPacketProcessor.hpp"
#include "DirectRxPacketDecoder.hpp"
#include "../../../Wire/MOTU/MotuPayloadReader.hpp"
#include "../../../Wire/CIP/CIPHeader.hpp"
#include "../../../../Isoch/Receive/IsochRxTiming.hpp"

#include <algorithm>
#include <cstring>
#include <span>

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
                                                                   uint32_t motuPcmChunks,
                                                                   ::ASFW::Encoding::Motu::MotuPortMap motuPorts) noexcept {
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

    // The UltraLite and 8pre put a wrong DBS in their CIP header; Linux sets
    // CIP_WRONG_DBS for exactly those two models (amdtp-motu.c:458-463) and divides the
    // payload by the configured data_block_quadlets instead (amdtp-stream.c:1475-1476).
    // Trusting the header here split each 8-block packet into 5 at the wrong stride:
    // capture ran at 5/8 of the real rate (127680 frames consumed over 4.08 s, ~31.3 kHz
    // against 48 kHz), samples and SPH timestamps were read from misaligned offsets, and
    // transmit replayed 5-block packets to a device expecting 8 -- audible as crackling.
    // For well-behaved models the configured value equals the header's, so use it for
    // every MOTU stream rather than keying on the model.
    const uint32_t dataBlockQuadlets =
        (isMotu && motuPcmChunks != 0)
            ? ASFW::Encoding::Motu::DataBlockQuadlets(motuPcmChunks)
            : cip->dataBlockSize;
    result.dbs = dataBlockQuadlets;

    const size_t payloadBytes = length - kIsochHeaderSize - 8;
    const size_t dbsBytes = static_cast<size_t>(dataBlockQuadlets) * 4u;
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
    // The quadlet-slot families put one sample in one slot, so dbs must cover the
    // channel count and match the negotiated slot count. MOTU packs 3-byte chunks past
    // an SPH quadlet, so dbs is SMALLER than the channel count for any stream wider than
    // about four channels -- a 14-chunk stream has dbs 13. Applying the slot rule to it
    // rejects every packet, so MOTU is checked against its own block geometry instead.
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
    } else if (cip->dataBlockSize < channels || am824Slots != cip->dataBlockSize) {
        result.status = DirectRxWriteStatus::kGeometryMismatch;
        return result;
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
            const uint32_t* frameIn = dataBlocks + (i * cip->dataBlockSize);
            DecodeDirectRxFrame(frameIn, channels, cip->dataBlockSize, format,
                                frameOut + channelOffset);
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
