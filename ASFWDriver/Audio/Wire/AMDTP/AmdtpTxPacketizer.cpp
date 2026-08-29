#include "AmdtpTxPacketizer.hpp"

#include "AmdtpRateGeometry.hpp"
#include "PcmSlotCodec.hpp"
#include "../IEC61883/Syt.hpp"

namespace ASFW::Protocols::Audio::AMDTP {
namespace {
constexpr uint32_t kCipHeaderBytes = 8;
constexpr uint32_t kBytesPerSlot = 4;

void WriteBE32(uint8_t* destination, uint32_t value) noexcept {
    destination[0] = static_cast<uint8_t>(value >> 24);
    destination[1] = static_cast<uint8_t>(value >> 16);
    destination[2] = static_cast<uint8_t>(value >> 8);
    destination[3] = static_cast<uint8_t>(value);
}
} // namespace

bool AmdtpTxPacketizer::Configure(const AmdtpStreamConfig& streamConfig,
                                  const AmdtpTxPolicy& txPolicy) noexcept {
    if (streamConfig.sampleRate != 48'000 &&
        streamConfig.sampleRate != 96'000 &&
        streamConfig.sampleRate != 192'000) {
        return false;
    }
    const auto geometry =
        ASFW::Encoding::AmdtpRateGeometryForSampleRate(streamConfig.sampleRate);
    if (!geometry || streamConfig.dbs == 0 ||
        streamConfig.framesPerDataPacket == 0 ||
        streamConfig.pcmChannels > streamConfig.dbs ||
        streamConfig.pcmChannels > ASFW::Encoding::kMaxPcmChannels ||
        !txPolicy.playbackChannelMap.FitsWithin(
            streamConfig.pcmChannels, streamConfig.dbs)) {
        return false;
    }
    const uint32_t maximumBytes = kCipHeaderBytes +
        static_cast<uint32_t>(streamConfig.framesPerDataPacket) *
            streamConfig.dbs * kBytesPerSlot;
    if (maximumBytes > streamConfig.maxPacketBytes) return false;

    streamConfig_ = streamConfig;
    streamConfig_.fdf = geometry->fdf;
    txPolicy_ = txPolicy;
    IEC61883::CipHeaderConfig config{};
    config.sid = streamConfig_.sid;
    config.dbs = streamConfig_.dbs;
    config.fmt = streamConfig_.fmt;
    config.fdf = streamConfig_.fdf;
    config.noDataFdf = txPolicy_.preserveFdfInNoDataPackets
        ? streamConfig_.fdf : 0xFF;
    cipBuilder_.Configure(config);
    Reset();
    return true;
}

void AmdtpTxPacketizer::BindTimeline(AmdtpPacketTimeline* timeline) noexcept {
    timeline_ = timeline;
}

void AmdtpTxPacketizer::Reset(uint8_t initialDbc) noexcept {
    dbcCounter_.Reset(initialDbc);
}

bool AmdtpTxPacketizer::PrepareDataPacket(
    TxPacketSlotView slot,
    const TxPresentationPlan& plan,
    uint8_t wireDataBlocks,
    uint16_t syt,
    const TxPcmSnapshotView& pcm,
    PreparedTxPacket& outPacket) noexcept {
    if (!timeline_ || !slot.bytes || plan.epoch == 0 ||
        wireDataBlocks > streamConfig_.framesPerDataPacket) {
        return false;
    }
    const bool isData = plan.disposition == AmdtpPacketDisposition::Data;
    if (isData && (plan.frameCount == 0 ||
                   plan.frameCount != wireDataBlocks ||
                   !pcm.interleavedFloat32 ||
                   pcm.frameCount != plan.frameCount ||
                   pcm.channels < streamConfig_.pcmChannels)) {
        return false;
    }

    const bool empty = !isData && txPolicy_.emptyPacketsDuringIdle;
    const uint8_t blocks = isData
        ? wireDataBlocks
        : (empty || !txPolicy_.cadencePacketsCarryDataBlocks
               ? uint8_t{0}
               : streamConfig_.framesPerDataPacket);
    const uint32_t payloadBytes =
        static_cast<uint32_t>(blocks) * streamConfig_.dbs * kBytesPerSlot;
    const uint32_t byteCount = empty ? 0 : kCipHeaderBytes + payloadBytes;
    if (slot.capacityBytes < byteCount) return false;

    outPacket = {
        .packetIndex = slot.packetIndex,
        .byteCount = byteCount,
        .isData = isData,
        .dbc = dbcCounter_.ValueForNextPacket(),
        .syt = isData ? syt : IEC61883::SytFormatter::kNoInfo,
        .firstAudioFrame = plan.firstAudioFrame,
        .framesInPacket = isData ? plan.frameCount : 0,
        .plannedFrameCount = plan.frameCount,
        .dbs = streamConfig_.dbs,
        .epoch = plan.epoch,
        .cycleOrdinal = plan.cycleOrdinal,
        .presentationBusTicks = plan.presentationBusTicks,
        .pcmFinalized = false,
    };

    if (isData) {
        WriteCipHeader(slot.bytes,
                       cipBuilder_.BuildData(outPacket.dbc, outPacket.syt));
        WriteDataPacketDefaults(slot.bytes, slot.capacityBytes, payloadBytes);
        WritePcmSnapshot(slot.bytes, outPacket, pcm);
        outPacket.pcmFinalized = true;
    } else if (!empty) {
        WriteCipHeader(slot.bytes, cipBuilder_.BuildNoData(outPacket.dbc));
        if (blocks != 0) WriteCadencePacketFill(slot.bytes, payloadBytes);
    }
    return true;
}

bool AmdtpTxPacketizer::CommitPreparedPacket(
    const PreparedTxPacket& packet,
    uint8_t wireDataBlocks) noexcept {
    if (!timeline_ || (packet.isData && !packet.pcmFinalized)) return false;
    if (packet.isData) {
        if (!timeline_->MarkDataPacketFinalized(packet)) return false;
    } else {
        timeline_->MarkNoDataPacket(packet);
    }
    timeline_->MarkPublished(packet.packetIndex);
    const uint8_t committedBlocks = packet.isData
        ? wireDataBlocks
        : (txPolicy_.cadencePacketsCarryDataBlocks
               ? streamConfig_.framesPerDataPacket
               : uint8_t{0});
    dbcCounter_.AdvanceDataBlocks(committedBlocks);
    return true;
}

const AmdtpStreamConfig& AmdtpTxPacketizer::StreamConfig() const noexcept {
    return streamConfig_;
}

const AmdtpTxPolicy& AmdtpTxPacketizer::TxPolicy() const noexcept {
    return txPolicy_;
}

void AmdtpTxPacketizer::WriteDataPacketDefaults(
    uint8_t* packetBytes, uint32_t, uint32_t payloadBytes) noexcept {
    uint8_t* payload = packetBytes + kCipHeaderBytes;
    for (uint32_t index = 0; index < payloadBytes; ++index) payload[index] = 0;
    if (!txPolicy_.initializeNonAudioSlots) return;
    const uint32_t blocks = payloadBytes /
        (streamConfig_.dbs * kBytesPerSlot);
    for (uint32_t block = 0; block < blocks; ++block) {
        for (uint32_t slot = 0; slot < streamConfig_.dbs; ++slot) {
            WriteBE32(payload + (block * streamConfig_.dbs + slot) * 4,
                      txPolicy_.defaultNonAudioSlotWord);
        }
    }
}

void AmdtpTxPacketizer::WriteCadencePacketFill(
    uint8_t* packetBytes, uint32_t payloadBytes) noexcept {
    WriteDataPacketDefaults(packetBytes, payloadBytes, payloadBytes);
    uint8_t* payload = packetBytes + kCipHeaderBytes;
    const uint32_t blocks = payloadBytes /
        (streamConfig_.dbs * kBytesPerSlot);
    for (uint32_t block = 0; block < blocks; ++block) {
        for (uint32_t channel = 0; channel < streamConfig_.pcmChannels;
             ++channel) {
            WriteBE32(payload +
                          (block * streamConfig_.dbs +
                           txPolicy_.playbackChannelMap.SlotFor(channel)) * 4,
                      txPolicy_.cadenceSlotWord);
        }
    }
}

void AmdtpTxPacketizer::WritePcmSnapshot(
    uint8_t* packetBytes, const PreparedTxPacket& packet,
    const TxPcmSnapshotView& pcm) noexcept {
    uint8_t* payload = packetBytes + kCipHeaderBytes;
    for (uint32_t frame = 0; frame < packet.framesInPacket; ++frame) {
        const float* source = pcm.interleavedFloat32 +
            static_cast<uint64_t>(frame) * pcm.channels;
        uint8_t* destination = payload +
            static_cast<uint64_t>(frame) * packet.dbs * kBytesPerSlot;
        for (uint32_t channel = 0; channel < streamConfig_.pcmChannels;
             ++channel) {
            WriteBE32(destination +
                          txPolicy_.playbackChannelMap.SlotFor(channel) * 4,
                      PcmSlotCodec::EncodeFloat32(
                          source[channel],
                          txPolicy_.hostToDevicePcmEncoding));
        }
    }
}

void AmdtpTxPacketizer::WriteCipHeader(
    uint8_t* packetBytes, const IEC61883::CipHeaderWords& header) noexcept {
    WriteBE32(packetBytes, header.q0);
    WriteBE32(packetBytes + 4, header.q1);
}

} // namespace ASFW::Protocols::Audio::AMDTP
