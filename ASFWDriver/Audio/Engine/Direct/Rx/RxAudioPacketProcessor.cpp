// Modified in 2026 by Rafal Zalech to add original MOTU UltraLite support.
#include "RxAudioPacketProcessor.hpp"
#include "DirectRxPacketDecoder.hpp"
#include "../../../Wire/CIP/CIPHeader.hpp"
#include "../../../Wire/MOTU/MotuBlockCodec.hpp"
#include "../../../Wire/MOTU/MotuBlockLayout.hpp"
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
                                                                   const std::array<uint8_t, 32>*
                                                                       wireChannelForHostChannel) noexcept {
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
    result.dbs = cip->dataBlockSize;
    result.dbc = cip->dataBlockCounter;

    const size_t payloadBytes = length - kIsochHeaderSize - 8;
    const bool isMotu =
        format == ASFW::Encoding::AudioWireFormat::kMotuV2;
    // Original UltraLite capture packets report an unreliable CIP DBS.  The
    // configured MOTU block geometry is authoritative for this wire format.
    const size_t dbsBytes = isMotu
        ? static_cast<size_t>(am824Slots) * 4u
        : static_cast<size_t>(cip->dataBlockSize) * 4u;
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

    // Geometry validation
    if (channels == 0 ||
        (!isMotu &&
         (cip->dataBlockSize < channels ||
          am824Slots != cip->dataBlockSize)) ||
        (isMotu &&
         (am824Slots != ASFW::Encoding::Motu::DataBlockQuadlets(channels) ||
          channels != 14))) {
        result.status = DirectRxWriteStatus::kGeometryMismatch;
        return result;
    }
    if (wireChannelForHostChannel) {
        for (uint32_t hostChannel = 0; hostChannel < channels; ++hostChannel) {
            if ((*wireChannelForHostChannel)[hostChannel] >= channels) {
                result.status = DirectRxWriteStatus::kGeometryMismatch;
                return result;
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

        if (isMotu) {
            const auto* blockBytes =
                reinterpret_cast<const uint8_t*>(dataBlocks) + i * dbsBytes;
            const std::span<const uint8_t> block(blockBytes, dbsBytes);
            if (i < result.motuSph.size()) {
                result.motuSph[i] = ASFW::Encoding::Motu::ReadSph(block);
                ++result.motuSphCount;
            }
            for (uint32_t hostChannel = 0; hostChannel < channels; ++hostChannel) {
                const uint32_t wireChannel = wireChannelForHostChannel
                    ? (*wireChannelForHostChannel)[hostChannel]
                    : hostChannel;
                const int32_t sample =
                    ASFW::Encoding::Motu::ReadPcmChannel(block, wireChannel) >> 8;
                frameOut[channelOffset + hostChannel] =
                    Detail::Signed24ToFloat32(sample);
            }
        } else {
            const uint32_t* frameIn = dataBlocks + (i * cip->dataBlockSize);
            // Write this stream's slice at its channel offset into the interleaved
            // frame; the writer stride covers the buffer's full channel width.
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
