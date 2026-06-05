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
    debugProducedPackets_ = 0;
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

    const ProducedPacketMetadata metadata{
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
    if (request.packetIndex < producedPacketMetadata_.size()) {
        producedPacketMetadata_[request.packetIndex] = metadata;
    }

    Tx::IsochTxPacket out{};
    std::memcpy(silentPacketStorage_.data(), pkt.data, pkt.size);
    if (pkt.isData) {
        (void)TryBuildPlaybackRingPacket(metadata,
                                         silentPacketStorage_.data(),
                                         pkt.size);
    }
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

bool IsochAudioTxPipeline::IsPlaybackRingPathReady(const ProducedPacketMetadata& metadata) const noexcept {
    if constexpr (!kEnableDirectTxHardwarePath) {
        return false;
    }

    if (!metadata.valid || !metadata.isData || metadata.framesPerPacket == 0) {
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
        directTxBinding_.outputChannels != metadata.pcmChannels ||
        directTxBinding_.am824Slots < directTxBinding_.outputChannels ||
        directTxBinding_.am824Slots != metadata.am824Slots) {
        return false;
    }

    return true;
}

void IsochAudioTxPipeline::PublishDirectTxConsumedEndFrame(uint64_t consumedEndFrame) noexcept {
    if (directTxBinding_.control) {
        directTxBinding_.control->outputConsumedEndFrame.store(consumedEndFrame,
                                                               std::memory_order_release);
        directTxBinding_.control->playbackRingReadFrame.store(consumedEndFrame,
                                                              std::memory_order_release);
    }
}

bool IsochAudioTxPipeline::InitializeDirectOutputCursor(const ProducedPacketMetadata& metadata) noexcept {
    const uint64_t writtenEnd = directOutputReader_.OutputWrittenEndFrame();
    if (writtenEnd == 0) {
        return false;
    }

    const uint64_t kTargetLead =
        timingPolicy_.CursorOffsetFrames(ASFW::Audio::AudioDirection::Output);
    directOutputFrameCursor_ = (writtenEnd > kTargetLead) ? (writtenEnd - kTargetLead) : 0;
    directCursorInitialized_ = true;

    // Hot-path cursor diagnostic, disabled for audio stability.
    // ASFW_LOG(Isoch,
    //          "IT: DIRECT-TX cursor init writtenEnd=%llu targetLead=%llu cursor=%llu ch=%u slots=%u framesPerPacket=%u",
    //          writtenEnd, kTargetLead, directOutputFrameCursor_,
    //          metadata.pcmChannels, metadata.am824Slots, metadata.framesPerPacket);
    return true;
}

bool IsochAudioTxPipeline::TryBuildPlaybackRingPacket(const ProducedPacketMetadata& metadata,
                                                      uint8_t* packetBytes,
                                                      uint32_t packetCapacityBytes) noexcept {
    if (!metadata.valid || !metadata.isData || !packetBytes || packetCapacityBytes == 0) {
        return false;
    }

    const PacketCipFields cip = metadata.cip;
    const uint32_t am824Slots =
        metadata.am824Slots == 0 ? metadata.pcmChannels : metadata.am824Slots;
    const auto format = metadata.wireFormat;

    auto writeSilence = [&](ASFW::Audio::Runtime::TxPacketState state) noexcept -> bool {
        uint32_t bytesWritten = 0;
        const Direct::Tx::DirectTxPacketHeaderRequest header{
            .sid = cip.sid,
            .am824Slots = am824Slots,
            .dbc = cip.dbc,
            .syt = cip.syt,
            .isNoData = false,
        };
        if (!Direct::Tx::BeginDirectTxPacket(header, packetBytes, packetCapacityBytes, bytesWritten)) {
            return false;
        }
        auto* payload = Direct::Tx::DirectTxPacketPayloadQuadlets(packetBytes);
        if (!payload) {
            return false;
        }
        for (uint32_t frame = 0; frame < metadata.framesPerPacket; ++frame) {
            auto* frameOut = payload + (static_cast<size_t>(frame) * am824Slots);
            Direct::Tx::EncodeDirectTxSilenceFrame(metadata.pcmChannels, am824Slots, format, frameOut);
        }
        counters_.directTxUnderrunSilencedPackets.fetch_add(1, std::memory_order_relaxed);
        counters_.directTxPackets.fetch_add(1, std::memory_order_relaxed);
        if (directTxBinding_.control) {
            auto& counters = directTxBinding_.control->counters;
            counters.txPackets.fetch_add(1, std::memory_order_relaxed);
            counters.txDataPackets.fetch_add(1, std::memory_order_relaxed);
            counters.txSilenceSubstitutions.fetch_add(1, std::memory_order_relaxed);
            if (cip.syt == 0xffff) {
                counters.txSytFfffPackets.fetch_add(1, std::memory_order_relaxed);
            } else {
                counters.txValidSytPackets.fetch_add(1, std::memory_order_relaxed);
            }
            switch (state) {
                case ASFW::Audio::Runtime::TxPacketState::NoPhaseSilence:
                    counters.txNoPhaseSilencePackets.fetch_add(1, std::memory_order_relaxed);
                    break;
                case ASFW::Audio::Runtime::TxPacketState::UnderrunSilence:
                    counters.txUnderrunSilencePackets.fetch_add(1, std::memory_order_relaxed);
                    counters.txUnderruns.fetch_add(1, std::memory_order_relaxed);
                    directTxBinding_.control->playbackRingUnderruns.fetch_add(1, std::memory_order_relaxed);
                    break;
                case ASFW::Audio::Runtime::TxPacketState::StaleSync:
                    counters.txStaleSyncPackets.fetch_add(1, std::memory_order_relaxed);
                    break;
                default:
                    counters.txValidPhaseSilencePackets.fetch_add(1, std::memory_order_relaxed);
                    break;
            }
        }
        return true;
    };

    if (cip.syt == Encoding::SYTGenerator::kNoInfo) {
        return writeSilence(ASFW::Audio::Runtime::TxPacketState::NoPhaseSilence);
    }

    if (!IsPlaybackRingPathReady(metadata)) {
        return writeSilence(ASFW::Audio::Runtime::TxPacketState::UnderrunSilence);
    }

    bool rxAuthorityEstablished = false;
    if (directTxBinding_.control) {
        const auto source =
            directTxBinding_.control->ztsState.selectedSource.load(std::memory_order_relaxed);
        const auto gen =
            directTxBinding_.control->ztsState.sourceGeneration.load(std::memory_order_relaxed);
        rxAuthorityEstablished =
            (source == ASFW::Audio::Runtime::ZtsAuthoritySource::RxClock && gen > 0);
    }

    if (rxAuthorityEstablished) {
        const auto sync = ReadExternalSyncState(/*allowStartupQualifiedOnly=*/false);
        if (sync.status != ExternalSyncState::SeedStatus::Ok) {
            if (directTxBinding_.control) {
                directTxBinding_.control->ztsState.selectedSource.store(
                    ASFW::Audio::Runtime::ZtsAuthoritySource::None,
                    std::memory_order_relaxed);
                directTxBinding_.control->fatalReason.store(
                    ASFW::Audio::Runtime::FatalStreamReason::RxAuthorityLost,
                    std::memory_order_release);
                directTxBinding_.control->fatalGeneration.fetch_add(1, std::memory_order_release);
            }
            ASFW_LOG(Audio,
                     "ADK FATAL: RX clock authority lost during playback-ring TX status=%u ageUsec=%llu threshold=%llu",
                     static_cast<uint32_t>(sync.status),
                     sync.ageUsec,
                     sync.staleThresholdUsec);
            counters_.directTxInvalidPackets.fetch_add(1, std::memory_order_relaxed);
            return writeSilence(ASFW::Audio::Runtime::TxPacketState::StaleSync);
        }
    }

    if (!directCursorInitialized_ && !InitializeDirectOutputCursor(metadata)) {
        return writeSilence(ASFW::Audio::Runtime::TxPacketState::UnderrunSilence);
    }

    const uint64_t writtenEnd = directOutputReader_.OutputWrittenEndFrame();
    if (directCursorInitialized_) {
        const uint64_t targetLead =
            timingPolicy_.CursorOffsetFrames(ASFW::Audio::AudioDirection::Output);
        const auto disc = Direct::Tx::DisciplineOutputCursor(
            directOutputFrameCursor_,
            writtenEnd,
            targetLead,
            timingPolicy_.SafetyOffsetFrames(ASFW::Audio::AudioDirection::Output));
        if (disc.resynced) {
            directOutputFrameCursor_ = disc.newCursor;
            counters_.directTxCursorResyncs.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (directOutputFrameCursor_ >
        (std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(metadata.framesPerPacket))) {
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
        writer.WritePacket(request, packetBytes, packetCapacityBytes);

    if (result.fatal) {
        counters_.directTxInvalidPackets.fetch_add(1, std::memory_order_relaxed);
        if (directTxBinding_.control) {
            directTxBinding_.control->counters.txInvalidGeometryPackets.fetch_add(1, std::memory_order_relaxed);
            directTxBinding_.control->fatalReason.store(ASFW::Audio::Runtime::FatalStreamReason::InvalidGeometry,
                                                        std::memory_order_release);
            directTxBinding_.control->fatalGeneration.fetch_add(1, std::memory_order_release);
        }
        return false;
    }

    if (result.frames != metadata.framesPerPacket) {
        return writeSilence(ASFW::Audio::Runtime::TxPacketState::UnderrunSilence);
    }

    if (directTxBinding_.control) {
        auto& counters = directTxBinding_.control->counters;
        counters.txPackets.fetch_add(1, std::memory_order_relaxed);
        if (result.blockingResult == ASFW::Audio::Runtime::TxBlockingResult::Data) {
            counters.txDataPackets.fetch_add(1, std::memory_order_relaxed);
        } else {
            counters.txNoDataPackets.fetch_add(1, std::memory_order_relaxed);
        }
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
            case ASFW::Audio::Runtime::TxPacketState::UnderrunSilence:
                counters.txUnderrunSilencePackets.fetch_add(1, std::memory_order_relaxed);
                counters.txSilenceSubstitutions.fetch_add(1, std::memory_order_relaxed);
                counters.txUnderruns.fetch_add(1, std::memory_order_relaxed);
                directTxBinding_.control->playbackRingUnderruns.fetch_add(1, std::memory_order_relaxed);
                break;
            case ASFW::Audio::Runtime::TxPacketState::ValidPhaseSilence:
                counters.txValidPhaseSilencePackets.fetch_add(1, std::memory_order_relaxed);
                counters.txSilenceSubstitutions.fetch_add(1, std::memory_order_relaxed);
                break;
            case ASFW::Audio::Runtime::TxPacketState::NoPhaseSilence:
                counters.txNoPhaseSilencePackets.fetch_add(1, std::memory_order_relaxed);
                counters.txSilenceSubstitutions.fetch_add(1, std::memory_order_relaxed);
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
    }

    directOutputFrameCursor_ = readFrame + result.frames;
    PublishDirectTxConsumedEndFrame(directOutputFrameCursor_);
    counters_.directTxPackets.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace ASFW::Isoch
