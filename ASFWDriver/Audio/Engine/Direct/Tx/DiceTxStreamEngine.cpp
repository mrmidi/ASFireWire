#include "DiceTxStreamEngine.hpp"

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
    profile_ = &profile;
    streamConfig_ = txConfig;
    txPolicy_ = txPolicy;
    return true;
}

void DiceTxStreamEngine::BindPcmSource(
    ASFW::Audio::Ports::ITxPcmSource* pcmSource) noexcept {
    pcmSource_ = pcmSource;
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

    AMDTP::AmdtpNextPacketPlan plan{};
    if (!packetizer_.PreviewNextPacket(timing, plan)) {
        return TxSlotPrepareResult::kPacketizerRejected;
    }

    AMDTP::TxPcmSnapshotView pcm{};
    if (plan.isData) {
        if (!pcmSource_) {
            return TxSlotPrepareResult::kPcmSourceUnavailable;
        }
        const uint64_t sampleCount =
            static_cast<uint64_t>(plan.framesInPacket) *
            packetizer_.StreamConfig().pcmChannels;
        if (sampleCount > pcmScratch_.size()) {
            counters_.pcmReadsInvalid.fetch_add(
                1, std::memory_order_relaxed);
            return TxSlotPrepareResult::kPcmInvalidRequest;
        }
        const auto sourceResult = pcmSource_->ReadFloat32Interleaved(
            {
                .firstFrame = plan.firstAudioFrame,
                .frameCount = plan.framesInPacket,
                .sourceChannelOffset =
                    packetizer_.StreamConfig().sourceChannelOffset,
                .channelCount = packetizer_.StreamConfig().pcmChannels,
            },
            pcmScratch_.data(),
            static_cast<uint32_t>(pcmScratch_.size()));
        switch (sourceResult) {
            case ASFW::Audio::Ports::TxPcmReadResult::kReady:
                counters_.pcmReadsReady.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case ASFW::Audio::Ports::TxPcmReadResult::kNotYetWritten:
                counters_.pcmReadsNotYetWritten.fetch_add(
                    1, std::memory_order_relaxed);
                return TxSlotPrepareResult::kPcmNotYetWritten;
            case ASFW::Audio::Ports::TxPcmReadResult::kStaleOverwritten:
                counters_.pcmReadsStaleOverwritten.fetch_add(
                    1, std::memory_order_relaxed);
                return TxSlotPrepareResult::kPcmStaleOverwritten;
            case ASFW::Audio::Ports::TxPcmReadResult::kSnapshotBusy:
                counters_.pcmReadsSnapshotBusy.fetch_add(
                    1, std::memory_order_relaxed);
                return TxSlotPrepareResult::kPcmSnapshotBusy;
            case ASFW::Audio::Ports::TxPcmReadResult::kInvalidRequest:
                counters_.pcmReadsInvalid.fetch_add(
                    1, std::memory_order_relaxed);
                return TxSlotPrepareResult::kPcmInvalidRequest;
        }
        pcm = {
            .interleavedFloat32 = pcmScratch_.data(),
            .frameCount = plan.framesInPacket,
            .channels = packetizer_.StreamConfig().pcmChannels,
        };
    }

    AMDTP::TxPacketSlotView slot{};
    if (!slotProvider_->AcquireWritableSlot(packetIndex, slot)) {
        counters_.slotAcquireFailures.fetch_add(1, std::memory_order_relaxed);
        return TxSlotPrepareResult::kSlotAcquireFailed;
    }

    AMDTP::PreparedTxPacket packet{};
    if (!packetizer_.PrepareNextPacket(slot, timing, pcm, packet)) {
        return TxSlotPrepareResult::kPacketizerRejected;
    }

    if (!slotProvider_->PublishSlot(packet)) {
        return TxSlotPrepareResult::kSlotPublishFailed;
    }
    timeline_.MarkPublished(packet.packetIndex);

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
    policy.noDataDbcPolicy = streamPolicy.advanceDbcOnNoData
        ? AMDTP::NoDataDbcPolicy::AdvanceBySytInterval
        : AMDTP::NoDataDbcPolicy::Hold;
    return policy;
}

} // namespace ASFW::Protocols::Audio::DICE
