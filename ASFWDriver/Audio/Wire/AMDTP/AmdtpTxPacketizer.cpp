#include "AmdtpTxPacketizer.hpp"

#include "AmdtpRateGeometry.hpp"
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
    // Resolve the rate's AMDTP geometry (SYT interval + AM824 FDF/SFC). Unknown
    // rates are rejected. Blocking mode handles any rate via the rational
    // cadence; non-blocking has no fractional form, so it stays integral-rate
    // (48 kHz) only.
    const auto geometry =
        ASFW::Encoding::AmdtpRateGeometryForSampleRate(streamConfig.sampleRate);
    if (!geometry) {
        return false;
    }
    if (streamConfig.streamMode != StreamMode::Blocking &&
        streamConfig.sampleRate != 48000) {
        return false;
    }

    AmdtpStreamConfig config = streamConfig;
    // FDF (AM824 SFC) must match the actual rate, not whatever the profile
    // defaulted (profiles hardcode the 48 kHz SFC 0x02).
    config.fdf = geometry->fdf;
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
    cipConfig.noDataFdf =
        txPolicy.preserveFdfInNoDataPackets ? config.fdf : 0xFF;
    cipBuilder_.Configure(cipConfig);

    if (config.streamMode == StreamMode::Blocking) {
        if (!blocking48kCadence_.Configure(
                config.sampleRate,
                static_cast<uint8_t>(geometry->sytIntervalFrames))) {
            return false;
        }
        cadence_ = static_cast<IAmdtpCadence*>(&blocking48kCadence_);
    } else {
        cadence_ = static_cast<IAmdtpCadence*>(&nonBlocking48kCadence_);
    }

    Reset(0, 0);
    return true;
}

void AmdtpTxPacketizer::BindTimeline(AmdtpPacketTimeline* timeline) noexcept {
    timeline_ = timeline;
}

void AmdtpTxPacketizer::Reset(uint8_t initialDbc,
                              uint64_t initialAudioFrame) noexcept {
    dbcCounter_.Reset(initialDbc);
    frameCursorAligned_ = false;
    ++cursorEpoch_;
    lastDataFirstAudioFrame_ = 0;
    lastDataEndAudioFrame_ = 0;
    lastDataPacketIndex_ = 0;
    hasLastDataPacket_ = false;
    telemetryNextAudioFrame_.store(initialAudioFrame, std::memory_order_relaxed);
    if (cadence_ != nullptr) {
        cadence_->Reset();
    }
    PublishTelemetrySnapshot();
}

bool AmdtpTxPacketizer::AlignFrameCursorOnce(uint64_t frameIndex) noexcept {
    if (frameCursorAligned_) {
        return false;
    }
    frameCursorAligned_ = true;
    ++cursorEpoch_;
    telemetryNextAudioFrame_.store(frameIndex, std::memory_order_relaxed);
    PublishTelemetrySnapshot();
    return true;
}

void AmdtpTxPacketizer::ReArmFrameCursorAlignment() noexcept {
    if (!frameCursorAligned_) {
        return;
    }
    frameCursorAligned_ = false;
    ++cursorEpoch_;
    PublishTelemetrySnapshot();
}

void AmdtpTxPacketizer::SetPresentationCursor(uint64_t epoch, uint64_t frame, bool aligned) noexcept {
    cursorEpoch_ = epoch;
    frameCursorAligned_ = aligned;
    telemetryNextAudioFrame_.store(frame, std::memory_order_relaxed);
    PublishTelemetrySnapshot();
}

bool AmdtpTxPacketizer::PrepareNextPacket(TxPacketSlotView slot,
                                          const AmdtpTimingState& timing,
                                          const TxPresentationPlan& plan,
                                          PreparedTxPacket& outPacket) noexcept {
    if (cadence_ == nullptr || timeline_ == nullptr || slot.bytes == nullptr) {
        return false;
    }

    if (plan.epoch != 0 && plan.epoch != cursorEpoch_) {
        return false; // Stale or rejected epoch cannot publish
    }

    const bool isData =
        plan.disposition == AmdtpPacketDisposition::Data && plan.frameCount > 0;
    if (isData && plan.frameCount > streamConfig_.framesPerDataPacket) {
        return false;
    }
    const uint8_t frames = isData ? static_cast<uint8_t>(plan.frameCount) : 0;
    const uint32_t payloadBytes =
        static_cast<uint32_t>(frames) * streamConfig_.dbs * kBytesPerSlot;

    const bool isEmptyPacket = !isData && txPolicy_.emptyPacketsDuringIdle;

    const uint32_t byteCount =
        isEmptyPacket ? 0 : (isData ? (kCipHeaderBytes + payloadBytes) : kCipHeaderBytes);

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
    outPacket.dbc = dbc;
    outPacket.dbs = streamConfig_.dbs;
    outPacket.firstAudioFrame = plan.firstAudioFrame;
    outPacket.framesInPacket = isData ? frames : 0;

    if (isData) {
        outPacket.syt = timing.txClockValid
                            ? timing.nextDataSyt
                            : IEC61883::SytFormatter::kNoInfo;

        WriteCipHeader(slot.bytes, cipBuilder_.BuildData(dbc, outPacket.syt));
        WriteDataPacketDefaults(slot.bytes, slot.capacityBytes, payloadBytes);

        if (!timeline_->ExposeDataPacket(outPacket, slot.bytes,
                                         slot.capacityBytes)) {
            return false; // bytes written but no counters advanced
        }

        dbcCounter_.AdvanceDataBlocks(frames);
        lastDataFirstAudioFrame_ = outPacket.firstAudioFrame;
        lastDataEndAudioFrame_ = plan.firstAudioFrame + frames;
        lastDataPacketIndex_ = outPacket.packetIndex;
        hasLastDataPacket_ = true;
        telemetryNextAudioFrame_.store(lastDataEndAudioFrame_, std::memory_order_release);
        PublishTelemetrySnapshot();
    } else {
        outPacket.syt = IEC61883::SytFormatter::kNoInfo;

        if (isEmptyPacket) {
            // Emitting genuine empty packets: byteCount = 0. No CIP header or payload is written.
            timeline_->MarkNoDataPacket(slot.packetIndex);
        } else {
            // CIP-header-only: no payload, even as padding (DICE-II rejects it).
            WriteCipHeader(slot.bytes, cipBuilder_.BuildNoData(dbc));
            timeline_->MarkNoDataPacket(slot.packetIndex);
            // DBC deliberately not advanced.
        }
    }

    cadence_->AdvanceCycle();
    return true;
}

