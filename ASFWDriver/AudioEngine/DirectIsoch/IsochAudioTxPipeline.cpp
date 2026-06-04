// IsochAudioTxPipeline.cpp
// ASFW - Audio semantics layer for IT transmit (Direct-Only implementation).

#include "IsochAudioTxPipeline.hpp"

#include "../../AudioWire/AMDTP/TimingUtils.hpp"
#include "../Direct/Tx/DirectTxPacketEncoder.hpp"

#include <limits>

namespace ASFW::Isoch {

namespace Direct = ASFW::AudioEngine::Direct;
using DirectTxReadStatus = ASFW::AudioEngine::Direct::Tx::DirectTxReadStatus;

namespace {

inline uint64_t ExternalSyncStaleThresholdTicks(const bool allowStartupQualifiedOnly) noexcept {
    const uint64_t staleThresholdNanos = allowStartupQualifiedOnly
        ? ASFW::Isoch::Core::kExternalSyncStartupSeedGraceNanos
        : ASFW::Isoch::Core::kExternalSyncLiveStaleNanos;
    uint64_t staleThresholdTicks = ASFW::Timing::nanosToHostTicks(staleThresholdNanos);
    if (staleThresholdTicks == 0 && ASFW::Timing::initializeHostTimebase()) {
        staleThresholdTicks = ASFW::Timing::nanosToHostTicks(staleThresholdNanos);
    }
    return staleThresholdTicks;
}

} // namespace

void IsochAudioTxPipeline::SetExternalSyncBridge(Core::ExternalSyncBridge* bridge) noexcept {
    externalSyncBridge_ = bridge;
    externalSyncDiscipline_.Reset();
}

void IsochAudioTxPipeline::SetDirectTxRuntimeBinding(const DirectTxRuntimeBinding& binding) noexcept {
    directTxBinding_ = binding;
    directOutputFrameCursor_ = 0;
    directCursorInitialized_ = false;

    // Rebuild the isoch-owned read view over the shared output memory + control
    // block. No ADK object pointers are stored; the reader only ever touches
    // raw stream memory and the atomic transport cursors.
    directOutputView_ = {};
    directOutputView_.memory.outputBase = binding.outputBase;
    directOutputView_.memory.outputFrameCapacity = binding.outputFrames;
    directOutputView_.memory.outputChannels = binding.outputChannels;
    directOutputView_.memory.storage = ASFW::Audio::Runtime::AudioSampleStorage::kInt32Native;
    directOutputView_.hostToDeviceAm824Slots = binding.am824Slots;
    directOutputView_.control = binding.control;
    directOutputReader_.Bind(&directOutputView_);

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
    externalSyncDiscipline_.Reset();
}

bool IsochAudioTxPipeline::PrimeSyncFromExternalBridge() noexcept {
    const auto syncState = ReadExternalSyncState(/*allowStartupQualifiedOnly=*/true);
    if (syncState.status != ExternalSyncState::SeedStatus::Ok) {
        ASFW_LOG(Isoch, "IT: Arming transmit-cycle SYT anchor (rx status=%u established=%d startupQual=%d seq=%u syt=0x%04x fdf=0x%02x dbs=%u age=%llu threshold=%llu)",
                 static_cast<uint32_t>(syncState.status),
                 syncState.clockEstablished,
                 syncState.startupQualified,
                 syncState.updateSeq,
                 syncState.rxSyt,
                 syncState.rxFdf,
                 syncState.rxDbs,
                 syncState.ageUsec,
                 syncState.staleThresholdUsec);
        sytGenerator_.armTransmitCycleAnchor();
        return true;
    }

    ASFW_LOG(Isoch, "IT: Arming transmit-cycle SYT anchor (rx established seq=%u syt=0x%04x fdf=0x%02x dbs=%u)",
             syncState.updateSeq, syncState.rxSyt, syncState.rxFdf, syncState.rxDbs);
    sytGenerator_.armTransmitCycleAnchor();
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

    ++debugProducedPackets_;
    if (debugProducedPackets_ <= 64 || (debugProducedPackets_ % 1000) == 0) {
        ASFW_LOG(Isoch,
                 "IT DBG PRODUCE n=%llu pktIdx=%u txCycle=%u hwTs=0x%04x isData=%d size=%u dbc=0x%02x syt=0x%04x ch=%u slots=%u fmt=%u",
                 debugProducedPackets_,
                 request.packetIndex,
                 request.transmitCycle,
                 request.hwTimestamp,
                 pkt.isData,
                 pkt.size,
                 pkt.dbc,
                 pkt.isData ? syt : Encoding::SYTGenerator::kNoInfo,
                 assembler_.channelCount(),
                 assembler_.am824SlotCount(),
                 static_cast<uint32_t>(assembler_.audioWireFormat()));
    }
    return out;
}

uint16_t IsochAudioTxPipeline::ComputeDataSyt(uint32_t transmitCycle) noexcept {
    if (!sytGenerator_.isValid() || !cycleTrackingValid_) {
        return Encoding::SYTGenerator::kNoInfo;
    }

    const uint16_t txSyt = sytGenerator_.computeDataSYT(transmitCycle, assembler_.samplesPerDataPacket());
    const bool safetyOk = MaybeApplyExternalSyncDiscipline(txSyt);
    if (!safetyOk) {
        return Encoding::SYTGenerator::kNoInfo;
    }
    return txSyt;
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
    state.rxSyt = Core::ExternalSyncBridge::UnpackSYT(packed);
    state.rxFdf = Core::ExternalSyncBridge::UnpackFDF(packed);
    state.rxDbs = Core::ExternalSyncBridge::UnpackDBS(packed);
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

bool IsochAudioTxPipeline::MaybeApplyExternalSyncDiscipline(uint16_t txSyt) noexcept {
    const auto syncState = ReadExternalSyncState(/*allowStartupQualifiedOnly=*/false);
    if (syncState.status != ExternalSyncState::SeedStatus::Ok) {
        return true;
    }

    const auto result = externalSyncDiscipline_.Update(true, txSyt, syncState.rxSyt);
    if (result.correctionTicks != 0) {
        sytGenerator_.nudgeOffsetTicks(result.correctionTicks);
    }

    return result.safetyGateOpen;
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

    constexpr uint64_t kMinSafetyLeadFrames = 64;
    const uint64_t packetLead = static_cast<uint64_t>(plan.framesPerPacket) * 3u;
    const uint64_t safetyLead = (packetLead > kMinSafetyLeadFrames) ? packetLead : kMinSafetyLeadFrames;

    directOutputFrameCursor_ = (writtenEnd > safetyLead) ? (writtenEnd - safetyLead) : 0;
    directCursorInitialized_ = true;

    ASFW_LOG(Isoch,
             "IT: DIRECT-TX cursor init writtenEnd=%llu safetyLead=%llu cursor=%llu ch=%u slots=%u framesPerPacket=%u",
             writtenEnd, safetyLead, directOutputFrameCursor_,
             plan.pcmChannels, plan.am824Slots, plan.framesPerPacket);
    return true;
}

bool IsochAudioTxPipeline::TryWriteDirectTxPacket(uint32_t packetIndex,
                                                  Tx::IsochTxDescriptorSlab& slab,
                                                  const AudioInjectionPlan& plan) noexcept {
    ++debugInjectionAttempts_;
    uint8_t* payloadVirt = slab.PayloadPtr(packetIndex);
    const uint32_t payloadBytes = PacketPayloadByteCount(packetIndex, slab);
    if (!payloadVirt || payloadBytes <= Encoding::kCIPHeaderSize) {
        ++debugInjectionSkips_;
        if (debugInjectionAttempts_ <= 64 || (debugInjectionAttempts_ % 1000) == 0) {
            ASFW_LOG(Isoch,
                     "IT DBG INJECT skip=no_payload attempt=%llu pktIdx=%u payload=%u ptr=%p",
                     debugInjectionAttempts_, packetIndex, payloadBytes, payloadVirt);
        }
        counters_.directTxInvalidPackets.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (packetIndex >= producedPacketMetadata_.size()) {
        ++debugInjectionSkips_;
        ASFW_LOG(Isoch,
                 "IT DBG INJECT skip=packet_index_oob attempt=%llu pktIdx=%u payload=%u",
                 debugInjectionAttempts_, packetIndex, payloadBytes);
        counters_.directTxInvalidPackets.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto& metadata = producedPacketMetadata_[packetIndex];
    if (!metadata.valid || !metadata.isData || metadata.sizeBytes != payloadBytes) {
        ++debugInjectionSkips_;
        if (debugInjectionAttempts_ <= 64 || (debugInjectionAttempts_ % 1000) == 0) {
            ASFW_LOG(Isoch,
                     "IT DBG INJECT skip=metadata attempt=%llu pktIdx=%u valid=%d isData=%d metaSize=%u payload=%u metaSyt=0x%04x",
                     debugInjectionAttempts_,
                     packetIndex,
                     metadata.valid,
                     metadata.isData,
                     metadata.sizeBytes,
                     payloadBytes,
                     metadata.cip.syt);
        }
        counters_.directTxInvalidPackets.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const PacketCipFields cip = metadata.cip;
    const uint32_t am824Slots = metadata.am824Slots == 0 ? metadata.pcmChannels : metadata.am824Slots;
    const auto format = metadata.wireFormat;

    bool armed = IsDirectTxHardwarePathReady(plan);
    const char* fallbackSource = "silence_not_ready";
    if (!armed) {
        fallbackSource = (!directTxBinding_.enabled ||
                          directTxBinding_.outputBase == nullptr ||
                          directTxBinding_.control == nullptr ||
                          !directOutputReader_.IsBound())
            ? "silence_no_binding"
            : "silence_not_ready";
    }
    if (debugInjectionAttempts_ <= 64 || (debugInjectionAttempts_ % 1000) == 0) {
        ASFW_LOG(Isoch,
                 "IT DBG INJECT attempt=%llu pktIdx=%u armed0=%d payload=%u frames=%u ch=%u slots=%u sid=%u dbc=0x%02x syt=0x%04x bind(en=%d base=%p frames=%u ch=%u rate=%u control=%p bound=%d)",
                 debugInjectionAttempts_,
                 packetIndex,
                 armed,
                 payloadBytes,
                 metadata.framesPerPacket,
                 metadata.pcmChannels,
                 am824Slots,
                 cip.sid,
                 cip.dbc,
                 cip.syt,
                 directTxBinding_.enabled,
                 static_cast<const void*>(directTxBinding_.outputBase),
                 directTxBinding_.outputFrames,
                 directTxBinding_.outputChannels,
                 directTxBinding_.sampleRateHz,
                 static_cast<void*>(directTxBinding_.control),
                 directOutputReader_.IsBound());
    }
    if (armed) {
        if (!directCursorInitialized_ && !InitializeDirectOutputCursor(plan)) {
            if (debugInjectionAttempts_ <= 64 || (debugInjectionAttempts_ % 1000) == 0) {
                ASFW_LOG(Isoch,
                         "IT DBG INJECT source=silence_no_cursor attempt=%llu pktIdx=%u writtenEnd=%llu",
                         debugInjectionAttempts_,
                         packetIndex,
                         directOutputReader_.OutputWrittenEndFrame());
            }
            fallbackSource = "silence_no_cursor";
            armed = false;
        }
    }

    if (armed && directOutputFrameCursor_ >
        (std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(metadata.framesPerPacket))) {
        counters_.directTxInvalidPackets.fetch_add(1, std::memory_order_relaxed);
        fallbackSource = "silence_no_pcm";
        armed = false;
    }

    if (armed) {
        const Direct::Tx::TxAudioPacketWriteRequest request{
            .firstFrame = directOutputFrameCursor_,
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
        const Direct::Tx::TxAudioPacketWriteResult result =
            writer.WritePacket(request, payloadVirt, payloadBytes);
        
        if (result.readStatus == DirectTxReadStatus::kAvailable ||
            result.readStatus == DirectTxReadStatus::kUnderrun) {
            
            if (result.bytesWritten == payloadBytes && result.framesEncoded == metadata.framesPerPacket) {
                if (result.readStatus == DirectTxReadStatus::kUnderrun || result.usedSilence) {
                    counters_.directTxUnderrunSilencedPackets.fetch_add(1, std::memory_order_relaxed);
                }
                const uint64_t consumedEndFrame = directOutputFrameCursor_ + metadata.framesPerPacket;
                directOutputFrameCursor_ = consumedEndFrame;
                PublishDirectTxConsumedEndFrame(consumedEndFrame);
                counters_.directTxPackets.fetch_add(1, std::memory_order_relaxed);
                ++debugInjectionSuccesses_;
                if (debugInjectionAttempts_ <= 64 || (debugInjectionAttempts_ % 1000) == 0) {
                    const char* source = result.usedSilence ? "silence_no_pcm" : "host_pcm";
                    ASFW_LOG(Isoch,
                             "IT DBG INJECT source=%{public}s attempt=%llu success=%llu pktIdx=%u status=%u bytes=%u frames=%u consumedEnd=%llu usedSilence=%d",
                             source,
                             debugInjectionAttempts_,
                             debugInjectionSuccesses_,
                             packetIndex,
                             static_cast<uint32_t>(result.readStatus),
                             result.bytesWritten,
                             result.framesEncoded,
                             consumedEndFrame,
                             result.usedSilence);
                }
                return true;
            }
        }
        if (debugInjectionAttempts_ <= 64 || (debugInjectionAttempts_ % 1000) == 0) {
            ASFW_LOG(Isoch,
                     "IT DBG INJECT source=silence_no_pcm writer_failed attempt=%llu pktIdx=%u status=%u bytes=%u frames=%u payload=%u expectedFrames=%u",
                     debugInjectionAttempts_,
                     packetIndex,
                     static_cast<uint32_t>(result.readStatus),
                     result.bytesWritten,
                     result.framesEncoded,
                     payloadBytes,
                     metadata.framesPerPacket);
        }
        counters_.directTxInvalidPackets.fetch_add(1, std::memory_order_relaxed);
        fallbackSource = "silence_no_pcm";
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
            counters_.directTxUnderrunSilencedPackets.fetch_add(1, std::memory_order_relaxed);
            counters_.directTxPackets.fetch_add(1, std::memory_order_relaxed);
            ++debugInjectionSuccesses_;
            if (debugInjectionAttempts_ <= 64 || (debugInjectionAttempts_ % 1000) == 0) {
                ASFW_LOG(Isoch,
                         "IT DBG INJECT source=%{public}s attempt=%llu success=%llu pktIdx=%u bytes=%u frames=%u dbc=0x%02x syt=0x%04x armed=%d",
                         fallbackSource,
                         debugInjectionAttempts_,
                         debugInjectionSuccesses_,
                         packetIndex,
                         payloadBytes,
                         metadata.framesPerPacket,
                         cip.dbc,
                         cip.syt,
                         armed);
            }
            return true;
        }
    }

    ++debugInjectionSkips_;
    if (debugInjectionAttempts_ <= 64 || (debugInjectionAttempts_ % 1000) == 0) {
        ASFW_LOG(Isoch,
                 "IT DBG INJECT fallback_failed attempt=%llu skips=%llu pktIdx=%u",
                 debugInjectionAttempts_, debugInjectionSkips_, packetIndex);
    }
    return false;
}

void IsochAudioTxPipeline::InjectNearHw(uint32_t hwPacketIndex, Tx::IsochTxDescriptorSlab& slab) noexcept {
    auto plan = BuildAudioInjectionPlan(hwPacketIndex);
    if (debugInjectionAttempts_ < 64 || ((debugInjectionAttempts_ + 1) % 1000) == 0) {
        ASFW_LOG(Isoch,
                 "IT DBG PLAN hwPkt=%u writeIdx=%u target=%u todo=%u frames=%u ch=%u slots=%u bind(en=%d base=%p frames=%u ch=%u rate=%u control=%p bound=%d)",
                 hwPacketIndex,
                 audioWriteIndex_,
                 plan.audioTarget,
                 plan.packetsToInject,
                 plan.framesPerPacket,
                 plan.pcmChannels,
                 plan.am824Slots,
                 directTxBinding_.enabled,
                 static_cast<const void*>(directTxBinding_.outputBase),
                 directTxBinding_.outputFrames,
                 directTxBinding_.outputChannels,
                 directTxBinding_.sampleRateHz,
                 static_cast<void*>(directTxBinding_.control),
                 directOutputReader_.IsBound());
    }
    if (plan.packetsToInject == 0) {
        return;
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
