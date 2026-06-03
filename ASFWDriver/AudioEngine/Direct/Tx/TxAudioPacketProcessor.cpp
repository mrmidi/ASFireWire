#include "TxAudioPacketProcessor.hpp"

#include "DirectTxProbe.hpp"
#include "DirectTxScratchEncoder.hpp"

#include <cstddef>

namespace ASFW::AudioEngine::Direct::Tx {

uint32_t TxAudioPacketProcessor::EffectiveAm824Slots(const TxAudioPacketRequest& request) noexcept {
    return request.am824Slots == 0 ? request.channels : request.am824Slots;
}

TxAudioPacketResult TxAudioPacketProcessor::BuildScratchPacket(const TxAudioPacketRequest& request,
                                                               DirectTxPacketScratch& scratch) noexcept {
    scratch.Reset();

    TxAudioPacketResult result{};

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
        const DirectTxScratchHeaderRequest header{
            .sid = request.sid,
            .am824Slots = am824Slots,
            .dbc = request.dbc,
            .syt = ASFW::Encoding::kSYTNoData,
            .isNoData = true,
        };
        if (!BeginDirectTxScratchPacket(header, scratch)) {
            result.readStatus = DirectTxReadStatus::kInvalidRange;
            return result;
        }

        result.readStatus = DirectTxReadStatus::kAvailable;
        return result;
    }

    if (!IsValidDirectTxGeometry(request.frameCount, request.channels, am824Slots)) {
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
    const DirectTxScratchHeaderRequest header{
        .sid = request.sid,
        .am824Slots = am824Slots,
        .dbc = request.dbc,
        .syt = request.syt,
        .isNoData = false,
    };
    if (!BeginDirectTxScratchPacket(header, scratch)) {
        result.readStatus = DirectTxReadStatus::kInvalidRange;
        return result;
    }

    auto* payload = DirectTxScratchPayloadQuadlets(scratch);
    for (uint32_t frame = 0; frame < request.frameCount; ++frame) {
        auto* frameOut = payload + (static_cast<size_t>(frame) * am824Slots);
        if (useSilence) {
            EncodeDirectTxSilenceFrameToAm824(request.channels, am824Slots, frameOut);
            continue;
        }

        const int32_t* frameIn = reader_.Frame(request.firstFrame + frame);
        if (!frameIn) {
            result.readStatus = DirectTxReadStatus::kInvalidBinding;
            scratch.Reset();
            return result;
        }
        EncodeDirectTxPcmFrameToAm824(frameIn, request.channels, am824Slots, frameOut);
    }

    scratch.length = DirectTxPacketByteCount(request.frameCount, am824Slots);
    scratch.framesEncoded = request.frameCount;
    scratch.usedSilence = useSilence;

    result.framesEncoded = request.frameCount;
    result.usedSilence = useSilence;
    return result;
}

} // namespace ASFW::AudioEngine::Direct::Tx
