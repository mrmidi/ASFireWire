#include "RxAudioPacketProcessor.hpp"
#include "DirectRxPacketDecoder.hpp"
#include "../../../Wire/CIP/CIPHeader.hpp"

#include <algorithm>
#include <cstring>

namespace ASFW::AudioEngine::Direct::Rx {

static constexpr size_t kIsochHeaderSize = 8; // Timestamp + isoch header

RxAudioPacketProcessorResult RxAudioPacketProcessor::ProcessPacket(const uint8_t* payload,
                                                                   size_t length,
                                                                   uint64_t absoluteFrame,
                                                                   uint32_t channels,
                                                                   uint32_t am824Slots,
                                                                   ASFW::Encoding::AudioWireFormat format) noexcept {
    RxAudioPacketProcessorResult result{};

    if (length < kIsochHeaderSize + 8) {
        result.status = DirectRxWriteStatus::kInvalidRange;
        return result;
    }

    const uint8_t* cipStart = payload + kIsochHeaderSize;
    const auto* quadlets = reinterpret_cast<const uint32_t*>(cipStart);
    
    // Decode CIP Header (quadlets[0] and quadlets[1])
    const auto cip = ASFW::Isoch::CIPHeader::Decode(quadlets[0], quadlets[1]);
    if (!cip) {
        result.status = DirectRxWriteStatus::kInvalidRange;
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
        result.status = DirectRxWriteStatus::kInvalidRange;
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
        cip->dataBlockSize < channels ||
        am824Slots != cip->dataBlockSize) {
        result.status = DirectRxWriteStatus::kInvalidRange;
        return result;
    }

    // If armed: decode quadlets directly to ADK input memory
    const uint32_t* dataBlocks = &quadlets[2];
    for (size_t i = 0; i < eventCount; ++i) {
        int32_t* frameOut = writer_.Frame(absoluteFrame + i);
        if (!frameOut) {
            result.status = DirectRxWriteStatus::kInvalidRange;
            return result;
        }

        const uint32_t* frameIn = dataBlocks + (i * cip->dataBlockSize);
        DecodeDirectRxFrame(frameIn, channels, cip->dataBlockSize, format, frameOut);
    }

    const uint64_t producedEnd = absoluteFrame + eventCount;
    writer_.PublishProducedEnd(producedEnd, static_cast<uint32_t>(eventCount));
    
    result.status = DirectRxWriteStatus::kAvailable;
    return result;
}

} // namespace ASFW::AudioEngine::Direct::Rx
