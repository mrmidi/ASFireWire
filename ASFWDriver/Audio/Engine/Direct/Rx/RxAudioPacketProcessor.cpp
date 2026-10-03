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

// The header every 828 Mk3 capture packet carries in the CIP position. Matched
// byte for byte, as observed on hardware: it is not an
// IEC 61883 CIP header (EOH1 is clear), so no field of it is interpreted.
static constexpr uint8_t kMotuV3CaptureHeader[8] = {
    0x0d, 0x04, 0x04, 0x00, 0x22, 0xff, 0xff, 0xff,
};

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
    const ::ASFW::Encoding::AudioPacketFraming framing) noexcept {
    RxAudioPacketProcessorResult result{};

    const bool headerless = framing == ::ASFW::Encoding::AudioPacketFraming::kHeaderless;
    const bool motuV3Header =
        framing == ::ASFW::Encoding::AudioPacketFraming::kMotuV3Header;
    if (length < kIsochHeaderSize + (headerless ? 0U : 8U)) {
        result.status = DirectRxWriteStatus::kShortPacket;
        return result;
    }

    result.hasReceiveCycleTimestamp =
        ASFW::Isoch::Rx::DecodeReceiveTimestamp(
            payload, length, result.receiveCycleTimestamp);

    const uint8_t* data = payload + kIsochHeaderSize;
    size_t payloadBytes = length - kIsochHeaderSize;
    uint8_t cipDBS = 0;
    if (motuV3Header) {
        if (std::memcmp(data, kMotuV3CaptureHeader, sizeof(kMotuV3CaptureHeader)) != 0) {
            result.status = DirectRxWriteStatus::kInvalidCipHeader;
            return result;
        }
        // Reported as a valid header with SYT NO_INFO, as the V3 decoder always
        // did: MOTU carries presentation time in each block's SPH instead.
        result.hasValidCip = true;
        result.syt = 0xFFFF;
        result.fdf = data[4];
        result.dbs = data[1];
        data += sizeof(kMotuV3CaptureHeader);
        payloadBytes -= sizeof(kMotuV3CaptureHeader);
    } else if (!headerless) {
        const auto* quadlets = reinterpret_cast<const uint32_t*>(data);
        const auto cip = ASFW::Isoch::CIPHeader::Decode(quadlets[0], quadlets[1]);
        if (!cip) {
            result.status = DirectRxWriteStatus::kInvalidCipHeader;
            return result;
        }
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
        return result;
    }

    // Headerless streams have no DBS field to bound each sample frame, so their
    // payload must consist of complete codec-defined frames.
    // The V3 header has no trustworthy DBS either, so the same rule applies.
    if ((headerless || motuV3Header) && payloadBytes % strideBytes != 0) {
        result.status = DirectRxWriteStatus::kGeometryMismatch;
        return result;
    }

    const size_t eventCount = payloadBytes / strideBytes;
    result.framesDecoded = static_cast<uint32_t>(eventCount);
    if (motuV3Header && eventCount != 0) {
        result.firstMotuSph = (static_cast<uint32_t>(data[0]) << 24) |
                              (static_cast<uint32_t>(data[1]) << 16) |
                              (static_cast<uint32_t>(data[2]) << 8) |
                              static_cast<uint32_t>(data[3]);
        result.hasMotuSph = true;
    }

    if (eventCount == 0) {
        result.status = DirectRxWriteStatus::kAvailable;
        return result;
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
