#include "DiceTxStreamEngine.hpp"

#include <span>

namespace ASFW::Protocols::Audio::DICE {

AMDTP::AmdtpStreamConfig DiceStreamConfigMapper::ToAmdtpConfig(
    const ASFW::Isoch::Audio::AudioStreamConfig& streamConfig) noexcept {
    AMDTP::AmdtpStreamConfig config{};
    config.sampleRate = streamConfig.sampleRate;
    config.streamMode = (streamConfig.streamMode == ASFW::Encoding::StreamMode::kBlocking)
                        ? AMDTP::StreamMode::Blocking 
                        : AMDTP::StreamMode::NonBlocking;
    config.sid = streamConfig.sid;
    config.dbs = streamConfig.dbs;
    config.pcmChannels = streamConfig.pcmChannels;
    config.midiSlots = streamConfig.midiSlots;
    config.fmt = streamConfig.fmt;
    config.fdf = streamConfig.fdf;
    config.framesPerDataPacket = streamConfig.framesPerDataPacket;
    config.sourceChannelOffset = streamConfig.sourceChannelOffset;
    // Compute the true max packet size: CIP headers (8 bytes) + frames × DBS × 4 bytes/slot.
    // Do not use the AmdtpStreamConfig default (512) — it is too small for high-channel
    // devices (e.g. a 24-channel device with DBS=24 needs 776 bytes at 8 fpd).
    config.maxPacketBytes = 8u +
        static_cast<uint32_t>(streamConfig.framesPerDataPacket) * streamConfig.dbs * 4u;
    return config;
}

bool DiceTxStreamEngine::Configure(const ASFW::Isoch::Audio::IAudioStreamProfile& profile,
                                   const ASFW::Isoch::Audio::AudioStreamConfig& txConfig) noexcept {
    if (txConfig.direction != ASFW::Isoch::Audio::AudioStreamDirection::HostToDevice) {
        return false;
    }

    const ASFW::Isoch::Audio::AudioStreamTxPolicy txPolicy = profile.TxStreamPolicy();
    const AMDTP::AmdtpStreamConfig amdtpConfig =
        DiceStreamConfigMapper::ToAmdtpConfig(txConfig);
    const AMDTP::AmdtpTxPolicy policy = BuildTxPolicy(txPolicy);

    const uint32_t slotCount =
        static_cast<uint32_t>(sizeof(timelineSlots_) / sizeof(timelineSlots_[0]));
    if (!timeline_.AttachSlots(timelineSlots_, slotCount)) {
        return false;
    }

    packetizer_.BindTimeline(&timeline_);
    if (!packetizer_.Configure(amdtpConfig, policy)) {
        return false;
    }
    payloadWriter_.Configure(packetizer_.StreamConfig(), policy);
    payloadWriter_.BindTimeline(&timeline_);

    // MOTU is not a quadlet-slot format: its chunk layout needs a different writer, and
    // the AMDTP one must not also run or it would overwrite the block with slot-shaped
    // samples.
    isMotu_ = (txPolicy.hostToDevicePcmEncoding ==
               ASFW::Encoding::AudioWireFormat::kMotuV2);
    motuPcmChunks_ = isMotu_ ? txConfig.pcmChannels : 0U;
    if (isMotu_) {
        motuPayloadWriter_.Configure(
            ::ASFW::Encoding::Motu::MotuPayloadStreamConfig{
                .pcmChunks = motuPcmChunks_,
                .sourceChannelOffset = packetizer_.StreamConfig().sourceChannelOffset});
        motuPayloadWriter_.BindTimeline(&timeline_);
    }

    profile_ = &profile;
    streamConfig_ = txConfig;
    txPolicy_ = txPolicy;
    return true;
}

void DiceTxStreamEngine::BindSlotProvider(
    AMDTP::IAmdtpTxSlotProvider* slotProvider) noexcept {
    slotProvider_ = slotProvider;
}

void DiceTxStreamEngine::ResetForStart(uint8_t initialDbc,
                                       uint64_t initialAudioFrame) noexcept {
    timeline_.Reset();
    packetizer_.Reset(initialDbc, initialAudioFrame);
}

bool DiceTxStreamEngine::AlignFrameCursorOnce(uint64_t frameIndex) noexcept {
    return packetizer_.AlignFrameCursorOnce(frameIndex);
}

void DiceTxStreamEngine::ReArmFrameCursorAlignment() noexcept {
    packetizer_.ReArmFrameCursorAlignment();
}

bool DiceTxStreamEngine::IsFrameCursorAligned() const noexcept {
    return packetizer_.IsFrameCursorAligned();
}

TxSlotPrepareResult DiceTxStreamEngine::PrepareNextTransmitSlot(
    uint32_t packetIndex, const AMDTP::AmdtpTimingState& timing) noexcept {
    if (slotProvider_ == nullptr) {
        return TxSlotPrepareResult::kSlotProviderUnavailable;
    }

    AMDTP::TxPacketSlotView slot{};
    if (!slotProvider_->AcquireWritableSlot(packetIndex, slot)) {
        counters_.slotAcquireFailures.fetch_add(1, std::memory_order_relaxed);
        return TxSlotPrepareResult::kSlotAcquireFailed;
    }

    AMDTP::PreparedTxPacket packet{};
    if (!packetizer_.PrepareNextPacket(slot, timing, packet)) {
        return TxSlotPrepareResult::kPacketizerRejected;
    }

    // MOTU needs a source packet header on every data block, replayed from the timing the
    // device itself sent (motu-stream.c:205-207). Stamp before publishing: an unstamped
    // block reaches the device carrying whatever the slot held before.
    if (isMotu_ && packet.isData && packet.framesInPacket > 0) {
        StampMotuSph(slot, packet, timing);
    }

    if (!slotProvider_->PublishSlot(packet)) {
        return TxSlotPrepareResult::kSlotPublishFailed;
    }

    counters_.packetsPrepared.fetch_add(1, std::memory_order_relaxed);
    if (packet.isData) {
        counters_.dataPacketsPrepared.fetch_add(1, std::memory_order_relaxed);
    } else {
        counters_.noDataPacketsPrepared.fetch_add(1, std::memory_order_relaxed);
    }
    return TxSlotPrepareResult::kPrepared;
}

void DiceTxStreamEngine::StampMotuSph(const AMDTP::TxPacketSlotView& slot,
                                      const AMDTP::PreparedTxPacket& packet,
                                      const AMDTP::AmdtpTimingState& timing) noexcept {
    // The base must be the cycle this packet actually goes out in: write_sph adds each
    // captured offset to it (amdtp-motu.c:379). Without an anchored cycle there is no
    // correct SPH to write, so leave the block alone rather than invent one.
    if (motuOffsetCache_ == nullptr || slot.bytes == nullptr || !timing.transmitCycleValid) {
        return;
    }
    // Drain exactly one offset per data block. Take() is all-or-nothing, so a cache that
    // has not caught up leaves the packet unstamped rather than half-timed.
    // AmdtpStreamConfig::framesPerDataPacket is a uint8_t, so 256 bounds every value a
    // packet can carry; the stack array avoids an allocation on the transmit path.
    constexpr uint32_t kMaxBlocksPerPacket = 256;
    uint32_t offsets[kMaxBlocksPerPacket]{};
    const uint32_t blocks = (packet.framesInPacket < kMaxBlocksPerPacket)
                                ? packet.framesInPacket
                                : kMaxBlocksPerPacket;
    if (motuOffsetCache_->Take(std::span<uint32_t>(offsets, blocks))) {
        (void)::ASFW::Encoding::Motu::WritePacketSph(
            std::span<uint8_t>(slot.bytes, packet.byteCount), packet.dbs, blocks,
            timing.transmitCycle,
            std::span<const uint32_t>(offsets, blocks));
    }
}

bool DiceTxStreamEngine::NextPacketWouldCarryData() const noexcept {
    return packetizer_.NextPacketWouldCarryData();
}

void DiceTxStreamEngine::WriteHostOutputFloat32(
    const AMDTP::HostAudioBufferView& hostBuffer,
    uint64_t completionCursor) noexcept {
    if (isMotu_) {
        motuPayloadWriter_.WriteFloat32Interleaved(hostBuffer, completionCursor);
        return;
    }
    payloadWriter_.WriteFloat32Interleaved(hostBuffer, completionCursor);
}

void DiceTxStreamEngine::BindMotuOffsetCache(
    ::ASFW::Encoding::Motu::MotuEventOffsetCache* cache) noexcept {
    motuOffsetCache_ = cache;
}

AMDTP::AmdtpPacketTimeline& DiceTxStreamEngine::Timeline() noexcept {
    return timeline_;
}

const AMDTP::AmdtpPacketTimeline& DiceTxStreamEngine::Timeline() const noexcept {
    return timeline_;
}

const AMDTP::AmdtpStreamConfig& DiceTxStreamEngine::StreamConfig() const noexcept {
    return packetizer_.StreamConfig();
}

AMDTP::AmdtpTxPacketizerTelemetrySnapshot
DiceTxStreamEngine::PacketizerTelemetrySnapshot() const noexcept {
    return packetizer_.TelemetrySnapshot();
}

const DiceTxEngineCounters& DiceTxStreamEngine::Counters() const noexcept {
    return counters_;
}

const AMDTP::AmdtpPayloadWriterCounters&
DiceTxStreamEngine::PayloadWriterCounters() const noexcept {
    return payloadWriter_.Counters();
}

AMDTP::AmdtpTxPolicy DiceTxStreamEngine::BuildTxPolicy(
    const ASFW::Isoch::Audio::AudioStreamTxPolicy& streamPolicy) const noexcept {
    AMDTP::AmdtpTxPolicy policy{};
    policy.hostToDevicePcmEncoding = (streamPolicy.hostToDevicePcmEncoding == ASFW::Encoding::AudioWireFormat::kRawPcm24In32)
                                     ? AMDTP::PcmSlotEncoding::RawSigned24In32BE
                                     : AMDTP::PcmSlotEncoding::Am824MBLA;
    policy.dbsPolicy = streamPolicy.variableDbs
                       ? AMDTP::DbsPolicy::VariablePerPacket
                       : AMDTP::DbsPolicy::Constant;
    policy.defaultNonAudioSlotWord = streamPolicy.defaultNonAudioSlotWord;
    policy.initializeNonAudioSlots = streamPolicy.initializeNonAudioSlots;
    policy.preserveFdfInNoDataPackets = streamPolicy.preserveFdfInNoDataPackets;
    policy.emptyPacketsDuringIdle = streamPolicy.emptyPacketsDuringIdle;
    policy.clearPayloadBeforeExposure = true;
    return policy;
}

} // namespace ASFW::Protocols::Audio::DICE
