// IsochAudioTxPipeline.cpp
// ASFW - Audio semantics layer for IT transmit (Direct-Only implementation).

#include "IsochAudioTxPipeline.hpp"

#include "../../AudioWire/AMDTP/TimingUtils.hpp"
#include "../Direct/Tx/DirectTxPacketEncoder.hpp"

#include <AudioDriverKit/AudioDriverKit.h>
#include <limits>

namespace ASFW::Isoch {

namespace Direct = ASFW::AudioEngine::Direct;
using DirectTxReadStatus = ASFW::AudioEngine::Direct::Tx::DirectTxReadStatus;

namespace {

inline uint64_t ExternalSyncStaleThresholdTicks(const bool allowStartupQualifiedOnly) noexcept {
    const uint64_t staleThresholdNanos = allowStartupQualifiedOnly
        ? ASFW::AudioEngine::DirectIsoch::kExternalSyncStartupSeedGraceNanos
        : ASFW::AudioEngine::DirectIsoch::kExternalSyncLiveStaleNanos;
    uint64_t staleThresholdTicks = ASFW::Timing::nanosToHostTicks(staleThresholdNanos);
    if (staleThresholdTicks == 0 && ASFW::Timing::initializeHostTimebase()) {
        staleThresholdTicks = ASFW::Timing::nanosToHostTicks(staleThresholdNanos);
    }
    return staleThresholdTicks;
}

[[maybe_unused]] [[nodiscard]] constexpr bool ShouldLogTxHotPathSample(uint64_t count) noexcept {
    return count <= 16 || (count % 8000) == 0;
}

} // namespace

void IsochAudioTxPipeline::SetExternalSyncBridge(ASFW::AudioEngine::DirectIsoch::ExternalSyncBridge* bridge) noexcept {
    externalSyncBridge_ = bridge;
    txPhaseLoop_.Reset();
    txPhaseReadIndexSeeded_ = false;
}

void IsochAudioTxPipeline::SetDirectTxRuntimeBinding(const DirectTxRuntimeBinding& binding) noexcept {
    directTxBinding_ = binding;
    directOutputFrameCursor_ = 0;
    directCursorInitialized_ = false;

    // Rebuild the isoch-owned read view over the shared output memory + control block.
    directOutputView_ = {};
    directOutputView_.sampleRateHz = binding.sampleRateHz;
    directOutputView_.memory.outputBase = binding.outputBase;
    directOutputView_.memory.outputFrameCapacity = binding.outputFrames;
    directOutputView_.memory.outputChannels = binding.outputChannels;
    directOutputView_.memory.storage = ASFW::Audio::Runtime::AudioSampleStorage::kInt32Native;
    directOutputView_.hostToDeviceAm824Slots = binding.am824Slots;
    directOutputView_.control = binding.control;
    directOutputReader_.Bind(&directOutputView_);
    if (binding.enabled && binding.control != nullptr) {
        txClockPublisher_.Bind(&directOutputView_);
    } else {
        txClockPublisher_.Unbind();
    }

    ASFW_LOG(Isoch,
             "IT: DIRECT-TX binding %{public}s base=%p frames=%u ch=%u slots=%u rate=%u mode=%u bound=%{public}s",
             binding.enabled ? "set(enabled)" : "set(disabled)",
             static_cast<const void*>(binding.outputBase),
             binding.outputFrames, binding.outputChannels, binding.am824Slots,
             binding.sampleRateHz, binding.streamModeRaw,
             directOutputReader_.IsBound() ? "yes" : "no");
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
kern_return_t IsochAudioTxPipeline::Configure(uint8_t sid,
                                              uint32_t streamModeRaw,
                                              uint32_t requestedChannels,
                                              uint32_t requestedAm824Slots,
                                              Encoding::AudioWireFormat wireFormat) noexcept {
    if (requestedChannels == 0 || requestedChannels > Config::kMaxPcmChannels) {
        ASFW_LOG(Isoch, "IT: Configure failed - invalid requestedChannels=%u", requestedChannels);
        return kIOReturnBadArgument;
    }

    uint32_t am824Slots = requestedChannels;
    if (requestedAm824Slots != 0) {
        if (requestedAm824Slots < requestedChannels) {
            ASFW_LOG(Isoch, "IT: Configure failed - am824Slots=%u < pcmChannels=%u",
                     requestedAm824Slots, requestedChannels);
            return kIOReturnBadArgument;
        }
        am824Slots = requestedAm824Slots;
    }

    requestedStreamMode_ = (streamModeRaw == std::to_underlying(ASFW::Encoding::StreamMode::kBlocking))
        ? Encoding::StreamMode::kBlocking
        : Encoding::StreamMode::kNonBlocking;
    sid_ = static_cast<uint8_t>(sid & 0x3Fu);

    // Phase 1.x: only blocking 48k is supported in the new pipeline.
    effectiveStreamMode_ = Encoding::StreamMode::kBlocking;

    assembler_.reconfigureAM824(requestedChannels, am824Slots, sid);
    assembler_.setStreamMode(effectiveStreamMode_);
    assembler_.setAudioWireFormat(wireFormat);

    sytGenerator_.reset();
    sytGenerator_.initialize(48000.0); // Hardcoded for bringup

    ASFW_LOG(Isoch, "IT: Configured direct-only pipeline: sid=%u mode=%u ch=%u slots=%u format=%u",
             sid, static_cast<uint32_t>(effectiveStreamMode_), requestedChannels, am824Slots, 
             static_cast<uint32_t>(wireFormat));

    return kIOReturnSuccess;
}

void IsochAudioTxPipeline::ResetForStart() noexcept {
    assembler_.reset();
    sytGenerator_.reset();
    dbcTracker_.Reset();
    producedPacketMetadata_ = {};
    directOutputFrameCursor_ = 0;
    directCursorInitialized_ = false;
    audioWriteIndex_ = 0;
    debugProducedPackets_ = 0;
    debugInjectionAttempts_ = 0;
    debugInjectionSuccesses_ = 0;
    debugInjectionSkips_ = 0;
    txCompletedSampleFrame_ = 0;
    txEventGroupCount_ = 0;
    txPhaseLoop_.Reset();
    txPhaseReadIndexSeeded_ = false;
}

bool IsochAudioTxPipeline::PrimeSyncFromExternalBridge() noexcept {
    const auto syncState = ReadExternalSyncState(/*allowStartupQualifiedOnly=*/true);
    ASFW_LOG(Isoch,
             "IT: TX phase loop armed (rx status=%u established=%d startupQual=%d seq=%u syt=0x%04x fdf=0x%02x dbs=%u age=%llu threshold=%llu)",
             static_cast<uint32_t>(syncState.status),
             syncState.clockEstablished,
             syncState.startupQualified,
             syncState.updateSeq,
             syncState.rxSyt,
             syncState.rxFdf,
             syncState.rxDbs,
             syncState.ageUsec,
             syncState.staleThresholdUsec);
    return true;
}

Tx::IsochTxPacket IsochAudioTxPipeline::NextTransmitPacket(const Tx::TxPacketRequest& request) noexcept {
    uint16_t syt = Encoding::SYTGenerator::kNoInfo;
    if (assembler_.nextIsData()) {
        syt = ComputeDataSyt(request.transmitCycle);
    }

    // silent=true: cadence/DBC/CIP advance, audio payload is valid silence.
    auto pkt = assembler_.assembleNext(syt, /*silent=*/true);

    // Producer-side DBC continuity validation (ignore NO-DATA).
    if (pkt.isData) {
        const uint8_t samplesInPkt = static_cast<uint8_t>(assembler_.samplesPerDataPacket());
        if (!dbcTracker_.firstPacket) {
            const uint8_t expectedDbc = static_cast<uint8_t>(dbcTracker_.lastDbc + dbcTracker_.lastDataBlockCount);
            if (pkt.dbc != expectedDbc) {
                dbcTracker_.discontinuityCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
        dbcTracker_.lastDbc = pkt.dbc;
        dbcTracker_.lastDataBlockCount = samplesInPkt;
        dbcTracker_.firstPacket = false;
    }

    if (request.packetIndex < producedPacketMetadata_.size()) {
        producedPacketMetadata_[request.packetIndex] = ProducedPacketMetadata{
            .valid = true,
            .isData = pkt.isData,
            .sizeBytes = pkt.size,
            .framesPerPacket = pkt.isData ? assembler_.samplesPerDataPacket() : 0,
            .pcmChannels = assembler_.channelCount(),
            .am824Slots = assembler_.am824SlotCount(),
            .wireFormat = assembler_.audioWireFormat(),
            .cip = PacketCipFields{
                .sid = sid_,
                .dbc = pkt.dbc,
                .syt = pkt.isData ? syt : Encoding::SYTGenerator::kNoInfo,
            },
        };
    }

    Tx::IsochTxPacket out{};
    std::memcpy(silentPacketStorage_.data(), pkt.data, pkt.size);
    out.words = reinterpret_cast<const uint32_t*>(silentPacketStorage_.data());
    out.sizeBytes = pkt.size;
    out.isData = pkt.isData;
    out.dbc = pkt.dbc;

    // Hot-path IT producer diagnostic, disabled for audio stability.
    // ++debugProducedPackets_;
    // if (ShouldLogTxHotPathSample(debugProducedPackets_)) {
    //     ASFW_LOG(Isoch,
    //              "IT DBG PRODUCE n=%llu pktIdx=%u txCycle=%u hwTs=0x%04x isData=%d size=%u dbc=0x%02x syt=0x%04x ch=%u slots=%u fmt=%u",
    //              debugProducedPackets_,
    //              request.packetIndex,
    //              request.transmitCycle,
    //              request.hwTimestamp,
    //              pkt.isData,
    //              pkt.size,
    //              pkt.dbc,
    //              pkt.isData ? syt : Encoding::SYTGenerator::kNoInfo,
    //              assembler_.channelCount(),
    //              assembler_.am824SlotCount(),
    //              static_cast<uint32_t>(assembler_.audioWireFormat()));
    // }
    return out;
}

uint16_t IsochAudioTxPipeline::ComputeDataSyt(uint32_t transmitCycle) noexcept {
    if (!cycleTrackingValid_) {
        return Encoding::SYTGenerator::kNoInfo;
    }

    const int64_t projectedOffsetTicks =
        ASFW::Timing::tstampToOffsets(0, transmitCycle % ASFW::Timing::kCyclesPerSecond, 0);
    uint16_t cadenceDelta = static_cast<uint16_t>(ASFW::Timing::kSytPacketStepTicks48k);
    if (externalSyncBridge_) {
        cadenceDelta = externalSyncBridge_->ReadCadenceDelta(txPhaseLoop_.CadenceReadIndex());
    }

    const auto result = txPhaseLoop_.EmitPacket(projectedOffsetTicks, cadenceDelta);
    return result.syt;
}

void IsochAudioTxPipeline::OnIsochEventGroup(const Core::IsochEventGroup& group) noexcept {
    if (group.direction != Core::IsochEventDirection::kTransmit ||
        group.hostTicks == 0 ||
        group.completedPacketCount == 0) {
        return;
    }

    uint64_t completedFrames = 0;
    for (uint32_t i = 0; i < group.completedPacketCount; ++i) {
        const uint32_t packetIndex =
            (group.completedPacketIndex + Tx::Layout::kNumPackets + 1u -
             group.completedPacketCount + i) % Tx::Layout::kNumPackets;
        if (packetIndex >= producedPacketMetadata_.size()) {
            continue;
        }
        const auto& metadata = producedPacketMetadata_[packetIndex];
        if (metadata.valid && metadata.isData) {
            completedFrames += metadata.framesPerPacket;
        }
    }
    txCompletedSampleFrame_ += completedFrames;

    const uint32_t sampleRateHz = directTxBinding_.sampleRateHz;
    const uint32_t hostNanosPerSampleQ8 =
        sampleRateHz == 0 ? 0 : static_cast<uint32_t>((1000000000ULL << 8) / sampleRateHz);
    if (txClockPublisher_.IsBound() && hostNanosPerSampleQ8 != 0) {
        directOutputView_.control->device.Publish(txCompletedSampleFrame_,
                                                  group.hostTicks,
                                                  hostNanosPerSampleQ8);
        directOutputView_.control->counters.CountZtsPublished();
    }

    ASFW::AudioEngine::DirectIsoch::RxCadenceSnapshot cadence{};
    if (externalSyncBridge_) {
        cadence = externalSyncBridge_->ReadCadenceSnapshot();
        if (cadence.established && !txPhaseReadIndexSeeded_) {
            txPhaseLoop_.SeedCadenceReadIndex(cadence.writeIndex);
            txPhaseReadIndexSeeded_ = true;
        }
    }

    const uint32_t completedCycle = group.outputLastTimestamp & 0x1FFFu;
    const uint32_t packetsAhead =
        (group.firstRefillPacket + Tx::Layout::kNumPackets - group.completedPacketIndex) %
        Tx::Layout::kNumPackets;
    const uint32_t projectedCycle =
        (completedCycle + packetsAhead) % ASFW::Timing::kCyclesPerSecond;
    const int64_t projectedOffsetTicks = ASFW::Timing::tstampToOffsets(0, projectedCycle, 0);
    txPhaseLoop_.BeginGroup(ASFW::AudioEngine::DirectIsoch::TxPhaseGroupUpdate{
        .projectedOffsetTicks = projectedOffsetTicks,
        .recoveredDeviceOffsetTicks = cadence.recoveredDeviceOffsetTicks,
        .recoveredValid = cadence.established,
    });

    ++txEventGroupCount_;
    if (txEventGroupCount_ <= 8 || (txEventGroupCount_ % 1024) == 0) {
        ASFW_LOG(Isoch,
                 "IT EVENT group count=%llu host=%llu hwPkt=%u completed=%u+%u refill=%u+%u hwTs=0x%04x sample=%llu cadence(est=%d warm=%u wr=%u seq=%u phaseValid=%d phase=%lld)",
                 txEventGroupCount_,
                 group.hostTicks,
                 group.hwPacketIndex,
                 group.completedPacketIndex,
                 group.completedPacketCount,
                 group.firstRefillPacket,
                 group.refillPacketCount,
                 group.outputLastTimestamp,
                 txCompletedSampleFrame_,
                 cadence.established,
                 cadence.warmupCount,
                 cadence.writeIndex,
                 cadence.seq,
                 txPhaseLoop_.PhaseValid(),
                 txPhaseLoop_.OutputPhaseTicks());
    }
}

IsochAudioTxPipeline::ExternalSyncState
IsochAudioTxPipeline::ReadExternalSyncState(const bool allowStartupQualifiedOnly) noexcept {
    ExternalSyncState state{};
    state.bridgePresent = (externalSyncBridge_ != nullptr);
    if (!externalSyncBridge_) {
        state.status = ExternalSyncState::SeedStatus::NoBridge;
        return state;
    }

    const uint32_t packed = externalSyncBridge_->lastPackedRx.load(std::memory_order_acquire);
    state.rxSyt = ASFW::AudioEngine::DirectIsoch::ExternalSyncBridge::UnpackSYT(packed);
    state.rxFdf = ASFW::AudioEngine::DirectIsoch::ExternalSyncBridge::UnpackFDF(packed);
    state.rxDbs = ASFW::AudioEngine::DirectIsoch::ExternalSyncBridge::UnpackDBS(packed);
    state.updateSeq = externalSyncBridge_->updateSeq.load(std::memory_order_acquire);
    state.active = externalSyncBridge_->active.load(std::memory_order_acquire);
    state.clockEstablished =
        externalSyncBridge_->clockEstablished.load(std::memory_order_acquire);
    state.startupQualified =
        externalSyncBridge_->startupQualified.load(std::memory_order_acquire);

    const uint64_t staleThresholdTicks = ExternalSyncStaleThresholdTicks(allowStartupQualifiedOnly);
    if (staleThresholdTicks != 0) {
        state.staleThresholdUsec =
            ASFW::Timing::hostTicksToNanos(staleThresholdTicks) / 1'000ULL;
    }

    if (!state.active) {
        state.status = ExternalSyncState::SeedStatus::Inactive;
        return state;
    }
    if (!state.clockEstablished &&
        !(allowStartupQualifiedOnly && state.startupQualified)) {
        state.status = ExternalSyncState::SeedStatus::NotEstablished;
        return state;
    }

    const uint64_t lastUpdateTicks =
        externalSyncBridge_->lastUpdateHostTicks.load(std::memory_order_acquire);
    if (staleThresholdTicks == 0 || lastUpdateTicks == 0) {
        state.status = ExternalSyncState::SeedStatus::MissingTimestamp;
        return state;
    }

    const uint64_t nowTicks = mach_absolute_time();
    if (nowTicks >= lastUpdateTicks) {
        state.ageUsec = ASFW::Timing::hostTicksToNanos(nowTicks - lastUpdateTicks) / 1'000ULL;
    }
    if (nowTicks < lastUpdateTicks ||
        (nowTicks - lastUpdateTicks) > staleThresholdTicks) {
        state.status = ExternalSyncState::SeedStatus::Stale;
        return state;
    }

    state.status = ExternalSyncState::SeedStatus::Ok;
    return state;
}

IsochAudioTxPipeline::AudioInjectionPlan
IsochAudioTxPipeline::BuildAudioInjectionPlan(uint32_t hwPacketIndex) noexcept {
    constexpr uint32_t kNumPackets = Tx::Layout::kNumPackets;

    AudioInjectionPlan plan{};
    plan.audioTarget = (hwPacketIndex + Tx::Layout::kAudioWriteAhead) % kNumPackets;

    const uint32_t distBehind = (hwPacketIndex + kNumPackets - audioWriteIndex_) % kNumPackets;
    if (distBehind > 0 && distBehind < kNumPackets / 2) {
        counters_.audioInjectCursorResets.fetch_add(1, std::memory_order_relaxed);
        counters_.audioInjectMissedPackets.fetch_add(distBehind, std::memory_order_relaxed);
        audioWriteIndex_ = hwPacketIndex;
    }

    plan.packetsToInject = (plan.audioTarget + kNumPackets - audioWriteIndex_) % kNumPackets;
    if (plan.packetsToInject > Tx::Layout::kAudioWriteAhead) {
        plan.packetsToInject = Tx::Layout::kAudioWriteAhead;
    }
    if (plan.packetsToInject == 0) {
        return plan;
    }

    plan.framesPerPacket = assembler_.samplesPerDataPacket();
    plan.pcmChannels = assembler_.channelCount();
    plan.am824Slots = assembler_.am824SlotCount();
    return plan;
}

bool IsochAudioTxPipeline::PacketCarriesAudio(uint32_t packetIndex,
                                              Tx::IsochTxDescriptorSlab& slab) noexcept {
    return PacketPayloadByteCount(packetIndex, slab) > Encoding::kCIPHeaderSize;
}

uint32_t IsochAudioTxPipeline::PacketPayloadByteCount(uint32_t packetIndex,
                                                      Tx::IsochTxDescriptorSlab& slab) noexcept {
    const uint32_t descBase = packetIndex * Tx::Layout::kBlocksPerPacket;
    auto* lastDesc = slab.GetDescriptorPtr(descBase + 2);
    if (!lastDesc) {
        return 0;
    }

    return static_cast<uint16_t>(lastDesc->control & 0xFFFF);
}

bool IsochAudioTxPipeline::IsDirectTxHardwarePathReady(const AudioInjectionPlan& plan) const noexcept {
    if constexpr (!kEnableDirectTxHardwarePath) {
        return false;
    }

    if (!directTxBinding_.enabled ||
        directTxBinding_.outputBase == nullptr ||
        directTxBinding_.control == nullptr ||
        !directOutputReader_.IsBound()) {
        return false;
    }

    if (directTxBinding_.sampleRateHz != 48000 ||
        directTxBinding_.streamModeRaw != std::to_underlying(ASFW::Encoding::StreamMode::kBlocking) ||
        effectiveStreamMode_ != Encoding::StreamMode::kBlocking) {
        return false;
    }

    const auto format = assembler_.audioWireFormat();
    if (format != Encoding::AudioWireFormat::kAM824 &&
        format != Encoding::AudioWireFormat::kRawPcm24In32) {
        return false;
    }

    if (directTxBinding_.outputChannels == 0 ||
        directTxBinding_.outputChannels != plan.pcmChannels ||
        directTxBinding_.am824Slots < directTxBinding_.outputChannels ||
        directTxBinding_.am824Slots != plan.am824Slots) {
        return false;
    }

    return plan.framesPerPacket > 0 && plan.packetsToInject > 0;
}

void IsochAudioTxPipeline::PublishDirectTxConsumedEndFrame(uint64_t consumedEndFrame) noexcept {
    if (directTxBinding_.control) {
        directTxBinding_.control->outputConsumedEndFrame.store(consumedEndFrame,
                                                               std::memory_order_release);
    }
}

bool IsochAudioTxPipeline::InitializeDirectOutputCursor(const AudioInjectionPlan& plan) noexcept {
    const uint64_t writtenEnd = directOutputReader_.OutputWrittenEndFrame();
    if (writtenEnd == 0) {
        return false;
    }

    // Start the consumer a full target lead behind the HAL write cursor so it can
    // ride out the per-WriteEnd bursts (one period each); the deadband discipline in
    // InjectNearHw maintains it afterward. FW-26 #6/#8.
    constexpr uint64_t kTargetLead = Config::kOutputConsumerLeadFrames;
    directOutputFrameCursor_ = (writtenEnd > kTargetLead) ? (writtenEnd - kTargetLead) : 0;
    directCursorInitialized_ = true;

    // Hot-path cursor diagnostic, disabled for audio stability.
    // ASFW_LOG(Isoch,
    //          "IT: DIRECT-TX cursor init writtenEnd=%llu targetLead=%llu cursor=%llu ch=%u slots=%u framesPerPacket=%u",
    //          writtenEnd, kTargetLead, directOutputFrameCursor_,
    //          plan.pcmChannels, plan.am824Slots, plan.framesPerPacket);
    return true;
}

void IsochAudioTxPipeline::LogTxCursorDiagnostic(const char* source,
                                                 uint32_t packetIndex,
                                                 const ProducedPacketMetadata& metadata,
                                                 const PacketCipFields& cip,
                                                 uint64_t readFrame,
                                                 uint64_t consumedEndFrame,
                                                 DirectTxReadStatus readStatus,
                                                 uint32_t bytesWritten,
                                                 uint32_t framesEncoded,
                                                 bool usedSilence) noexcept {
    if (!ShouldLogTxHotPathSample(debugInjectionAttempts_)) {
        return;
    }

    const uint64_t writtenEnd = directOutputReader_.OutputWrittenEndFrame();
    const uint64_t requestedEnd = readFrame + metadata.framesPerPacket;
    int64_t leadFrames = 0;
    if (writtenEnd >= readFrame) {
        leadFrames = static_cast<int64_t>(writtenEnd - readFrame);
    } else {
        const uint64_t underrunFrames = readFrame - writtenEnd;
        leadFrames = underrunFrames > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
            ? std::numeric_limits<int64_t>::min()
            : -static_cast<int64_t>(underrunFrames);
    }

    const uint32_t ringFrame = directTxBinding_.outputFrames == 0
        ? 0
        : static_cast<uint32_t>(readFrame % directTxBinding_.outputFrames);

    uint64_t writeEndCount = 0;
    uint64_t ztsTotal = 0;
    uint64_t ztsRx = 0;
    uint64_t ztsRxAdk = 0;
    uint64_t deviceFrame = 0;
    uint64_t consumedPublished = 0;
    if (directTxBinding_.control) {
        writeEndCount =
            directTxBinding_.control->counters.ioWriteEndCount.load(std::memory_order_relaxed);
        ztsTotal =
            directTxBinding_.control->counters.ztsPublished.load(std::memory_order_relaxed);
        ztsRx =
            directTxBinding_.control->counters.ztsRxPublished.load(std::memory_order_relaxed);
        ztsRxAdk =
            directTxBinding_.control->counters.ztsRxAdkPublished.load(std::memory_order_relaxed);
        deviceFrame =
            directTxBinding_.control->device.sampleFrame.load(std::memory_order_acquire);
        consumedPublished =
            directTxBinding_.control->outputConsumedEndFrame.load(std::memory_order_acquire);
    }

    const auto sync = ReadExternalSyncState(/*allowStartupQualifiedOnly=*/false);
    ASFW_LOG(Isoch,
             "IT DBG TXCURSOR source=%{public}s attempt=%llu pktIdx=%u txSyt=0x%04x rxSyt=0x%04x rxSeq=%u rxStatus=%u rxAge=%llu dbc=0x%02x read=%llu reqEnd=%llu consumed=%llu publishedConsumed=%llu writeEnd=%llu lead=%lld ring=%u/%u writeEndCount=%llu zts(total=%llu rx=%llu rxAdk=%llu deviceFrame=%llu) status=%u bytes=%u/%u frames=%u/%u silence=%d",
             source,
             debugInjectionAttempts_,
             packetIndex,
             cip.syt,
             sync.rxSyt,
             sync.updateSeq,
             static_cast<uint32_t>(sync.status),
             sync.ageUsec,
             cip.dbc,
             readFrame,
             requestedEnd,
             consumedEndFrame,
             consumedPublished,
             writtenEnd,
             leadFrames,
             ringFrame,
             directTxBinding_.outputFrames,
             writeEndCount,
             ztsTotal,
             ztsRx,
             ztsRxAdk,
             deviceFrame,
             static_cast<uint32_t>(readStatus),
             bytesWritten,
             metadata.sizeBytes,
             framesEncoded,
             metadata.framesPerPacket,
             usedSilence);
}

bool IsochAudioTxPipeline::TryWriteDirectTxPacket(uint32_t packetIndex,
                                                  Tx::IsochTxDescriptorSlab& slab,
                                                  const AudioInjectionPlan& plan) noexcept {
    if (directTxBinding_.control &&
        directTxBinding_.control->ztsState.selectedSource.load(std::memory_order_relaxed) == ASFW::Audio::Runtime::ZtsAuthoritySource::None) {
        counters_.directTxInvalidPackets.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    ++debugInjectionAttempts_;
    uint8_t* payloadVirt = slab.PayloadPtr(packetIndex);
    const uint32_t payloadBytes = PacketPayloadByteCount(packetIndex, slab);
    if (!payloadVirt || payloadBytes <= Encoding::kCIPHeaderSize) {
        // Hot-path IT injection diagnostic, disabled for audio stability.
        // ++debugInjectionSkips_;
        // if (ShouldLogTxHotPathSample(debugInjectionAttempts_)) {
        //     ASFW_LOG(Isoch,
        //              "IT DBG INJECT skip=no_payload attempt=%llu pktIdx=%u payload=%u ptr=%p",
        //              debugInjectionAttempts_, packetIndex, payloadBytes, payloadVirt);
        // }
        counters_.directTxInvalidPackets.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (packetIndex >= producedPacketMetadata_.size()) {
        // Hot-path IT injection diagnostic, disabled for audio stability.
        // ++debugInjectionSkips_;
        // ASFW_LOG(Isoch,
        //          "IT DBG INJECT skip=packet_index_oob attempt=%llu pktIdx=%u payload=%u",
        //          debugInjectionAttempts_, packetIndex, payloadBytes);
        counters_.directTxInvalidPackets.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto& metadata = producedPacketMetadata_[packetIndex];
    if (!metadata.valid || !metadata.isData || metadata.sizeBytes != payloadBytes) {
        // Hot-path IT injection diagnostic, disabled for audio stability.
        // ++debugInjectionSkips_;
        // if (ShouldLogTxHotPathSample(debugInjectionAttempts_)) {
        //     ASFW_LOG(Isoch,
        //              "IT DBG INJECT skip=metadata attempt=%llu pktIdx=%u valid=%d isData=%d metaSize=%u payload=%u metaSyt=0x%04x",
        //              debugInjectionAttempts_,
        //              packetIndex,
        //              metadata.valid,
        //              metadata.isData,
        //              metadata.sizeBytes,
        //              payloadBytes,
        //              metadata.cip.syt);
        // }
        counters_.directTxInvalidPackets.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const PacketCipFields cip = metadata.cip;
    const uint32_t am824Slots = metadata.am824Slots == 0 ? metadata.pcmChannels : metadata.am824Slots;
    const auto format = metadata.wireFormat;

    bool armed = IsDirectTxHardwarePathReady(plan);
    // Hot-path IT injection diagnostic, disabled for audio stability.
    // if (ShouldLogTxHotPathSample(debugInjectionAttempts_)) {
    //     ASFW_LOG(Isoch,
    //              "IT DBG INJECT attempt=%llu pktIdx=%u armed0=%d payload=%u frames=%u ch=%u slots=%u sid=%u dbc=0x%02x syt=0x%04x bind(en=%d base=%p frames=%u ch=%u rate=%u control=%p bound=%d)",
    //              debugInjectionAttempts_,
    //              packetIndex,
    //              armed,
    //              payloadBytes,
    //              metadata.framesPerPacket,
    //              metadata.pcmChannels,
    //              am824Slots,
    //              cip.sid,
    //              cip.dbc,
    //              cip.syt,
    //              directTxBinding_.enabled,
    //              static_cast<const void*>(directTxBinding_.outputBase),
    //              directTxBinding_.outputFrames,
    //              directTxBinding_.outputChannels,
    //              directTxBinding_.sampleRateHz,
    //              static_cast<void*>(directTxBinding_.control),
    //              directOutputReader_.IsBound());
    // }
    if (armed) {
        if (!directCursorInitialized_ && !InitializeDirectOutputCursor(plan)) {
            // Hot-path IT injection diagnostic, disabled for audio stability.
            // if (ShouldLogTxHotPathSample(debugInjectionAttempts_)) {
            //     ASFW_LOG(Isoch,
            //              "IT DBG INJECT source=silence_no_cursor attempt=%llu pktIdx=%u writtenEnd=%llu",
            //              debugInjectionAttempts_,
            //              packetIndex,
            //              directOutputReader_.OutputWrittenEndFrame());
            // }
            armed = false;
        }
    }

    if (armed && directOutputFrameCursor_ >
        (std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(metadata.framesPerPacket))) {
        counters_.directTxInvalidPackets.fetch_add(1, std::memory_order_relaxed);
        armed = false;
    }

    if (armed) {
        // 1. Check if RX Clock authority was once established and is now lost/stale
        bool rxAuthorityEstablished = false;
        if (directTxBinding_.control) {
            const auto source = directTxBinding_.control->ztsState.selectedSource.load(std::memory_order_relaxed);
            const auto gen = directTxBinding_.control->ztsState.sourceGeneration.load(std::memory_order_relaxed);
            if (source == ASFW::Audio::Runtime::ZtsAuthoritySource::RxClock && gen > 0) {
                rxAuthorityEstablished = true;
            }
        }

        const auto sync = ReadExternalSyncState(/*allowStartupQualifiedOnly=*/false);
        if (rxAuthorityEstablished && sync.status != ExternalSyncState::SeedStatus::Ok) {
            if (directTxBinding_.control) {
                directTxBinding_.control->ztsState.selectedSource.store(ASFW::Audio::Runtime::ZtsAuthoritySource::None, std::memory_order_relaxed);
                directTxBinding_.control->counters.txStaleSyncPackets.fetch_add(1, std::memory_order_relaxed);
                directTxBinding_.control->fatalReason.store(ASFW::Audio::Runtime::FatalStreamReason::RxAuthorityLost, std::memory_order_release);
                directTxBinding_.control->fatalGeneration.fetch_add(1, std::memory_order_release);
            }
            ASFW_LOG(Audio, "ADK FATAL: RX clock authority lost! status=%u ageUsec=%llu threshold=%llu. Stopping stream.",
                     static_cast<uint32_t>(sync.status), sync.ageUsec, sync.staleThresholdUsec);
            counters_.directTxInvalidPackets.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const uint64_t readFrame = directOutputFrameCursor_;
        const Direct::Tx::TxAudioPacketWriteRequest request{
            .firstFrame = readFrame,
            .frameCount = metadata.framesPerPacket,
            .channels = metadata.pcmChannels,
            .am824Slots = am824Slots,
            .sid = cip.sid,
            .dbc = cip.dbc,
            .syt = cip.syt,
            .dataPacket = true,
            .wireFormat = format,
        };

        Direct::Tx::TxAudioPacketWriter writer(directOutputReader_);
        const ASFW::Audio::Runtime::TxPacketProductionResult result =
            writer.WritePacket(request, payloadVirt, payloadBytes);
        
        if (result.fatal) {
            counters_.directTxInvalidPackets.fetch_add(1, std::memory_order_relaxed);
            if (directTxBinding_.control) {
                directTxBinding_.control->counters.txInvalidGeometryPackets.fetch_add(1, std::memory_order_relaxed);
                directTxBinding_.control->fatalReason.store(ASFW::Audio::Runtime::FatalStreamReason::InvalidGeometry, std::memory_order_release);
                directTxBinding_.control->fatalGeneration.fetch_add(1, std::memory_order_release);
            }
            return false;
        }

        const uint32_t bytesWritten = result.quadlets * sizeof(uint32_t);
        if (bytesWritten == payloadBytes && result.frames == metadata.framesPerPacket) {
            if (dmaMemory_ && bytesWritten > 0) {
                dmaMemory_->PublishToDevice(
                    reinterpret_cast<const std::byte*>(payloadVirt),
                    bytesWritten
                );
            }

            // Increment specific atomic counter based on packet state
            if (directTxBinding_.control) {
                auto& counters = directTxBinding_.control->counters;
                if (result.syt == 0xffff) {
                    counters.txSytFfffPackets.fetch_add(1, std::memory_order_relaxed);
                } else {
                    counters.txValidSytPackets.fetch_add(1, std::memory_order_relaxed);
                }

                switch (result.state) {
                    case ASFW::Audio::Runtime::TxPacketState::ValidPhasePcm:
                        counters.txValidPhasePcmPackets.fetch_add(1, std::memory_order_relaxed);
                        counters.txPcmFramesEncoded.fetch_add(result.frames, std::memory_order_relaxed);
                        break;
                    case ASFW::Audio::Runtime::TxPacketState::ValidPhaseSilence:
                        counters.txValidPhaseSilencePackets.fetch_add(1, std::memory_order_relaxed);
                        counters.txSilenceSubstitutions.fetch_add(1, std::memory_order_relaxed);
                        break;
                    case ASFW::Audio::Runtime::TxPacketState::NoPhaseSilence:
                        counters.txNoPhaseSilencePackets.fetch_add(1, std::memory_order_relaxed);
                        counters.txSilenceSubstitutions.fetch_add(1, std::memory_order_relaxed);
                        break;
                    case ASFW::Audio::Runtime::TxPacketState::UnderrunSilence:
                        counters.txUnderrunSilencePackets.fetch_add(1, std::memory_order_relaxed);
                        counters.txSilenceSubstitutions.fetch_add(1, std::memory_order_relaxed);
                        counters.txUnderruns.fetch_add(1, std::memory_order_relaxed);
                        break;
                    case ASFW::Audio::Runtime::TxPacketState::StaleSync:
                        counters.txStaleSyncPackets.fetch_add(1, std::memory_order_relaxed);
                        break;
                    case ASFW::Audio::Runtime::TxPacketState::InvalidGeometry:
                        counters.txInvalidGeometryPackets.fetch_add(1, std::memory_order_relaxed);
                        break;
                    default:
                        break;
                }

                // Increment baseline counters
                counters.txPackets.fetch_add(1, std::memory_order_relaxed);
                if (result.blockingResult == ASFW::Audio::Runtime::TxBlockingResult::Data) {
                    counters.txDataPackets.fetch_add(1, std::memory_order_relaxed);
                } else {
                    counters.txNoDataPackets.fetch_add(1, std::memory_order_relaxed);
                }
            }

            const uint64_t consumedEndFrame = readFrame + result.frames;
            directOutputFrameCursor_ = consumedEndFrame;
            PublishDirectTxConsumedEndFrame(consumedEndFrame);
            counters_.directTxPackets.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        counters_.directTxInvalidPackets.fetch_add(1, std::memory_order_relaxed);
    }

    // Write silence to maintain bus-visible cadence (unarmed or fallback-due-to-error).
    uint32_t bytesWritten = 0;
    const Direct::Tx::DirectTxPacketHeaderRequest header{
        .sid = cip.sid,
        .am824Slots = am824Slots,
        .dbc = cip.dbc,
        .syt = cip.syt,
        .isNoData = false,
    };

    if (Direct::Tx::BeginDirectTxPacket(header, payloadVirt, payloadBytes, bytesWritten)) {
        auto* payload = Direct::Tx::DirectTxPacketPayloadQuadlets(payloadVirt);
        if (payload) {
            for (uint32_t frame = 0; frame < metadata.framesPerPacket; ++frame) {
                auto* frameOut = payload + (static_cast<size_t>(frame) * am824Slots);
                Direct::Tx::EncodeDirectTxSilenceFrame(metadata.pcmChannels, am824Slots, format, frameOut);
            }
            if (dmaMemory_ && payloadBytes > 0) {
                dmaMemory_->PublishToDevice(
                    reinterpret_cast<const std::byte*>(payloadVirt),
                    payloadBytes
                );
            }
            counters_.directTxUnderrunSilencedPackets.fetch_add(1, std::memory_order_relaxed);
            counters_.directTxPackets.fetch_add(1, std::memory_order_relaxed);
            if (directTxBinding_.control) {
                bool rxAuthorityEstablished = false;
                const auto source = directTxBinding_.control->ztsState.selectedSource.load(std::memory_order_relaxed);
                const auto gen = directTxBinding_.control->ztsState.sourceGeneration.load(std::memory_order_relaxed);
                if (source == ASFW::Audio::Runtime::ZtsAuthoritySource::RxClock && gen > 0) {
                    rxAuthorityEstablished = true;
                }

                if (rxAuthorityEstablished) {
                    directTxBinding_.control->counters.txValidPhaseSilencePackets.fetch_add(1, std::memory_order_relaxed);
                } else {
                    directTxBinding_.control->counters.txNoPhaseSilencePackets.fetch_add(1, std::memory_order_relaxed);
                }

                directTxBinding_.control->counters.txPackets.fetch_add(
                    1, std::memory_order_relaxed);
                directTxBinding_.control->counters.txSilenceSubstitutions.fetch_add(
                    1, std::memory_order_relaxed);
                directTxBinding_.control->counters.txUnderruns.fetch_add(
                    1, std::memory_order_relaxed);
                directTxBinding_.control->counters.txNoDataPackets.fetch_add(
                    1, std::memory_order_relaxed);
            }
            // Hot-path IT injection diagnostic, disabled for audio stability.
            // ++debugInjectionSuccesses_;
            // if (ShouldLogTxHotPathSample(debugInjectionAttempts_)) {
            //     ASFW_LOG(Isoch,
            //              "IT DBG INJECT source=%{public}s attempt=%llu success=%llu pktIdx=%u bytes=%u frames=%u dbc=0x%02x syt=0x%04x armed=%d",
            //              "silence_no_pcm",
            //              debugInjectionAttempts_,
            //              debugInjectionSuccesses_,
            //              packetIndex,
            //              payloadBytes,
            //              metadata.framesPerPacket,
            //              cip.dbc,
            //              cip.syt,
            //              armed);
            // }
            // Hot-path cursor diagnostic, disabled for audio stability.
            // LogTxCursorDiagnostic("silence_no_pcm",
            //                       packetIndex,
            //                       metadata,
            //                       cip,
            //                       directOutputFrameCursor_,
            //                       directOutputFrameCursor_,
            //                       DirectTxReadStatus::kUnavailable,
            //                       bytesWritten,
            //                       metadata.framesPerPacket,
            //                       true);
            return true;
        }
    }

    // Hot-path IT injection diagnostic, disabled for audio stability.
    // ++debugInjectionSkips_;
    // if (ShouldLogTxHotPathSample(debugInjectionAttempts_)) {
    //     ASFW_LOG(Isoch,
    //              "IT DBG INJECT fallback_failed attempt=%llu skips=%llu pktIdx=%u",
    //              debugInjectionAttempts_, debugInjectionSkips_, packetIndex);
    // }
    return false;
}

void IsochAudioTxPipeline::InjectNearHw(uint32_t hwPacketIndex, Tx::IsochTxDescriptorSlab& slab) noexcept {
    auto plan = BuildAudioInjectionPlan(hwPacketIndex);
    // Hot-path IT planning diagnostic, disabled for audio stability.
    // if (ShouldLogTxHotPathSample(debugInjectionAttempts_ + 1)) {
    //     ASFW_LOG(Isoch,
    //              "IT DBG PLAN hwPkt=%u writeIdx=%u target=%u todo=%u frames=%u ch=%u slots=%u bind(en=%d base=%p frames=%u ch=%u rate=%u control=%p bound=%d)",
    //              hwPacketIndex,
    //              audioWriteIndex_,
    //              plan.audioTarget,
    //              plan.packetsToInject,
    //              plan.framesPerPacket,
    //              plan.pcmChannels,
    //              plan.am824Slots,
    //              directTxBinding_.enabled,
    //              static_cast<const void*>(directTxBinding_.outputBase),
    //              directTxBinding_.outputFrames,
    //              directTxBinding_.outputChannels,
    //              directTxBinding_.sampleRateHz,
    //              static_cast<void*>(directTxBinding_.control),
    //              directOutputReader_.IsBound());
    // }
    if (plan.packetsToInject == 0) {
        return;
    }

    // Keep the read cursor a stable lead behind the HAL write cursor. The producer
    // (writtenEnd) advances on the ZTS clock while the consumer advances on the OHCI
    // clock, so they drift; rebase only when the lead leaves the deadband. FW-26 #6/#8.
    if (directCursorInitialized_) {
        const uint64_t writtenEnd = directOutputReader_.OutputWrittenEndFrame();
        const auto disc = Direct::Tx::DisciplineOutputCursor(
            directOutputFrameCursor_,
            writtenEnd,
            Config::kOutputConsumerLeadFrames,
            Config::kOutputCursorResyncDeadbandFrames);
        if (disc.resynced) {
            directOutputFrameCursor_ = disc.newCursor;
            [[maybe_unused]] const uint64_t resyncs =
                counters_.directTxCursorResyncs.fetch_add(1, std::memory_order_relaxed) + 1;
            // Hot-path cursor diagnostic, disabled for audio stability.
            // if (ShouldLogTxHotPathSample(resyncs)) {
            //     ASFW_LOG(Isoch,
            //              "IT DBG CURSOR resync n=%llu cursor=%llu writtenEnd=%llu targetLead=%u",
            //              resyncs, directOutputFrameCursor_, writtenEnd,
            //              Config::kOutputConsumerLeadFrames);
            // }
        }
    }

    for (uint32_t i = 0; i < plan.packetsToInject; ++i) {
        const uint32_t packetIndex = (audioWriteIndex_ + i) % Tx::Layout::kNumPackets;
        if (!PacketCarriesAudio(packetIndex, slab)) {
            continue;
        }

        (void)TryWriteDirectTxPacket(packetIndex, slab, plan);
    }

    audioWriteIndex_ = plan.audioTarget;

    std::atomic_thread_fence(std::memory_order_release);
    ASFW::Driver::WriteBarrier();
}

} // namespace ASFW::Isoch
