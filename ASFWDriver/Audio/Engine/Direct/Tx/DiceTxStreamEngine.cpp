#include "DiceTxStreamEngine.hpp"
#include "../../../../Isoch/Core/IsochTxQueue.hpp"

#ifdef ASFW_HOST_TEST
#include <mach/mach_time.h>
#else
#include <DriverKit/IOLib.h> // mach_time
#endif

#include <algorithm>

#include <iterator>
#include "../../../../Midi/Trace/MidiMessageTrace.hpp"
#include "../../../../Midi/Trace/MidiTraceLog.hpp"

namespace ASFW::Protocols::Audio::DICE {

AMDTP::AmdtpStreamConfig DiceStreamConfigMapper::ToAmdtpConfig(
    const ASFW::Isoch::Audio::AudioStreamConfig& streamConfig) noexcept {
    AMDTP::AmdtpStreamConfig config{};
    config.sampleRate = streamConfig.sampleRate;
    config.streamMode =
        streamConfig.streamMode == ASFW::Encoding::StreamMode::kBlocking
            ? AMDTP::StreamMode::Blocking : AMDTP::StreamMode::NonBlocking;
    config.sid = streamConfig.sid;
    config.dbs = streamConfig.dbs;
    config.pcmChannels = streamConfig.pcmChannels;
    config.midiSlots = streamConfig.midiSlots;
    config.fmt = streamConfig.fmt;
    config.fdf = streamConfig.fdf;
    config.framesPerDataPacket = streamConfig.framesPerDataPacket;
    config.sourceChannelOffset = streamConfig.sourceChannelOffset;
    config.maxPacketBytes = 8u +
        static_cast<uint32_t>(streamConfig.framesPerDataPacket) *
            streamConfig.dbs * 4u;
    return config;
}

bool DiceTxStreamEngine::Configure(
    const ASFW::Isoch::Audio::IAudioStreamProfile& profile,
    const ASFW::Isoch::Audio::AudioStreamConfig& txConfig) noexcept {
    if (txConfig.direction !=
        ASFW::Isoch::Audio::AudioStreamDirection::HostToDevice) {
        return false;
    }
    const auto geometry =
        ASFW::Encoding::AmdtpRateGeometryForSampleRate(txConfig.sampleRate);
    if (!geometry || (txConfig.sampleRate != 44'100 &&
                      txConfig.sampleRate != 48'000 &&
                      txConfig.sampleRate != 96'000 &&
                      txConfig.sampleRate != 192'000)) {
        return false;
    }
    const auto policy = profile.TxStreamPolicy();
    const auto config = DiceStreamConfigMapper::ToAmdtpConfig(txConfig);
    if (!timeline_.AttachSlots(
            timelineSlots_,
            static_cast<uint32_t>(std::size(timelineSlots_)))) {
        return false;
    }
    packetizer_.BindTimeline(&timeline_);
    if (!packetizer_.Configure(config, BuildTxPolicy(policy))) return false;

    if (config.streamMode == AMDTP::StreamMode::Blocking) {
        if (!blockingCadence_.Configure(
                config.sampleRate,
                static_cast<uint8_t>(geometry->sytIntervalFrames))) {
            return false;
        }
        cadence_ = &blockingCadence_;
    } else {
        if (config.sampleRate != 48'000) return false;
        cadence_ = &nonBlockingCadence_;
    }
    streamConfig_ = txConfig;
    txPolicy_ = policy;
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

void DiceTxStreamEngine::ResetForStart(uint8_t initialDbc) noexcept {
    timeline_.Reset();
    packetizer_.Reset(initialDbc);
    if (cadence_) cadence_->Reset();
    // Packet indices restart at zero, so retention from the previous run would
    // match by index while describing a different epoch's frames. Clear it: a
    // fill must never be attempted against an arm this run did not make.
    for (auto& armed : armedPackets_) armed = {};
    for (auto& filled : armedFilled_) filled = false;
    for (auto& filled : pcmFilled_) filled = false;
    // Same boundary as the queue's own consumer reset, so the producer and
    // transport counters the [TxFill] line subtracts share one run. See
    // DiceTxEngineCounters::Reset.
    counters_.Reset();
}

bool DiceTxStreamEngine::PreviewPresentationPlan(
    uint64_t epoch,
    uint64_t cycleOrdinal,
    uint64_t firstAudioFrame,
    uint64_t presentationBusTicks,
    const AMDTP::AmdtpTimingState& timing,
    AMDTP::TxPresentationPlan& outPlan,
    uint8_t& outWireDataBlocks,
    uint16_t& outSyt) const noexcept {
    if (!cadence_ || epoch == 0) return false;
    const bool hasExternalSchedule =
        timing.hasExplicitPacketSchedule || timing.replayValid;
    const uint16_t externalBlocks = timing.hasExplicitPacketSchedule
        ? timing.explicitDataBlocks : timing.replayDataBlocks;
    const bool cadenceData = cadence_->CurrentCycleIsData();
    const uint16_t selectedBlocks = hasExternalSchedule
        ? externalBlocks : cadence_->CurrentCycleDataFrames();
    const bool data =
        timing.disposition == AMDTP::AmdtpPacketDisposition::Data &&
        (hasExternalSchedule ? selectedBlocks != 0 : cadenceData);
    if (selectedBlocks > packetizer_.StreamConfig().framesPerDataPacket) {
        return false;
    }
    outWireDataBlocks = data ? static_cast<uint8_t>(selectedBlocks) : 0;
    outSyt = data && timing.txClockValid ? timing.nextDataSyt : 0xFFFF;
    outPlan = {
        .epoch = epoch,
        .cycleOrdinal = cycleOrdinal,
        .firstAudioFrame = firstAudioFrame,
        .frameCount = data ? static_cast<uint32_t>(outWireDataBlocks) : 0U,
        .presentationBusTicks = presentationBusTicks,
        .disposition = data ? AMDTP::AmdtpPacketDisposition::Data
                            : AMDTP::AmdtpPacketDisposition::NoData,
    };
    return true;
}

TxSlotPrepareResult DiceTxStreamEngine::PrepareTransmitSlot(
    uint32_t packetIndex,
    const AMDTP::TxPresentationPlan& plan,
    uint8_t wireDataBlocks,
    uint16_t syt) noexcept {
    if (!slotProvider_) return TxSlotPrepareResult::SlotProviderUnavailable;

    // Arming never consults the content source. A planned DATA packet is
    // encoded from silence so transport always has a complete, valid image for
    // the slot; real content arrives later through FillTransmitSlot, if it
    // arrives in time at all. This is what lets the arm lead stay deep -- deep
    // enough to absorb a producer stall -- while the content lead stays short,
    // which is the only part of the two that CoreAudio pays for as latency.
    AMDTP::TxPcmSnapshotView pcm{};
    if (plan.disposition == AMDTP::AmdtpPacketDisposition::Data) {
        const uint64_t sampleCount = static_cast<uint64_t>(plan.frameCount) *
            packetizer_.StreamConfig().pcmChannels;
        if (sampleCount > silenceScratch_.size()) {
            counters_.pcmCopiesInvalid.fetch_add(1, std::memory_order_relaxed);
            return TxSlotPrepareResult::PcmInvalidRequest;
        }
        pcm = {
            .interleavedFloat32 = silenceScratch_.data(),
            .frameCount = plan.frameCount,
            .channels = packetizer_.StreamConfig().pcmChannels,
        };
    }

    AMDTP::TxPacketSlotView slot{};
    if (!slotProvider_->AcquireWritableSlot(packetIndex, slot)) {
        counters_.slotAcquireFailures.fetch_add(1, std::memory_order_relaxed);
        return TxSlotPrepareResult::SlotAcquireFailed;
    }
    AMDTP::PreparedTxPacket packet{};
    if (!packetizer_.PrepareDataPacket(
            slot, plan, wireDataBlocks, syt, pcm, packet)) {
        return TxSlotPrepareResult::PacketizerRejected;
    }
    if (!slotProvider_->PublishSlot(packet)) {
        return TxSlotPrepareResult::SlotPublishFailed;
    }
    if (!packetizer_.CommitPreparedPacket(packet, wireDataBlocks)) {
        return TxSlotPrepareResult::CommitRejected;
    }

    const uint32_t retention = std::min<uint32_t>(
        slotProvider_->SlotCount(),
        ASFW::Audio::Shared::AudioTimingGeometry::kTimelineSlots);
    if (retention != 0) {
        const uint32_t index = packetIndex % retention;
        armedPackets_[index] = packet;
        armedFilled_[index] = false;
        pcmFilled_[index] = false;
    }

    if (packet.isData && slotProvider_->SlotCount() != 0) {
        const uint64_t expectedGen = ASFW::Isoch::ExpectedTxCommitGeneration(
            packet.packetIndex, slotProvider_->SlotCount());
        timeline_.SetImageProvenance(
            packet.packetIndex,
            0,
            expectedGen,
            packet.firstAudioFrame,
            packet.framesInPacket,
            static_cast<uint8_t>(ASFW::Audio::Ports::PcmCopyResult::NotYetPublished));
    }

    cadence_->AdvanceCycle();
    counters_.packetsPrepared.fetch_add(1, std::memory_order_relaxed);
    if (packet.isData) {
        counters_.dataPacketsPrepared.fetch_add(1, std::memory_order_relaxed);
    } else {
        counters_.noDataPacketsPrepared.fetch_add(1, std::memory_order_relaxed);
    }
    return TxSlotPrepareResult::Prepared;
}

void DiceTxStreamEngine::NoteFrozenWithoutContent(
    uint32_t packetIndex) noexcept {
    if (!slotProvider_) return;
    const uint32_t retention = std::min<uint32_t>(
        slotProvider_->SlotCount(),
        ASFW::Audio::Shared::AudioTimingGeometry::kTimelineSlots);
    if (retention == 0) return;
    const uint32_t index = packetIndex % retention;
    // Only a DATA packet this engine armed and never filled. A cadence NO-DATA
    // packet carries no samples, and a filled one carries content.
    if (armedFilled_[index] || !armedPackets_[index].isData ||
        armedPackets_[index].packetIndex != packetIndex) {
        return;
    }
    armedFilled_[index] = true;  // do not count the same packet twice
    counters_.pcmSilenceSubstitutions.fetch_add(1, std::memory_order_relaxed);
}

uint64_t DiceTxStreamEngine::FreezeFrontier() const noexcept {
    return slotProvider_ ? slotProvider_->FinalizedEnd() : 0;
}

TxSlotFillResult DiceTxStreamEngine::FillTransmitSlot(
    uint32_t packetIndex) noexcept {
    if (!slotProvider_ || (!pcmSource_ && midiBlock_ == nullptr)) {
        return TxSlotFillResult::NotFillable;
    }
    const uint32_t retention = std::min<uint32_t>(
        slotProvider_->SlotCount(),
        ASFW::Audio::Shared::AudioTimingGeometry::kTimelineSlots);
    if (retention == 0) return TxSlotFillResult::NotFillable;
    const uint32_t index = packetIndex % retention;
    const AMDTP::PreparedTxPacket& armed = armedPackets_[index];

    // Only a DATA packet this engine actually armed, and only once. A cadence
    // NO-DATA packet carries no samples and must keep the geometry it was
    // planned with.
    if (armedFilled_[index] || !armed.isData ||
        armed.packetIndex != packetIndex || armed.framesInPacket == 0) {
        return TxSlotFillResult::NotFillable;
    }

    const uint64_t sampleCount = static_cast<uint64_t>(armed.framesInPacket) *
        packetizer_.StreamConfig().pcmChannels;
    if (sampleCount > pcmScratch_.size()) {
        counters_.pcmCopiesInvalid.fetch_add(1, std::memory_order_relaxed);
        return TxSlotFillResult::Rejected;
    }

    using CopyResult = ASFW::Audio::Ports::PcmCopyResult;
    CopyResult result = CopyResult::Ready;
    if (pcmSource_ != nullptr) {
        result = pcmSource_->CopyExact(
            {
                .epoch = armed.epoch,
                .firstFrame = armed.firstAudioFrame,
                .frameCount = armed.framesInPacket,
                .sourceChannelOffset = packetizer_.StreamConfig().sourceChannelOffset,
                .channelCount = packetizer_.StreamConfig().pcmChannels,
            },
            pcmScratch_.data(), static_cast<uint32_t>(pcmScratch_.size()));
    } else {
        std::memset(pcmScratch_.data(), 0, sampleCount * sizeof(float));
    }

    if (slotProvider_->SlotCount() != 0) {
        const uint64_t expectedGen = ASFW::Isoch::ExpectedTxCommitGeneration(
            packetIndex, slotProvider_->SlotCount());
        timeline_.SetImageProvenance(
            packetIndex,
            1,
            expectedGen,
            armed.firstAudioFrame,
            armed.framesInPacket,
            static_cast<uint8_t>(result));
    }
    switch (result) {
        case CopyResult::Ready:
            counters_.pcmCopiesReady.fetch_add(1, std::memory_order_relaxed);
            break;
        case CopyResult::NotYetPublished:
            counters_.pcmCopiesNotYetPublished.fetch_add(
                1, std::memory_order_relaxed);
            counters_.lateFillsUnavailable.fetch_add(
                1, std::memory_order_relaxed);
            return TxSlotFillResult::ContentUnavailable;
        case CopyResult::Expired:
            counters_.pcmCopiesExpired.fetch_add(1, std::memory_order_relaxed);
            counters_.lateFillsUnavailable.fetch_add(
                1, std::memory_order_relaxed);
            return TxSlotFillResult::ContentUnavailable;
        case CopyResult::WrongEpoch:
            counters_.pcmCopiesWrongEpoch.fetch_add(
                1, std::memory_order_relaxed);
            counters_.lateFillsUnavailable.fetch_add(
                1, std::memory_order_relaxed);
            return TxSlotFillResult::ContentUnavailable;
        case CopyResult::ConcurrentRewrite:
            counters_.pcmCopiesConcurrentRewrite.fetch_add(
                1, std::memory_order_relaxed);
            counters_.lateFillsUnavailable.fetch_add(
                1, std::memory_order_relaxed);
            return TxSlotFillResult::ContentUnavailable;
        case CopyResult::InvalidRequest:
            counters_.pcmCopiesInvalid.fetch_add(1, std::memory_order_relaxed);
            return TxSlotFillResult::Rejected;
    }

    AMDTP::TxPacketSlotView slot{};
    if (!slotProvider_->AcquireLatePayloadSlot(packetIndex, slot)) {
        // Frozen. The armed silence transmits, and the range is consumed --
        // content is never retried into a later packet.
        counters_.lateFillsTooLate.fetch_add(1, std::memory_order_relaxed);
        return TxSlotFillResult::TooLate;
    }
    const AMDTP::TxPcmSnapshotView pcm{
        .interleavedFloat32 = pcmScratch_.data(),
        .frameCount = armed.framesInPacket,
        .channels = packetizer_.StreamConfig().pcmChannels,
    };
    if (!packetizer_.RefillPcm(slot, armed, pcm)) {
        return TxSlotFillResult::Rejected;
    }
    pcmFilled_[index] = true;
    // Gated on the capture, not merely stored conditionally: this runs once per
    // packet on the encode path, so an unconditional clock read would tax every
    // stream whether or not a session is measuring one.
    if (slotProvider_->CaptureActive()) {
        slotProvider_->RecordEncodingCompleted(packetIndex,
                                               mach_absolute_time());
    }

    // MIDI is NOT composed here. This runs at the audio fill cursor, which with
    // a bound PCM source chases committedEnd across the whole committed DMA
    // runway -- headroom audio wants and MIDI does not. Composing here would
    // charge every byte that entire lead. CommitFill composes it instead, in
    // the same breath as publishing the late payload near finalizedEnd.

    return TxSlotFillResult::Filled;
}

bool DiceTxStreamEngine::IsPcmFilled(uint32_t packetIndex) const noexcept {
    if (!slotProvider_) return false;
    const uint32_t retention = std::min<uint32_t>(
        slotProvider_->SlotCount(),
        ASFW::Audio::Shared::AudioTimingGeometry::kTimelineSlots);
    if (retention == 0) return false;
    const uint32_t index = packetIndex % retention;
    return pcmFilled_[index] && armedPackets_[index].packetIndex == packetIndex && !armedFilled_[index];
}

void DiceTxStreamEngine::SetMidiTransport(
    ASFW::Midi::MidiTransportBlock* block, uint64_t streamEpoch,
    const ASFW::Encoding::MpxMidiGeometry& geometry, uint32_t sampleRateHz,
    uint32_t sytIntervalFrames) noexcept {
    midiBlock_ = block;
    midiEpoch_ = streamEpoch;
    midiGeometry_ = geometry;
    // Reset first: a reservation left over from the previous stream must not be
    // retired by a packet of the new one that happens to share its index.
    midiScope_.Reset();
    for (auto& parser : midiTxTrace_) {
        // A new stream must not have the previous one's trailing bytes joined
        // onto its first message.
        parser.Reset();
    }
    if (block != nullptr) {
        midiScope_.ConfigureLimiter(sampleRateHz, sytIntervalFrames);
    }
}

bool DiceTxStreamEngine::CommitFill(uint32_t packetIndex) noexcept {
    if (!slotProvider_) return false;
    const uint32_t retention = std::min<uint32_t>(
        slotProvider_->SlotCount(),
        ASFW::Audio::Shared::AudioTimingGeometry::kTimelineSlots);
    if (retention == 0) return false;
    const uint32_t index = packetIndex % retention;
    if (armedFilled_[index] || !pcmFilled_[index] ||
        armedPackets_[index].packetIndex != packetIndex) {
        return false;
    }
    const AMDTP::PreparedTxPacket& armed = armedPackets_[index];
    if (!armed.isData) return false;

    // Compose MIDI before publishing the late payload, so transport seals the
    // complete, final image (PCM + MIDI). Modifying the payload after publication
    // corrupts the transport checksum seal.
    bool midiSelected = false;
    if (midiBlock_ != nullptr && midiGeometry_.Valid()) {
        AMDTP::TxPacketSlotView slot{};
        if (!slotProvider_->AcquireLatePayloadSlot(packetIndex, slot)) {
            return false;
        }

        if (midiScope_.HasOutstanding()) {
            midiScope_.Cancel(midiReservation_);
        }
        if (midiScope_.Begin(*midiBlock_, midiEpoch_, packetIndex,
                              midiReservation_)) {
            midiSelected = true;
            if (midiReservation_.Any()) {
                ASFW::Encoding::MpxMidiPacketBytes bytes{};
                for (uint32_t port = 0; port < ASFW::Encoding::kMpxMidiPorts; ++port) {
                    uint8_t byte = 0;
                    if (midiReservation_.Get(static_cast<uint8_t>(port), byte)) {
                        bytes.byteForPort[port] = byte;
                        bytes.hasByteForPort[port] = true;
                    }
                }
                packetizer_.ComposeMidi(slot, armed, midiGeometry_, bytes);
                TraceMidiTxReservation(packetIndex, armed.dbc);
            }
        }
    }

    if (!slotProvider_->PublishLatePayload(packetIndex)) {
        if (midiSelected) {
            midiScope_.Cancel(midiReservation_);
        }
        return false;
    }

    if (midiSelected) {
        (void)midiScope_.Commit(*midiBlock_, midiReservation_);
    }

    armedFilled_[index] = true;
    counters_.lateFillsPublished.fetch_add(1, std::memory_order_relaxed);
    // Retire on the successful offer, not at encode or arm time. This is a
    // software retirement point under an at-most-once policy: transport can
    // still discard an accepted offer, which is counted as loss rather than
    // replayed, because replaying would put a byte behind bytes already sent.
    return true;
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
const DiceTxEngineCounters& DiceTxStreamEngine::Counters() const noexcept {
    return counters_;
}

AMDTP::AmdtpTxPolicy DiceTxStreamEngine::BuildTxPolicy(
    const ASFW::Isoch::Audio::AudioStreamTxPolicy& streamPolicy) const noexcept {
    AMDTP::AmdtpTxPolicy policy{};
    policy.hostToDevicePcmEncoding =
        streamPolicy.hostToDevicePcmEncoding ==
                ASFW::Encoding::AudioWireFormat::kRawPcm24In32
            ? AMDTP::PcmSlotEncoding::RawSigned24In32BE
            : AMDTP::PcmSlotEncoding::Am824MBLA;
    policy.dbsPolicy = streamPolicy.variableDbs
        ? AMDTP::DbsPolicy::VariablePerPacket : AMDTP::DbsPolicy::Constant;
    policy.defaultNonAudioSlotWord = streamPolicy.defaultNonAudioSlotWord;
    policy.initializeNonAudioSlots = streamPolicy.initializeNonAudioSlots;
    policy.preserveFdfInNoDataPackets = streamPolicy.preserveFdfInNoDataPackets;
    policy.emptyPacketsDuringIdle = streamPolicy.emptyPacketsDuringIdle;
    policy.cadencePacketsCarryDataBlocks =
        streamPolicy.cadencePacketsCarryDataBlocks;
    policy.substituteSilenceOnPcmUnavailable =
        streamPolicy.substituteSilenceOnPcmUnavailable;
    policy.playbackChannelMap = streamPolicy.playbackChannelMap;
    return policy;
}


void DiceTxStreamEngine::TraceMidiTxReservation(uint32_t packetIndex,
                                               uint8_t dbc) noexcept {
    for (uint32_t port = 0; port < ASFW::Encoding::kMpxMidiPorts; ++port) {
        uint8_t byte = 0;
        if (midiReservation_.Get(static_cast<uint8_t>(port), byte)) {
            TraceMidiTxByte(static_cast<uint8_t>(port), byte, packetIndex, dbc);
        }
    }
}

void DiceTxStreamEngine::TraceMidiTxByte(uint8_t port, uint8_t byte,
                                         uint32_t packetIndex,
                                         uint8_t dbc) noexcept {
    if (port >= ASFW::Encoding::kMpxMidiPorts) return;
    // The block this byte will land in is decided by the same rotation the mux
    // applies, so derive it rather than guessing: block f carries port
    // (dbc + f) mod 8 when the rotation is DBC-aligned.
    const uint8_t block = midiGeometry_.dbcAligned
        ? static_cast<uint8_t>((port - dbc) & (ASFW::Encoding::kMpxMidiPorts - 1))
        : port;
    // Packets this byte will sit in the ring before the hardware transmits it.
    // At 125 us per packet this is the dominant term in MIDI latency and it is
    // entirely ours: it is the distance from the late commit pass to the wire.
    //
    // An upper bound, not an exact figure. completionCursor is only written by
    // the refill pass on the IT completion interrupt, one completion group
    // apart, so between interrupts it understates the true hardware position by
    // 0..8 packets. Read the printed lead as (true lead + up to 1 ms).
    const uint64_t completion =
        slotProvider_ ? slotProvider_->CompletionCursor() : 0;
    const uint32_t leadPackets = (completion != 0 && packetIndex > completion)
        ? static_cast<uint32_t>(packetIndex - completion)
        : 0;
    ASFW::Midi::Trace::DecodedMidiMessage message{};
    if (midiTxTrace_[port].Feed(byte, message)) {
        ASFW::Midi::Trace::LogDecodedMidi(
            ASFW::Midi::Trace::MidiTraceDirection::kOut, port, message,
            "pkt", packetIndex, block, mach_absolute_time(), leadPackets);
    }
    ASFW::Midi::Trace::DecodedMidiMessage deferred{};
    if (midiTxTrace_[port].HasDeferred() &&
        midiTxTrace_[port].TakeDeferred(deferred)) {
        ASFW::Midi::Trace::LogDecodedMidi(
            ASFW::Midi::Trace::MidiTraceDirection::kOut, port, deferred,
            "pkt", packetIndex, block, mach_absolute_time(), leadPackets);
    }
}

} // namespace ASFW::Protocols::Audio::DICE
