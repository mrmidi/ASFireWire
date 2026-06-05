#include "TxAudioPacketWriter.hpp"

#include "DirectTxPacketEncoder.hpp"
#include "DirectTxProbe.hpp"

#include <cstddef>

namespace ASFW::AudioEngine::Direct::Tx {

uint32_t TxAudioPacketWriter::EffectiveAm824Slots(const TxAudioPacketWriteRequest& request) noexcept {
    return request.am824Slots == 0 ? request.channels : request.am824Slots;
}

TxPacketProductionResult TxAudioPacketWriter::WritePacket(const TxAudioPacketWriteRequest& request,
                                                          uint8_t* packetBytes,
                                                          uint32_t packetCapacityBytes) noexcept {
    TxPacketProductionResult result{};
    result.syt = request.syt;
    result.dbc = request.dbc;
    result.frames = 0;
    result.quadlets = 0;
    result.hasValidPhase = (request.syt != 0xFFFF);
    result.fatal = false;

    if (!reader_.IsBound()) {
        result.state = TxPacketState::InvalidGeometry;
        result.fatal = true;
        return result;
    }

    const uint32_t am824Slots = EffectiveAm824Slots(request);
    if (request.channels == 0 ||
        am824Slots < request.channels ||
        am824Slots > ASFW::Isoch::Config::kMaxAmdtpDbs) {
        result.state = TxPacketState::InvalidGeometry;
        result.fatal = true;
        return result;
    }

    if (!request.dataPacket) {
        if (!packetBytes || packetCapacityBytes < kDirectTxCipHeaderBytes) {
            result.state = TxPacketState::InvalidGeometry;
            result.fatal = true;
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
            result.state = TxPacketState::InvalidGeometry;
            result.fatal = true;
            return result;
        }

        result.state = (request.syt == 0xFFFF) ? TxPacketState::NoPhaseSilence : TxPacketState::ValidPhaseSilence;
        result.blockingResult = TxBlockingResult::NoData;
        result.hasPayloadFrames = false;
        result.frames = 0;
        result.quadlets = 2;
        return result;
    }

    if (!IsValidDirectTxGeometry(request.frameCount, request.channels, am824Slots)) {
        result.state = TxPacketState::InvalidGeometry;
        result.fatal = true;
        return result;
    }

    const uint32_t packetBytesNeeded = DirectTxPacketByteCount(request.frameCount, am824Slots);
    if (!packetBytes || packetCapacityBytes < packetBytesNeeded) {
        result.state = TxPacketState::InvalidGeometry;
        result.fatal = true;
        return result;
    }

    DirectTxProbe probe(reader_);
    const auto read = probe.Probe(DirectTxReadRequest{
        .firstFrame = request.firstFrame,
        .frameCount = request.frameCount,
        .channels = request.channels,
    });

    if (read.status == DirectTxReadStatus::kInvalidBinding ||
        read.status == DirectTxReadStatus::kInvalidRange) {
        result.state = TxPacketState::InvalidGeometry;
        result.fatal = true;
        result.blockingResult = TxBlockingResult::NoData;
        return result;
    }

    if (read.status == DirectTxReadStatus::kUnavailable) {
        result.state = (request.syt == 0xFFFF) ? TxPacketState::NoPhaseSilence : TxPacketState::ValidPhaseSilence;
        result.blockingResult = TxBlockingResult::NoData;
        return result;
    }

    const bool useSilence = read.status == DirectTxReadStatus::kUnderrun;
    const int32_t* inputFrames[kMaxDirectTxScratchFrames]{};
    if (!useSilence) {
        for (uint32_t frame = 0; frame < request.frameCount; ++frame) {
            const int32_t* frameIn = reader_.Frame(request.firstFrame + frame);
            if (!frameIn) {
                result.state = TxPacketState::InvalidGeometry;
                result.fatal = true;
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
        result.state = TxPacketState::InvalidGeometry;
        result.fatal = true;
        return result;
    }

    auto* payload = DirectTxPacketPayloadQuadlets(packetBytes);
    if (!payload) {
        result.state = TxPacketState::InvalidGeometry;
        result.fatal = true;
        return result;
    }

    for (uint32_t frame = 0; frame < request.frameCount; ++frame) {
        auto* frameOut = payload + (static_cast<size_t>(frame) * am824Slots);
        if (useSilence) {
            EncodeDirectTxSilenceFrame(request.channels, am824Slots, request.wireFormat, frameOut);
            continue;
        }

        EncodeDirectTxPcmFrame(inputFrames[frame], request.channels, am824Slots, request.wireFormat, frameOut);
    }

    if (useSilence) {
        result.state = TxPacketState::UnderrunSilence;
    } else {
        result.state = (request.syt == 0xFFFF) ? TxPacketState::NoPhaseSilence : TxPacketState::ValidPhasePcm;
    }
    result.blockingResult = TxBlockingResult::Data;
    result.hasPayloadFrames = true;
    result.frames = request.frameCount;
    result.quadlets = packetBytesNeeded / sizeof(uint32_t);
    return result;
}

} // namespace ASFW::AudioEngine::Direct::Tx
