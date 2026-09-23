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

} // namespace ASFW::AudioEngine::Direct::Rx
