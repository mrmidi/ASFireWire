#include "AmdtpTxPacketizer.hpp"

#include "AmdtpRateGeometry.hpp"
#include "PcmSlotCodec.hpp"
#include "../IEC61883/Syt.hpp"

namespace ASFW::Protocols::Audio::AMDTP {

// Design decisions (see ../../../README.md, Step 3):
//
// 1. Configure() selects the cadence from streamMode + sampleRate and rejects
//    anything but 48 kHz — honest failure over an untested rate path.
// 2. packetIndex comes from the caller's TxPacketSlotView; the packetizer owns
//    no cycle numbering.
// 3. Slot bytes are wire-order (big-endian); this is the single
//    logical-to-bus conversion point for the packet image.
// 4. Frame continuity is NOT owned here. The caller's TxPresentationPlan
//    states each packet's first frame; DiceTxStreamEngine owns the one
//    audio-frame cursor (documentation/TX_OWNERSHIP.md).
// 5. The default no-data packet is CIP-header-only (8 bytes) and leaves DBC
//    unchanged. The M-Audio special profile sends full-size cadence packets
//    with no-audio labels; those wire blocks advance DBC.
//
// Failure contract: PrepareNextPacket mutates no state (cadence, DBC) on any
// failure path, so a failed call can be retried with a
// corrected slot.

namespace {

constexpr uint32_t kCipHeaderBytes = 8;
constexpr uint32_t kBytesPerSlot = 4;

inline void WriteBE32(uint8_t* dest, uint32_t value) noexcept {
    dest[0] = static_cast<uint8_t>(value >> 24);
    dest[1] = static_cast<uint8_t>(value >> 16);
    dest[2] = static_cast<uint8_t>(value >> 8);
    dest[3] = static_cast<uint8_t>(value);
}

} // namespace

bool AmdtpTxPacketizer::Configure(const AmdtpStreamConfig& streamConfig,
                                  const AmdtpTxPolicy& txPolicy) noexcept {
    // Both cadence modes use the same resolved rate geometry.
    const auto geometry =
        ASFW::Encoding::AmdtpRateGeometryForSampleRate(streamConfig.sampleRate);
    if (!geometry) return false;

    AmdtpStreamConfig config = streamConfig;
    if (config.packetFraming != AmdtpStreamConfig::PacketFraming::Cip &&
        config.packetFraming != AmdtpStreamConfig::PacketFraming::Headerless) {
        return false;
    }
    // FDF (AM824 SFC) must match the actual rate, not whatever the profile
    // defaulted (profiles hardcode the 48 kHz SFC 0x02).
    config.fdf = geometry->fdf;
    if (config.dbs == 0) {
        config.dbs = static_cast<uint8_t>(config.pcmChannels + config.midiSlots);
    }
    const uint32_t requiredFrames = config.streamMode == StreamMode::Blocking
        ? geometry->sytIntervalFrames : geometry->nominalFramesPerCycle;
    if (config.dbs == 0 || config.framesPerDataPacket < requiredFrames) {
        return false;
    }
    if (config.packetFraming == AmdtpStreamConfig::PacketFraming::Headerless &&
        config.midiSlots != 0) {
        return false;
    }
    const uint32_t headerBytes =
        config.packetFraming == AmdtpStreamConfig::PacketFraming::Cip
            ? kCipHeaderBytes : 0U;
    if (config.packetFraming == AmdtpStreamConfig::PacketFraming::Headerless) {
        config.isochTag = 0;
        config.isochSync = 0;
    }

    const uint32_t dataPacketBytes =
        headerBytes + static_cast<uint32_t>(config.framesPerDataPacket) *
                              config.dbs * kBytesPerSlot;
    if (dataPacketBytes > config.maxPacketBytes) {
        return false;
    }

    streamConfig_ = config;
    txPolicy_ = txPolicy;

    IEC61883::CipHeaderConfig cipConfig{};
    cipConfig.sid = config.sid;
    cipConfig.dbs = config.dbs;
    cipConfig.fn = 0;
    cipConfig.qpc = 0;
    cipConfig.sph = false;
    cipConfig.fmt = config.fmt;
    cipConfig.fdf = config.fdf;
    cipConfig.noDataFdf =
        txPolicy.preserveFdfInNoDataPackets ? config.fdf : 0xFF;
    cipBuilder_.Configure(cipConfig);

    if (config.streamMode == StreamMode::Blocking) {
        if (!blockingCadence_.Configure(
                config.sampleRate,
                static_cast<uint8_t>(geometry->sytIntervalFrames))) {
            return false;
        }
        cadence_ = static_cast<IAmdtpCadence*>(&blockingCadence_);
    } else {
        if (!nonBlockingCadence_.Configure(config.sampleRate)) return false;
        cadence_ = &nonBlockingCadence_;
    }

    Reset(0);
    return true;
}

void AmdtpTxPacketizer::BindTimeline(AmdtpPacketTimeline* timeline) noexcept {
    timeline_ = timeline;
}

void AmdtpTxPacketizer::Reset(uint8_t initialDbc) noexcept {
    dbcCounter_.Reset(initialDbc);
    if (cadence_ != nullptr) {
        cadence_->Reset();
    }
}

bool AmdtpTxPacketizer::PrepareNextPacket(TxPacketSlotView slot,
                                          const AmdtpTimingState& timing,
                                          const TxPresentationPlan& plan,
                                          PreparedTxPacket& outPacket) noexcept {
    if (cadence_ == nullptr || timeline_ == nullptr || slot.bytes == nullptr) {
        return false;
    }

    const bool isData =
        plan.disposition == AmdtpPacketDisposition::Data && plan.frameCount > 0;
    if (isData && plan.frameCount > streamConfig_.framesPerDataPacket) {
        return false;
    }
    const uint8_t frames = isData ? static_cast<uint8_t>(plan.frameCount) : 0;
    const uint8_t cadenceBlocks =
        (!isData && txPolicy_.cadencePacketsCarryDataBlocks &&
         !txPolicy_.emptyPacketsDuringIdle)
            ? streamConfig_.framesPerDataPacket
            : uint8_t{0};
    const uint8_t wireBlocks = isData ? frames : cadenceBlocks;
    const uint32_t payloadBytes =
        static_cast<uint32_t>(wireBlocks) * streamConfig_.dbs * kBytesPerSlot;

    const bool isHeaderless = streamConfig_.packetFraming ==
                              AmdtpStreamConfig::PacketFraming::Headerless;
    const uint32_t headerBytes = isHeaderless ? 0U : kCipHeaderBytes;
    const bool isEmptyPacket = !isData && txPolicy_.emptyPacketsDuringIdle;
    const bool isSkipCycle = !isData && isHeaderless;

    const uint32_t byteCount = (isEmptyPacket || isSkipCycle)
                                   ? 0
                                   : ((isData || cadenceBlocks != 0)
                                          ? headerBytes + payloadBytes
                                          : headerBytes);

    if (slot.capacityBytes < byteCount) {
        return false; // no state advanced; caller may retry
    }

    // For an end-event family the header carries the count after this packet's blocks
    // (amdtp-stream.c:1040-1046). A NO-DATA packet carries no blocks, so both
    // conventions write the same value for it.
    const uint8_t dbc = static_cast<uint8_t>(
        (dbcCounter_.ValueForNextPacket() +
         ((txPolicy_.dbcIsEndEvent && isData) ? frames : 0U)) &
        0xFFU);

    outPacket = PreparedTxPacket{};
    outPacket.packetIndex = slot.packetIndex;
    outPacket.byteCount = byteCount;
    outPacket.isData = isData;
    outPacket.operation = isSkipCycle
                              ? PreparedTxPacket::Operation::SkipCycle
                              : PreparedTxPacket::Operation::Packet;
    outPacket.isochTag = streamConfig_.isochTag;
    outPacket.isochSync = streamConfig_.isochSync;
    outPacket.dbc = dbc;
    outPacket.dbs = streamConfig_.dbs;
    outPacket.firstAudioFrame = plan.firstAudioFrame;
    outPacket.framesInPacket = isData ? frames : 0;

    if (isData) {
        outPacket.syt = !isHeaderless && timing.txClockValid
                            ? timing.nextDataSyt
                            : IEC61883::SytFormatter::kNoInfo;

        if (!isHeaderless) {
            WriteCipHeader(slot.bytes, cipBuilder_.BuildData(dbc, outPacket.syt));
        }
        WriteDataPacketDefaults(slot.bytes, slot.capacityBytes, payloadBytes);

        if (!timeline_->ExposeDataPacket(outPacket, slot.bytes,
                                         slot.capacityBytes)) {
            return false; // bytes written but no counters advanced
        }

        dbcCounter_.AdvanceDataBlocks(frames);
    } else {
        outPacket.syt = IEC61883::SytFormatter::kNoInfo;

        if (isSkipCycle || isEmptyPacket) {
            // A headerless skip transmits no packet; a CIP empty packet carries
            // its immediate isoch header with a zero-byte payload. Neither has
            // a content header or payload in the shared buffer.
            timeline_->MarkNoDataPacket(slot.packetIndex);
        } else {
            // Some endpoints require a full-size cadence packet whose audio
            // slots carry the AM824 no-audio label. Other profiles retain the
            // header-only packet form.
            WriteCipHeader(slot.bytes, cipBuilder_.BuildNoData(dbc));
            if (cadenceBlocks != 0) {
                WriteCadencePacketFill(slot.bytes, payloadBytes);
                dbcCounter_.AdvanceDataBlocks(cadenceBlocks);
            }
            timeline_->MarkNoDataPacket(slot.packetIndex);
        }
    }

    cadence_->AdvanceCycle();
    return true;
}

void AmdtpTxPacketizer::RevertToNoData(TxPacketSlotView slot, PreparedTxPacket& packet) noexcept {
    if (!packet.isData) {
        return;
    }
    const uint8_t frames = packet.framesInPacket;
    dbcCounter_.RewindDataBlocks(frames);
    const uint8_t dbc = dbcCounter_.ValueForNextPacket();
    const bool isHeaderless = streamConfig_.packetFraming ==
                              AmdtpStreamConfig::PacketFraming::Headerless;
    if (isHeaderless) {
        packet.byteCount = 0;
        packet.operation = PreparedTxPacket::Operation::SkipCycle;
        packet.isData = false;
        packet.dbc = dbc;
        packet.framesInPacket = 0;
        packet.syt = IEC61883::SytFormatter::kNoInfo;
        timeline_->RetractNewestDataPacket(packet.packetIndex, packet.firstAudioFrame);
        return;
    }
    const bool isEmptyPacket = txPolicy_.emptyPacketsDuringIdle;
    if (isEmptyPacket) {
        packet.byteCount = 0;
        timeline_->RetractNewestDataPacket(packet.packetIndex, packet.firstAudioFrame);
    } else {
        const uint8_t cadenceBlocks =
            (txPolicy_.cadencePacketsCarryDataBlocks &&
             !txPolicy_.emptyPacketsDuringIdle)
                ? streamConfig_.framesPerDataPacket
                : uint8_t{0};
        const uint32_t payloadBytes =
            static_cast<uint32_t>(cadenceBlocks) * streamConfig_.dbs *
            kBytesPerSlot;
        packet.byteCount = cadenceBlocks == 0
                               ? kCipHeaderBytes
                               : kCipHeaderBytes + payloadBytes;
        WriteCipHeader(slot.bytes, cipBuilder_.BuildNoData(dbc));
        if (cadenceBlocks != 0) {
            WriteCadencePacketFill(slot.bytes, payloadBytes);
            dbcCounter_.AdvanceDataBlocks(cadenceBlocks);
        }
        timeline_->RetractNewestDataPacket(packet.packetIndex, packet.firstAudioFrame);
    }
    packet.isData = false;
    packet.dbc = dbc;
    packet.framesInPacket = 0;
    packet.syt = IEC61883::SytFormatter::kNoInfo;
}

const AmdtpStreamConfig& AmdtpTxPacketizer::StreamConfig() const noexcept {
    return streamConfig_;
}

const AmdtpTxPolicy& AmdtpTxPacketizer::TxPolicy() const noexcept {
    return txPolicy_;
}

bool AmdtpTxPacketizer::NextPacketWouldCarryData() const noexcept {
    return cadence_ != nullptr && cadence_->CurrentCycleIsData();
}

uint8_t AmdtpTxPacketizer::CurrentCycleDataFrames() const noexcept {
    return cadence_ != nullptr ? cadence_->CurrentCycleDataFrames() : 0;
}

void AmdtpTxPacketizer::WriteDataPacketDefaults(uint8_t* packetBytes,
                                                uint32_t packetCapacityBytes,
                                                uint32_t payloadBytes) noexcept {
    (void)packetCapacityBytes; // capacity validated by the caller

    const uint32_t headerBytes = streamConfig_.packetFraming ==
                                         AmdtpStreamConfig::PacketFraming::Cip
                                     ? kCipHeaderBytes : 0U;
    uint8_t* payload = packetBytes + headerBytes;

    if (txPolicy_.clearPayloadBeforeExposure) {
        for (uint32_t i = 0; i < payloadBytes; ++i) {
            payload[i] = 0;
        }
        // Arm every PCM slot with encoded silence, so a DATA packet whose PCM
        // never arrives is still valid on the wire: AM824 MBLA 0x40000000,
        // not label 0x00. Cross-validated with Linux amdtp-am824.c:209-220
        // (write_pcm_silence, used at :362 when no PCM is available). Slots
        // follow the playback channel map as the payload writer does.
        const uint32_t silence =
            PcmSlotCodec::EncodeFloat32(0.0f, txPolicy_.hostToDevicePcmEncoding);
        if (silence != 0) {
            const uint32_t dbs = streamConfig_.dbs;
            const uint32_t pcmSlots = streamConfig_.pcmChannels < dbs
                                          ? streamConfig_.pcmChannels
                                          : dbs;
            const bool mapUsable = txPolicy_.playbackChannelMap.FitsWithin(pcmSlots, dbs);
            const uint32_t frames = payloadBytes / (dbs * kBytesPerSlot);
            for (uint32_t frame = 0; frame < frames; ++frame) {
                for (uint32_t channel = 0; channel < pcmSlots; ++channel) {
                    const uint32_t slot =
                        mapUsable ? txPolicy_.playbackChannelMap.SlotFor(channel) : channel;
                    WriteBE32(payload + (frame * dbs + slot) * kBytesPerSlot, silence);
                }
            }
        }
    }

    if (txPolicy_.initializeNonAudioSlots &&
        streamConfig_.dbs > streamConfig_.pcmChannels) {
        const uint32_t frames = payloadBytes / (streamConfig_.dbs * kBytesPerSlot);
        for (uint32_t frame = 0; frame < frames; ++frame) {
            for (uint32_t s = streamConfig_.pcmChannels; s < streamConfig_.dbs;
                 ++s) {
                WriteBE32(payload + (frame * streamConfig_.dbs + s) * kBytesPerSlot,
                          txPolicy_.defaultNonAudioSlotWord);
            }
        }
    }
}

void AmdtpTxPacketizer::WriteCadencePacketFill(uint8_t* packetBytes,
                                               uint32_t payloadBytes) noexcept {
    const uint32_t headerBytes = streamConfig_.packetFraming ==
                                         AmdtpStreamConfig::PacketFraming::Cip
                                     ? kCipHeaderBytes : 0U;
    uint8_t* payload = packetBytes + headerBytes;
    const uint32_t blocks =
        payloadBytes / (streamConfig_.dbs * kBytesPerSlot);

    for (uint32_t block = 0; block < blocks; ++block) {
        for (uint32_t slot = 0; slot < streamConfig_.dbs; ++slot) {
            WriteBE32(payload + (block * streamConfig_.dbs + slot) * kBytesPerSlot,
                      txPolicy_.defaultNonAudioSlotWord);
        }
        for (uint32_t channel = 0; channel < streamConfig_.pcmChannels; ++channel) {
            const uint8_t slot = txPolicy_.playbackChannelMap.SlotFor(channel);
            if (slot < streamConfig_.dbs) {
                WriteBE32(payload + (block * streamConfig_.dbs + slot) * kBytesPerSlot,
                          txPolicy_.cadenceSlotWord);
            }
        }
    }
}

void AmdtpTxPacketizer::WriteCipHeader(
    uint8_t* packetBytes, const IEC61883::CipHeaderWords& header) noexcept {
    WriteBE32(packetBytes, header.q0);
    WriteBE32(packetBytes + 4, header.q1);
}

uint32_t AmdtpTxPacketizer::DataPacketBytes() const noexcept {
    return (streamConfig_.packetFraming == AmdtpStreamConfig::PacketFraming::Cip
                ? kCipHeaderBytes : 0U) + PayloadBytes();
}

uint32_t AmdtpTxPacketizer::PayloadBytes() const noexcept {
    return static_cast<uint32_t>(streamConfig_.framesPerDataPacket) *
           streamConfig_.dbs * kBytesPerSlot;
}

} // namespace ASFW::Protocols::Audio::AMDTP
