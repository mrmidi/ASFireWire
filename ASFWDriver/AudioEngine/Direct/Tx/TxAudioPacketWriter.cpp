#include "TxAudioPacketWriter.hpp"

#include "DirectTxPacketEncoder.hpp"
#include "DirectTxProbe.hpp"

#include <cstddef>

namespace ASFW::AudioEngine::Direct::Tx {

uint32_t TxAudioPacketWriter::EffectiveAm824Slots(const TxAudioPacketWriteRequest& request) noexcept {
    return request.am824Slots == 0 ? request.channels : request.am824Slots;
}

TxAudioPacketWriteResult TxAudioPacketWriter::WritePacket(const TxAudioPacketWriteRequest& request,
                                                          uint8_t* packetBytes,
                                                          uint32_t packetCapacityBytes) noexcept {
    TxAudioPacketWriteResult result{};

    if (!reader_.IsBound()) {
        result.readStatus = DirectTxReadStatus::kInvalidBinding;
        return result;
    }

    const uint32_t am824Slots = EffectiveAm824Slots(request);
    if (request.channels == 0 ||
        am824Slots < request.channels ||
        am824Slots > ASFW::Isoch::Config::kMaxAmdtpDbs) {
        result.readStatus = DirectTxReadStatus::kInvalidRange;
        return result;
    }

    if (!request.dataPacket) {
        if (!packetBytes || packetCapacityBytes < kDirectTxCipHeaderBytes) {
            result.readStatus = DirectTxReadStatus::kInvalidRange;
            return result;
        }

        uint32_t bytesWritten = 0;
        const DirectTxPacketHeaderRequest header{
            .sid = request.sid,
            .am824Slots = am824Slots,
            .dbc = request.dbc,
            .syt = ASFW::Encoding::kSYTNoData,
            .isNoData = true,
        };
        if (!BeginDirectTxPacket(header, packetBytes, packetCapacityBytes, bytesWritten)) {
            result.readStatus = DirectTxReadStatus::kInvalidRange;
            return result;
        }

        result.readStatus = DirectTxReadStatus::kAvailable;
        result.bytesWritten = bytesWritten;
        return result;
    }

    if (!IsValidDirectTxGeometry(request.frameCount, request.channels, am824Slots)) {
        result.readStatus = DirectTxReadStatus::kInvalidRange;
        return result;
    }

    const uint32_t packetBytesNeeded = DirectTxPacketByteCount(request.frameCount, am824Slots);
    if (!packetBytes || packetCapacityBytes < packetBytesNeeded) {
        result.readStatus = DirectTxReadStatus::kInvalidRange;
        return result;
    }

    DirectTxProbe probe(reader_);
    const auto read = probe.Probe(DirectTxReadRequest{
        .firstFrame = request.firstFrame,
        .frameCount = request.frameCount,
        .channels = request.channels,
    });
    result.readStatus = read.status;

    if (read.status != DirectTxReadStatus::kAvailable &&
        read.status != DirectTxReadStatus::kUnderrun) {
        return result;
    }

    const bool useSilence = read.status == DirectTxReadStatus::kUnderrun;
    const int32_t* inputFrames[kMaxDirectTxScratchFrames]{};
    if (!useSilence) {
        for (uint32_t frame = 0; frame < request.frameCount; ++frame) {
            const int32_t* frameIn = reader_.Frame(request.firstFrame + frame);
            if (!frameIn) {
                result.readStatus = DirectTxReadStatus::kInvalidBinding;
                return result;
            }
            inputFrames[frame] = frameIn;
        }
    }

    uint32_t bytesWritten = 0;
    const DirectTxPacketHeaderRequest header{
        .sid = request.sid,
        .am824Slots = am824Slots,
        .dbc = request.dbc,
        .syt = request.syt,
        .isNoData = false,
    };
    if (!BeginDirectTxPacket(header, packetBytes, packetCapacityBytes, bytesWritten)) {
        result.readStatus = DirectTxReadStatus::kInvalidRange;
        return result;
    }

    auto* payload = DirectTxPacketPayloadQuadlets(packetBytes);
    if (!payload) {
        result.readStatus = DirectTxReadStatus::kInvalidRange;
        return result;
    }

    for (uint32_t frame = 0; frame < request.frameCount; ++frame) {
        auto* frameOut = payload + (static_cast<size_t>(frame) * am824Slots);
        if (useSilence) {
            EncodeDirectTxSilenceFrameToAm824(request.channels, am824Slots, frameOut);
            continue;
        }

        EncodeDirectTxPcmFrameToAm824(inputFrames[frame], request.channels, am824Slots, frameOut);
    }

    result.bytesWritten = packetBytesNeeded;
    result.framesEncoded = request.frameCount;
    result.usedSilence = useSilence;
    return result;
}

} // namespace ASFW::AudioEngine::Direct::Tx