bool AmdtpTxPacketizer::PrepareNextPacket(TxPacketSlotView slot,
                                          const AmdtpTimingState& timing,
                                          PreparedTxPacket& outPacket) noexcept {
    if (cadence_ == nullptr) {
        return false;
    }

    const bool cadenceData = cadence_->CurrentCycleIsData();
    const bool isData =
        timing.disposition == AmdtpPacketDisposition::Data &&
        (timing.replayValid
             ? timing.replayDataBlocks != 0
             : cadenceData);
    const uint8_t frames =
        isData
            ? static_cast<uint8_t>(
                  timing.replayValid
                      ? timing.replayDataBlocks
                      : cadence_->CurrentCycleDataFrames())
            : 0;

    TxPresentationPlan plan{};
    plan.epoch = cursorEpoch_;
    plan.cycleOrdinal = slot.packetIndex;
    plan.firstAudioFrame = telemetryNextAudioFrame_.load(std::memory_order_relaxed);
    plan.frameCount = frames;
    plan.disposition =
        isData ? AmdtpPacketDisposition::Data : AmdtpPacketDisposition::NoData;

    return PrepareNextPacket(slot, timing, plan, outPacket);
}

void AmdtpTxPacketizer::RevertToNoData(TxPacketSlotView slot, PreparedTxPacket& packet) noexcept {
    if (!packet.isData) {
        return;
    }
    const uint8_t frames = packet.framesInPacket;
    dbcCounter_.RewindDataBlocks(frames);
    const uint8_t dbc = dbcCounter_.ValueForNextPacket();
    const bool isEmptyPacket = txPolicy_.emptyPacketsDuringIdle;
    if (isEmptyPacket) {
        packet.byteCount = 0;
        timeline_->MarkNoDataPacket(packet.packetIndex);
    } else {
        packet.byteCount = kCipHeaderBytes;
        WriteCipHeader(slot.bytes, cipBuilder_.BuildNoData(dbc));
        timeline_->MarkNoDataPacket(packet.packetIndex);
    }
    packet.isData = false;
    packet.dbc = dbc;
    telemetryNextAudioFrame_.store(packet.firstAudioFrame, std::memory_order_relaxed);
    packet.framesInPacket = 0;
    packet.syt = IEC61883::SytFormatter::kNoInfo;
    hasLastDataPacket_ = false;
    PublishTelemetrySnapshot();
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

AmdtpTxPacketizerTelemetrySnapshot
AmdtpTxPacketizer::TelemetrySnapshot() const noexcept {
    AmdtpTxPacketizerTelemetrySnapshot snapshot{};
    snapshot.nextAudioFrame =
        telemetryNextAudioFrame_.load(std::memory_order_acquire);
    snapshot.lastDataFirstAudioFrame =
        telemetryLastDataFirstAudioFrame_.load(std::memory_order_relaxed);
    snapshot.lastDataEndAudioFrame =
        telemetryLastDataEndAudioFrame_.load(std::memory_order_relaxed);
    snapshot.lastDataPacketIndex =
        telemetryLastDataPacketIndex_.load(std::memory_order_relaxed);
    snapshot.cursorEpoch = telemetryCursorEpoch_.load(std::memory_order_relaxed);
    snapshot.frameCursorAligned =
        telemetryFrameCursorAligned_.load(std::memory_order_relaxed);
    snapshot.hasLastDataPacket =
        telemetryHasLastDataPacket_.load(std::memory_order_relaxed);
    return snapshot;
}

void AmdtpTxPacketizer::PublishTelemetrySnapshot() noexcept {
    // Publish data fields before the acquire load of nextAudioFrame in
    // TelemetrySnapshot(). The snapshot is deliberately best-effort: it is
    // diagnostic only and never participates in packet preparation.
    telemetryLastDataFirstAudioFrame_.store(lastDataFirstAudioFrame_,
                                            std::memory_order_relaxed);
    telemetryLastDataEndAudioFrame_.store(lastDataEndAudioFrame_,
                                          std::memory_order_relaxed);
    telemetryLastDataPacketIndex_.store(lastDataPacketIndex_,
                                        std::memory_order_relaxed);
    telemetryCursorEpoch_.store(cursorEpoch_, std::memory_order_relaxed);
    telemetryFrameCursorAligned_.store(frameCursorAligned_,
                                       std::memory_order_relaxed);
    telemetryHasLastDataPacket_.store(hasLastDataPacket_,
                                      std::memory_order_relaxed);
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
