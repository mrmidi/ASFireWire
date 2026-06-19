#include "DiceTxStreamEngine.hpp"

namespace ASFW::Protocols::Audio::DICE {

AMDTP::AmdtpStreamConfig DiceStreamConfigMapper::ToAmdtpConfig(
    const ASFW::Isoch::Audio::DICE::DiceStreamConfig& diceConfig) noexcept {
    AMDTP::AmdtpStreamConfig config{};
    config.sampleRate = diceConfig.sampleRate;
    config.streamMode = (diceConfig.streamMode == ASFW::Encoding::StreamMode::kBlocking) 
                        ? AMDTP::StreamMode::Blocking 
                        : AMDTP::StreamMode::NonBlocking;
    config.sid = diceConfig.sid;
    config.dbs = diceConfig.dbs;
    config.pcmChannels = diceConfig.pcmChannels;
    config.midiSlots = diceConfig.midiSlots;
    config.fmt = diceConfig.fmt;
    config.fdf = diceConfig.fdf;
    config.framesPerDataPacket = diceConfig.framesPerDataPacket;
    // maxPacketBytes keeps the AmdtpStreamConfig default (512), matching the
    // lab slot capacity and the ASFW IT ring payload budget.
    return config;
}

bool DiceTxStreamEngine::Configure(const ASFW::Isoch::Audio::DICE::IDiceDeviceProfile& profile,
                                   const ASFW::Isoch::Audio::DICE::DiceStreamConfig& txConfig) noexcept {
    if (txConfig.direction != ASFW::Isoch::Audio::DICE::DiceStreamDirection::HostToDevice) {
        return false;
    }

    const ASFW::Isoch::Audio::DICE::DiceDeviceQuirks quirks = profile.Quirks();
    const AMDTP::AmdtpStreamConfig amdtpConfig =
        DiceStreamConfigMapper::ToAmdtpConfig(txConfig);
    const AMDTP::AmdtpTxPolicy policy = BuildTxPolicy(quirks);

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

    profile_ = &profile;
    diceConfig_ = txConfig;
    quirks_ = quirks;
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

bool DiceTxStreamEngine::NextPacketWouldCarryData() const noexcept {
    return packetizer_.NextPacketWouldCarryData();
}

void DiceTxStreamEngine::WriteHostOutputFloat32(
    const AMDTP::HostAudioBufferView& hostBuffer,
    uint64_t completionCursor) noexcept {
    payloadWriter_.WriteFloat32Interleaved(hostBuffer, completionCursor);
}

AMDTP::AmdtpPacketTimeline& DiceTxStreamEngine::Timeline() noexcept {
    return timeline_;
}

const AMDTP::AmdtpPacketTimeline& DiceTxStreamEngine::Timeline() const noexcept {
    return timeline_;
}

const DiceTxEngineCounters& DiceTxStreamEngine::Counters() const noexcept {
    return counters_;
}

const AMDTP::AmdtpPayloadWriterCounters&
DiceTxStreamEngine::PayloadWriterCounters() const noexcept {
    return payloadWriter_.Counters();
}

AMDTP::AmdtpTxPolicy DiceTxStreamEngine::BuildTxPolicy(
    const ASFW::Isoch::Audio::DICE::DiceDeviceQuirks& quirks) const noexcept {
    AMDTP::AmdtpTxPolicy policy{};
    policy.hostToDevicePcmEncoding = (quirks.tx.hostToDevicePcmEncoding == ASFW::Encoding::AudioWireFormat::kRawPcm24In32)
                                     ? AMDTP::PcmSlotEncoding::RawSigned24In32BE
                                     : AMDTP::PcmSlotEncoding::Am824MBLA;
    policy.dbsPolicy = (quirks.tx.dbsPolicy == ASFW::Isoch::Audio::DICE::DbsPolicy::VariablePerPacket)
                       ? AMDTP::DbsPolicy::VariablePerPacket
                       : AMDTP::DbsPolicy::Constant;
    policy.defaultNonAudioSlotWord = quirks.tx.defaultNonAudioSlotWord;
    policy.initializeNonAudioSlots = quirks.tx.initializeNonAudioSlots;
    policy.preserveFdfInNoDataPackets =
        quirks.tx.preserveFdfInNoDataPackets;
    policy.clearPayloadBeforeExposure = true;
    return policy;
}

} // namespace ASFW::Protocols::Audio::DICE
