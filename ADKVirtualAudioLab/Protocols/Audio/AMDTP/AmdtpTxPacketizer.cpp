#include "AmdtpTxPacketizer.hpp"

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
// 4. Frame continuity is owned here (nextAudioFrame_, seeded by Reset);
//    AmdtpTimingState.nextAudioFrame is reserved for rebase logic
//    (Milestone 2) and ignored for now — the timeline stays gapless by
//    construction.
// 5. Golden rules (Linux amdtp + FFADO, see README): no-data packets are
//    CIP-header-only (8 bytes) with DBC carried unchanged; data packets carry
//    DBC of their first data block, advanced after emission.
//
// Failure contract: PrepareNextPacket mutates no state (cadence, DBC, frame
// counter) on any failure path, so a failed call can be retried with a
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
    if (streamConfig.sampleRate != 48000) {
        return false; // only 48 kHz cadences exist; reject untested rates
    }

    AmdtpStreamConfig config = streamConfig;
    if (config.dbs == 0) {
        config.dbs = static_cast<uint8_t>(config.pcmChannels + config.midiSlots);
    }
    if (config.dbs == 0 || config.framesPerDataPacket == 0) {
        return false;
    }

    const uint32_t dataPacketBytes =
        kCipHeaderBytes + static_cast<uint32_t>(config.framesPerDataPacket) *
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
    cipBuilder_.Configure(cipConfig);

    cadence_ = (config.streamMode == StreamMode::Blocking)
                   ? static_cast<IAmdtpCadence*>(&blocking48kCadence_)
                   : static_cast<IAmdtpCadence*>(&nonBlocking48kCadence_);

    Reset(0, 0);
    return true;
}

void AmdtpTxPacketizer::BindTimeline(AmdtpPacketTimeline* timeline) noexcept {
    timeline_ = timeline;
}

void AmdtpTxPacketizer::Reset(uint8_t initialDbc,
                              uint64_t initialAudioFrame) noexcept {
    dbcCounter_.Reset(initialDbc);
    nextAudioFrame_ = initialAudioFrame;
    if (cadence_ != nullptr) {
        cadence_->Reset();
    }
}

bool AmdtpTxPacketizer::PrepareNextPacket(TxPacketSlotView slot,
                                          const AmdtpTimingState& timing,
                                          PreparedTxPacket& outPacket) noexcept {
    if (cadence_ == nullptr || timeline_ == nullptr || slot.bytes == nullptr) {
        return false;
    }

    const bool isData = cadence_->CurrentCycleIsData();
    const uint8_t frames = cadence_->CurrentCycleDataFrames();
    const uint32_t payloadBytes =
        static_cast<uint32_t>(frames) * streamConfig_.dbs * kBytesPerSlot;
    const uint32_t byteCount =
        isData ? (kCipHeaderBytes + payloadBytes) : kCipHeaderBytes;

    if (slot.capacityBytes < byteCount) {
        return false; // no state advanced; caller may retry
    }

    const uint8_t dbc = dbcCounter_.ValueForNextPacket();

    outPacket = PreparedTxPacket{};
    outPacket.packetIndex = slot.packetIndex;
    outPacket.byteCount = byteCount;
    outPacket.isData = isData;
    outPacket.dbc = dbc;
    outPacket.dbs = streamConfig_.dbs;
    outPacket.firstAudioFrame = nextAudioFrame_;
    outPacket.framesInPacket = isData ? frames : 0;

    if (isData) {
        outPacket.syt = timing.txClockValid ? timing.nextDataSyt
                                            : IEC61883::SytFormatter::kNoInfo;

        WriteCipHeader(slot.bytes, cipBuilder_.BuildData(dbc, outPacket.syt));
        WriteDataPacketDefaults(slot.bytes, slot.capacityBytes, payloadBytes);

        if (!timeline_->ExposeDataPacket(outPacket, slot.bytes,
                                         slot.capacityBytes)) {
            return false; // bytes written but no counters advanced
        }

        dbcCounter_.AdvanceDataBlocks(frames);
        nextAudioFrame_ += frames;
    } else {
        outPacket.syt = IEC61883::SytFormatter::kNoInfo;

        // CIP-header-only: no payload, even as padding (DICE-II rejects it).
        WriteCipHeader(slot.bytes, cipBuilder_.BuildNoData(dbc));

        timeline_->MarkNoDataPacket(slot.packetIndex);
        // DBC deliberately not advanced.
    }

    cadence_->AdvanceCycle();
    return true;
}

const AmdtpStreamConfig& AmdtpTxPacketizer::StreamConfig() const noexcept {
    return streamConfig_;
}

const AmdtpTxPolicy& AmdtpTxPacketizer::TxPolicy() const noexcept {
    return txPolicy_;
}

void AmdtpTxPacketizer::WriteDataPacketDefaults(uint8_t* packetBytes,
                                                uint32_t packetCapacityBytes,
                                                uint32_t payloadBytes) noexcept {
    (void)packetCapacityBytes; // capacity validated by the caller

    uint8_t* payload = packetBytes + kCipHeaderBytes;

    if (txPolicy_.clearPayloadBeforeExposure) {
        for (uint32_t i = 0; i < payloadBytes; ++i) {
            payload[i] = 0;
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

void AmdtpTxPacketizer::WriteCipHeader(
    uint8_t* packetBytes, const IEC61883::CipHeaderWords& header) noexcept {
    WriteBE32(packetBytes, header.q0);
    WriteBE32(packetBytes + 4, header.q1);
}

uint32_t AmdtpTxPacketizer::DataPacketBytes() const noexcept {
    return kCipHeaderBytes + PayloadBytes();
}

uint32_t AmdtpTxPacketizer::PayloadBytes() const noexcept {
    return static_cast<uint32_t>(streamConfig_.framesPerDataPacket) *
           streamConfig_.dbs * kBytesPerSlot;
}

} // namespace ASFW::Protocols::Audio::AMDTP
