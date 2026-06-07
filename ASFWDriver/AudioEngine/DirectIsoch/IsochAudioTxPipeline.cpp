// IsochAudioTxPipeline.cpp
// ASFW - Audio semantics layer for IT transmit (Direct-Only implementation).

#include "IsochAudioTxPipeline.hpp"

#include "../../AudioWire/AMDTP/AmdtpRateGeometry.hpp"
#include "../../AudioWire/AMDTP/TimingUtils.hpp"
#include "../Direct/Tx/DirectTxPacketEncoder.hpp"
#include "../../Logging/LogConfig.hpp"

#include <AudioDriverKit/AudioDriverKit.h>
#include <algorithm>
#include <limits>

namespace ASFW::Isoch {

namespace Direct = ASFW::AudioEngine::Direct;
using DirectTxReadStatus = ASFW::AudioEngine::Direct::Tx::DirectTxReadStatus;

extern "C" __attribute__((noinline, optnone, used))
void ASFWDebugTxFrameMatrixBreakpoint(const int32_t* sourceMatrix,
                                      const uint32_t* encodedDestinationMatrix,
                                      uint64_t packetFirstFrame,
                                      uint32_t frames,
                                      uint32_t sourceChannels,
                                      uint32_t destinationSlots) noexcept {
    (void)sourceMatrix;
    (void)encodedDestinationMatrix;
    (void)packetFirstFrame;
    (void)frames;
    (void)sourceChannels;
    (void)destinationSlots;
    __asm__ __volatile__("" ::: "memory");
}

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

constexpr uint64_t kTxFrameMatrixLogIntervalNs = 1'000'000'000ULL;
constexpr uint32_t kTxFrameMatrixChunkSlots = 8;
constexpr uint32_t kTxFrameMatrixMaxFrames = Encoding::kSamplesPerPacket48k;
constexpr uint32_t kTxFrameMatrixMaxSlots = 16;

void LogTxFrameChunk(uint64_t packetFirstFrame,
                     uint32_t frameInPacket,
                     uint32_t slotBase,
                     uint32_t slotCount,
                     const int32_t* srcFrame,
                     uint32_t sourceChannels,
                     const uint32_t* encodedFrame,
                     uint32_t encodedSlots) noexcept {
    const auto srcAt = [&](uint32_t offset) noexcept -> uint32_t {
        const uint32_t channel = slotBase + offset;
        return srcFrame && channel < sourceChannels
            ? static_cast<uint32_t>(srcFrame[channel])
            : 0;
    };
    const auto dstAt = [&](uint32_t offset) noexcept -> uint32_t {
        const uint32_t slot = slotBase + offset;
        return encodedFrame && slot < encodedSlots ? encodedFrame[slot] : 0;
    };

    ASFW_LOG(
        Audio,
        "TX FRAME SRC_RING packetFirst=%llu frame=%llu row=%u slots=[%u,%u) srcNative=[%08x %08x %08x %08x %08x %08x %08x %08x] dstEncodedHost=[%08x %08x %08x %08x %08x %08x %08x %08x]",
        packetFirstFrame,
        packetFirstFrame + frameInPacket,
        frameInPacket,
        slotBase,
        slotBase + slotCount,
        srcAt(0), srcAt(1), srcAt(2), srcAt(3),
        srcAt(4), srcAt(5), srcAt(6), srcAt(7),
        dstAt(0), dstAt(1), dstAt(2), dstAt(3),
        dstAt(4), dstAt(5), dstAt(6), dstAt(7));
}

} // namespace

void IsochAudioTxPipeline::SetExternalSyncBridge(ASFW::AudioEngine::DirectIsoch::ExternalSyncBridge* bridge) noexcept {
    externalSyncBridge_ = bridge;
    txPhaseLoop_.Reset();
    txPhaseReadIndexSeeded_ = false;
}

uint64_t IsochAudioTxPipeline::SeedTxTimeline(uint64_t oldestValid) noexcept {
    const uint32_t fpp = txPcmPacketRing_.framesPerPacket;
    if (fpp == 0) {
        return 0;
    }

    const uint64_t base = oldestValid - (oldestValid % fpp);
    txOutputOffsetFrames_ = (kTxOutputOffsetFrames / fpp) * fpp;
    const uint64_t timelineBase = base + txOutputOffsetFrames_;

    txPcmPacketRing_.baseFrame = base;
    txScheduledSampleFrame_ = timelineBase;
    txCompletedSampleFrame_ = timelineBase;
    lastPopulatedWriteEnd_ = oldestValid;

    for (uint32_t s = 0; s < txPcmPacketRing_.packetSlots; ++s) {
        txPcmPacketRing_.slotStamps[s] = {};
    }

    const uint32_t capacity = txPcmPacketRing_.outputFrameCapacity;
    const uint32_t slots = txPcmPacketRing_.am824Slots;
    const auto format = assembler_.audioWireFormat();
    if (capacity > 0 && slots > 0) {
        for (uint32_t f = 0; f < capacity; ++f) {
            uint32_t* destPtr = txPcmPacketRing_.words + (f * slots);
            Direct::Tx::EncodeDirectTxSilenceFrame(assembler_.channelCount(), slots, format, destPtr);
        }
        for (uint32_t f = 0; f < fpp; ++f) {
            uint32_t* destPtr = txPcmPacketRing_.silenceWords + (f * slots);
            Direct::Tx::EncodeDirectTxSilenceFrame(assembler_.channelCount(), slots, format, destPtr);
        }
    }

    if (directTxBinding_.control) {
        directTxBinding_.control->txScheduledSampleFrame.store(
            timelineBase, std::memory_order_release);
        directTxBinding_.control->txCompletedSampleFrame.store(
            timelineBase, std::memory_order_release);
    }

    return timelineBase;
}

void IsochAudioTxPipeline::SetDirectTxRuntimeBinding(const DirectTxRuntimeBinding& binding) noexcept {
    directTxBinding_ = binding;
    lastDiscontinuityGeneration_ = 0;
    lastTxFrameMatrixLogNs_ = 0;

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

    if (binding.enabled && directOutputReader_.IsBound()) {
        const uint64_t oldestValid = directOutputReader_.OutputOldestValidFrame();
        const uint32_t framesPerPacket = assembler_.samplesPerDataPacket();
        
        const bool geometryOk =
            binding.outputFrames != 0 &&
            framesPerPacket != 0 &&
            (binding.outputFrames % framesPerPacket) == 0 &&
            binding.outputFrames <= TxPcmPacketRing::kMaxOutputFrames &&
            assembler_.am824SlotCount() != 0 &&
            assembler_.am824SlotCount() <= TxPcmPacketRing::kMaxChannels;

        if (!geometryOk) {
            txPcmPacketRing_ = {};
            lastPopulatedWriteEnd_ = 0;
            ASFW_LOG(Isoch, "IT: SetDirectTxRuntimeBinding ERROR - invalid geometry or bounds exceeded: frames=%u max=%u channels=%u max=%u fpp=%u",
                     binding.outputFrames, TxPcmPacketRing::kMaxOutputFrames,
                     assembler_.am824SlotCount(), TxPcmPacketRing::kMaxChannels, framesPerPacket);
        } else {
            txPcmPacketRing_.outputFrameCapacity = binding.outputFrames;
            txPcmPacketRing_.framesPerPacket = framesPerPacket;
            txPcmPacketRing_.packetSlots = binding.outputFrames / framesPerPacket;
            txPcmPacketRing_.packetBase = 0;
            txPcmPacketRing_.am824Slots = assembler_.am824SlotCount();
            txPcmPacketRing_.valid = true;

            SeedTxTimeline(oldestValid);
        }
    } else {
        txPcmPacketRing_ = {};
        lastPopulatedWriteEnd_ = 0;
    }

    ASFW_LOG(Isoch,
             "IT: DIRECT-TX binding %{public}s base=%p frames=%u ch=%u slots=%u rate=%u mode=%u offset=%u bound=%{public}s",
             binding.enabled ? "set(enabled)" : "set(disabled)",
             static_cast<const void*>(binding.outputBase),
             binding.outputFrames, binding.outputChannels, binding.am824Slots,
             binding.sampleRateHz, binding.streamModeRaw, txOutputOffsetFrames_,
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

    constexpr uint32_t kEnabledSampleRateHz = 48000;
    const auto geometry = Encoding::AmdtpRateGeometryForSampleRate(kEnabledSampleRateHz);
    if (!geometry ||
        assembler_.samplesPerDataPacket() != geometry->sytIntervalFrames ||
        timingPolicy_.PreparationDeadlineFrames(
            ASFW::Audio::AudioDirection::Output) !=
            geometry->nominalFramesPerCycle *
                Tx::Layout::kPreparationDeadlinePackets) {
        ASFW_LOG(Isoch, "IT: Configure failed - invalid AMDTP 48k geometry");
        return kIOReturnInternalError;
    }

    sytGenerator_.reset();
    sytGenerator_.initialize(static_cast<double>(geometry->sampleRateHz));

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
    previousPacketMetadata_ = {};
    debugProducedPackets_ = 0;
    txCompletedSampleFrame_ = 0;
    txScheduledSampleFrame_ = 0;
    lastDiscontinuityGeneration_ = 0;
    lastTxFrameMatrixLogNs_ = 0;
    txEventGroupCount_ = 0;
    txPhaseLoop_.Reset();
    txPhaseReadIndexSeeded_ = false;

    if (directOutputReader_.IsBound()) {
        const uint64_t oldestValid = directOutputReader_.OutputOldestValidFrame();
        const uint32_t fpp = assembler_.samplesPerDataPacket();
        if (fpp > 0 && txPcmPacketRing_.valid) {
            const uint64_t timelineBase = SeedTxTimeline(oldestValid);
            ASFW_LOG(Isoch,
                     "IT: ResetForStart seed - oldestValid=%llu audioBase=%llu timelineBase=%llu offset=%u",
                     oldestValid, txPcmPacketRing_.baseFrame, timelineBase, txOutputOffsetFrames_);
        }
    } else if (directTxBinding_.control) {
        directTxBinding_.control->txScheduledSampleFrame.store(0, std::memory_order_release);
        directTxBinding_.control->txCompletedSampleFrame.store(0, std::memory_order_release);
    }
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

    ProducedPacketMetadata metadata{
        .valid = true,
        .isData = pkt.isData,
        .sizeBytes = pkt.size,
        .framesPerPacket = pkt.isData ? assembler_.samplesPerDataPacket() : 0,
        .pcmChannels = assembler_.channelCount(),
        .am824Slots = assembler_.am824SlotCount(),
        .packetIndex = request.packetIndex,
        .generation = request.slotGeneration,
        .wireFormat = assembler_.audioWireFormat(),
        .timelineFirstFrame = pkt.isData ? txScheduledSampleFrame_ : 0,
        .cip = PacketCipFields{
            .sid = sid_,
            .dbc = pkt.dbc,
            .syt = pkt.isData ? syt : Encoding::SYTGenerator::kNoInfo,
        },
    };
    if (request.packetIndex < producedPacketMetadata_.size()) {
        previousPacketMetadata_[request.packetIndex] =
            producedPacketMetadata_[request.packetIndex];
        producedPacketMetadata_[request.packetIndex] = metadata;
    }

    Tx::IsochTxPacket out{};
    std::memcpy(silentPacketStorage_.data(), pkt.data, pkt.size);
    if (pkt.isData) {
        if (txScheduledSampleFrame_ >
            (std::numeric_limits<uint64_t>::max() - metadata.framesPerPacket)) {
            counters_.directTxTimelineInvariantFailures.fetch_add(1, std::memory_order_relaxed);
            if (directTxBinding_.control) {
                directTxBinding_.control->counters.txTimelineInvariantFailures.fetch_add(
                    1, std::memory_order_relaxed);
            }
        } else {
            txScheduledSampleFrame_ += metadata.framesPerPacket;
            if (directTxBinding_.control) {
                directTxBinding_.control->txScheduledSampleFrame.store(
                    txScheduledSampleFrame_, std::memory_order_release);
            }
        }
    }
    out.words = reinterpret_cast<const uint32_t*>(silentPacketStorage_.data());
    out.sizeBytes = pkt.size;
    out.isData = pkt.isData;
    out.dbc = pkt.dbc;
    out.syt = pkt.isData ? syt : Encoding::SYTGenerator::kNoInfo;
    out.framesPerPacket = metadata.framesPerPacket;
    out.timelineFirstFrame = metadata.timelineFirstFrame;
    if (directTxBinding_.control) {
        auto& counters = directTxBinding_.control->counters;
        counters.txPackets.fetch_add(1, std::memory_order_relaxed);
        if (pkt.isData) {
            counters.txDataPackets.fetch_add(1, std::memory_order_relaxed);
            if (syt == Encoding::SYTGenerator::kNoInfo) {
                counters.txSytFfffPackets.fetch_add(1, std::memory_order_relaxed);
            } else {
                counters.txValidSytPackets.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            counters.txNoDataPackets.fetch_add(1, std::memory_order_relaxed);
        }
    }

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

bool IsochAudioTxPipeline::OnTransmitSlotCompleted(
    const Tx::CompletedTxSlot& completed) noexcept {
    const auto& metadata = completed.metadata;
    if (!metadata.valid) {
        return true;
    }

    auto* control = directTxBinding_.control;
    if (metadata.state == Tx::PreparedTxSlotState::PcmPrepared &&
        !completed.payloadHashMatches) {
        if (control) {
            control->counters.txCompletedPayloadHashMismatches.fetch_add(
                1, std::memory_order_relaxed);
        }
        const Tx::PreparedTxPayloadRequest request{
            .packetIndex = completed.packetIndex,
            .hwPacketIndex = completed.hwPacketIndex,
            .distanceToHardware = 0,
            .writable = false,
            .deadline = true,
            .hardwareOwned = true,
            .metadata = metadata,
        };
        PublishFatalFault(
            ASFW::Audio::Runtime::FatalStreamReason::TxPayloadMismatch,
            request,
            metadata.sourceFirstFrame,
            metadata.sourceEndFrame,
            directOutputReader_.OutputOldestValidFrame(),
            directOutputReader_.OutputWrittenEndFrame(),
            metadata.preparedPayloadHash,
            completed.completedPayloadHash);
        return false;
    }

    if (!metadata.isData) {
        return true;
    }

    if (metadata.state == Tx::PreparedTxSlotState::PcmPrepared && control) {
        control->counters.txCompletedPayloadHashMatches.fetch_add(
            1, std::memory_order_relaxed);
        control->counters.txCompletedPcmSlots.fetch_add(
            1, std::memory_order_relaxed);
        if (ASFW::LogConfig::Shared().IsIsochTxVerifierEnabled()) {
            ASFW_LOG_RL(
                Isoch, "tx/completed_payload", 1000, OS_LOG_TYPE_DEFAULT,
                "IT TX COMPLETE pkt=%u hwPkt=%u gen=%llu prepDistance=%u dbc=0x%02x syt=0x%04x timeline=%llu source=[%llu,%llu) src=[%08x %08x] wire=[%08x %08x] hash=0x%016llx/0x%016llx",
                completed.packetIndex,
                completed.hwPacketIndex,
                metadata.generation,
                metadata.preparationDistance,
                metadata.dbc,
                metadata.syt,
                metadata.timelineFirstFrame,
                metadata.sourceFirstFrame,
                metadata.sourceEndFrame,
                static_cast<uint32_t>(metadata.firstSourceSamples[0]),
                static_cast<uint32_t>(metadata.firstSourceSamples[1]),
                metadata.firstEncodedWords[0],
                metadata.firstEncodedWords[1],
                metadata.preparedPayloadHash,
                completed.completedPayloadHash);
        }
    } else if (control) {
        control->counters.txCompletedStartupSilenceSlots.fetch_add(
            1, std::memory_order_relaxed);
    }

    txCompletedSampleFrame_ += metadata.framesPerPacket;
    if (control) {
        auto& counters = control->counters;
        if (metadata.state != Tx::PreparedTxSlotState::PcmPrepared) {
            counters.txSilenceSubstitutions.fetch_add(1, std::memory_order_relaxed);
            if (metadata.syt == Encoding::SYTGenerator::kNoInfo) {
                counters.txNoPhaseSilencePackets.fetch_add(
                    1, std::memory_order_relaxed);
            } else {
                counters.txValidPhaseSilencePackets.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }

        control->txCompletedSampleFrame.store(
            txCompletedSampleFrame_, std::memory_order_release);

        // Calculate consumed audio frame from completed timeline frame
        const uint64_t consumedAudioFrame =
            txCompletedSampleFrame_ >= txOutputOffsetFrames_
                ? txCompletedSampleFrame_ - txOutputOffsetFrames_
                : 0;
        PublishDirectTxConsumedEndFrame(consumedAudioFrame);
    }

    constexpr uint64_t kMaxQueuedDataFrames =
        static_cast<uint64_t>(Tx::Layout::kNumPackets) * Encoding::kSamplesPerPacket48k;
    if (txCompletedSampleFrame_ > txScheduledSampleFrame_ ||
        (txScheduledSampleFrame_ - txCompletedSampleFrame_) > kMaxQueuedDataFrames) {
        counters_.directTxTimelineInvariantFailures.fetch_add(1, std::memory_order_relaxed);
        if (directTxBinding_.control) {
            directTxBinding_.control->counters.txTimelineInvariantFailures.fetch_add(
                1, std::memory_order_relaxed);
        }
        ASFW_LOG_RL(Isoch, "tx/timeline_invariant", 1000, OS_LOG_TYPE_ERROR,
                    "IT TX timeline invariant scheduled=%llu completed=%llu queued=%llu max=%llu",
                    txScheduledSampleFrame_,
                    txCompletedSampleFrame_,
                    txScheduledSampleFrame_ >= txCompletedSampleFrame_
                        ? txScheduledSampleFrame_ - txCompletedSampleFrame_ : 0,
                    kMaxQueuedDataFrames);
    }
    return true;
}

void IsochAudioTxPipeline::OnIsochEventGroup(const Core::IsochEventGroup& group) noexcept {
    if (group.direction != Core::IsochEventDirection::kTransmit ||
        group.completedPacketCount == 0) {
        return;
    }

    if (group.hostTicks == 0) {
        return;
    }

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
    const uint32_t oldProjectedCycle =
        (completedCycle + packetsAhead) % ASFW::Timing::kCyclesPerSecond;

    uint32_t seedProjectedCycle = oldProjectedCycle;
    const bool phaseWasValid = txPhaseLoop_.PhaseValid();
    if (!phaseWasValid) {
        seedProjectedCycle =
            (oldProjectedCycle + group.refillPacketCount) % ASFW::Timing::kCyclesPerSecond;
    }

    if (txEventGroupCount_ < 8) {
        ASFW_LOG(Isoch,
                 "PHASE seed: phaseWasValid=%d completedCycle=%u packetsAhead=%u refillPacketCount=%u oldProjectedCycle=%u seedProjectedCycle=%u",
                 phaseWasValid ? 1 : 0,
                 completedCycle,
                 packetsAhead,
                 group.refillPacketCount,
                 oldProjectedCycle,
                 seedProjectedCycle);
    }

    const int64_t projectedOffsetTicks = ASFW::Timing::tstampToOffsets(0, seedProjectedCycle, 0);
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
    (void)metadata;
    const uint64_t writtenEnd = directOutputReader_.OutputWrittenEndFrame();
    if (writtenEnd == 0) {
        return false;
    }

    const uint64_t kTargetLead =
        timingPolicy_.PacketLeadFrames(ASFW::Audio::AudioDirection::Output);
    const uint64_t target = (writtenEnd > kTargetLead) ? (writtenEnd - kTargetLead) : 0;
    directOutputFrameCursor_ = std::max(target, directOutputReader_.OutputOldestValidFrame());
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

    ++debugTryBuildCount_;

    auto logAndReturn = [&](bool succ, const char* dec, ASFW::Audio::Runtime::TxPacketState resState,
                            ASFW::Audio::Runtime::TxBlockingResult resBlocking, uint32_t resFrames) noexcept -> bool {
        if (ASFW::LogConfig::Shared().IsIsochTxVerifierEnabled()) {
            const uint64_t playWr = directOutputReader_.OutputWrittenEndFrame();
            const uint64_t oldest = directOutputReader_.OutputOldestValidFrame();
            const uint64_t playRd = directTxBinding_.control ? directTxBinding_.control->playbackRingReadFrame.load(std::memory_order_relaxed) : 0;
            const uint64_t currentCursor = directOutputFrameCursor_;
            const uint64_t avail = (playWr > currentCursor) ? (playWr - currentCursor) : 0;
            ASFW_LOG_RL(Isoch, "tx/ring_decision", 1000, OS_LOG_TYPE_DEFAULT,
                     "TXRING pkt=%u dbc=0x%02x syt=0x%04x scheduled=%llu completed=%llu source=%llu oldest=%llu written=%llu decision=%{public}s state=%u blocking=%u frames=%u playRd=%llu avail=%llu",
                     metadata.packetIndex,
                     cip.dbc,
                     cip.syt,
                     metadata.timelineFirstFrame,
                     txCompletedSampleFrame_,
                     currentCursor,
                     oldest,
                     playWr,
                     dec,
                     static_cast<uint32_t>(resState),
                     static_cast<uint32_t>(resBlocking),
                     resFrames,
                     playRd,
                     avail);
        }
        return succ;
    };

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
        return logAndReturn(writeSilence(ASFW::Audio::Runtime::TxPacketState::NoPhaseSilence),
                            "no_phase",
                            ASFW::Audio::Runtime::TxPacketState::NoPhaseSilence,
                            ASFW::Audio::Runtime::TxBlockingResult::NoData,
                            0);
    }

    if (!IsPlaybackRingPathReady(metadata)) {
        return logAndReturn(writeSilence(ASFW::Audio::Runtime::TxPacketState::UnderrunSilence),
                            "not_ready",
                            ASFW::Audio::Runtime::TxPacketState::UnderrunSilence,
                            ASFW::Audio::Runtime::TxBlockingResult::NoData,
                            0);
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
            return logAndReturn(writeSilence(ASFW::Audio::Runtime::TxPacketState::StaleSync),
                                "stale_sync",
                                ASFW::Audio::Runtime::TxPacketState::StaleSync,
                                ASFW::Audio::Runtime::TxBlockingResult::NoData,
                                0);
        }
    }

    const uint64_t writtenEnd = directOutputReader_.OutputWrittenEndFrame();
    const uint64_t oldestValid = directOutputReader_.OutputOldestValidFrame();
    const uint64_t discontinuityGeneration = directTxBinding_.control
        ? directTxBinding_.control->playbackRingDiscontinuityGeneration.load(
              std::memory_order_acquire)
        : 0;

    uint64_t sourceFrame =
        timingPolicy_.HardwareOutputFrameToReportedFrame(metadata.timelineFirstFrame);

    auto applyForwardCorrection = [&](uint64_t correctedSource) noexcept {
        if (correctedSource <= sourceFrame) {
            return;
        }
        const uint64_t delta = correctedSource - sourceFrame;
        if (sourceTimelineOffsetFrames_ <=
            (std::numeric_limits<uint64_t>::max() - delta)) {
            sourceTimelineOffsetFrames_ += delta;
            sourceFrame = correctedSource;
            counters_.directTxCursorResyncs.fetch_add(1, std::memory_order_relaxed);
            if (directTxBinding_.control) {
                directTxBinding_.control->counters.txForwardCursorCorrections.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
    };

    if (sourceTimelineOffsetFrames_ >
        (std::numeric_limits<uint64_t>::max() - sourceFrame)) {
        counters_.directTxTimelineInvariantFailures.fetch_add(1, std::memory_order_relaxed);
        if (directTxBinding_.control) {
            directTxBinding_.control->counters.txTimelineInvariantFailures.fetch_add(
                1, std::memory_order_relaxed);
        }
        return logAndReturn(false,
                            "source_overflow",
                            ASFW::Audio::Runtime::TxPacketState::InvalidGeometry,
                            ASFW::Audio::Runtime::TxBlockingResult::NoData,
                            0);
    }
    sourceFrame += sourceTimelineOffsetFrames_;

    if (!sourceTimelineAnchored_ && writtenEnd != 0) {
        applyForwardCorrection(oldestValid);
        sourceTimelineAnchored_ = true;
        lastDiscontinuityGeneration_ = discontinuityGeneration;
    } else if (sourceTimelineAnchored_ &&
               discontinuityGeneration != lastDiscontinuityGeneration_) {
        if (sourceFrame < oldestValid) {
            applyForwardCorrection(oldestValid);
        } else if (sourceFrame > oldestValid && directTxBinding_.control) {
            directTxBinding_.control->counters.txPreventedBackwardCorrections.fetch_add(
                1, std::memory_order_relaxed);
        }
        lastDiscontinuityGeneration_ = discontinuityGeneration;
    }

    if (directCursorInitialized_ && sourceFrame < lastSourceFrame_) {
        sourceFrame = lastSourceFrame_;
        counters_.directTxTimelineInvariantFailures.fetch_add(1, std::memory_order_relaxed);
        if (directTxBinding_.control) {
            directTxBinding_.control->counters.txTimelineInvariantFailures.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    lastSourceFrame_ = sourceFrame;
    directOutputFrameCursor_ = sourceFrame;
    directCursorInitialized_ = true;

    if (directOutputFrameCursor_ >
        (std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(metadata.framesPerPacket))) {
        counters_.directTxInvalidPackets.fetch_add(1, std::memory_order_relaxed);
        return logAndReturn(false,
                            "cursor_overflow",
                            ASFW::Audio::Runtime::TxPacketState::ValidPhaseSilence,
                            ASFW::Audio::Runtime::TxBlockingResult::NoData,
                            0);
    }

    const uint64_t readFrame = sourceFrame;
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
    {
        const ASFW::Audio::Runtime::TxPacketProductionResult result =
            writer.WritePacket(request, packetBytes, packetCapacityBytes);
        const auto sourceReadStatus =
            static_cast<DirectTxReadStatus>(result.sourceReadStatus);

        if (result.fatal) {
            counters_.directTxInvalidPackets.fetch_add(1, std::memory_order_relaxed);
            if (directTxBinding_.control) {
                directTxBinding_.control->counters.txInvalidGeometryPackets.fetch_add(1, std::memory_order_relaxed);
                directTxBinding_.control->fatalReason.store(ASFW::Audio::Runtime::FatalStreamReason::InvalidGeometry,
                                                            std::memory_order_release);
                directTxBinding_.control->fatalGeneration.fetch_add(1, std::memory_order_release);
            }
            return logAndReturn(false,
                                "writer_fatal",
                                result.state,
                                result.blockingResult,
                                result.frames);
        }

        if (result.frames != metadata.framesPerPacket) {
            return logAndReturn(writeSilence(ASFW::Audio::Runtime::TxPacketState::UnderrunSilence),
                                "writer_underrun",
                                result.state,
                                result.blockingResult,
                                result.frames);
        }

        if (directTxBinding_.control) {
            auto& counters = directTxBinding_.control->counters;
            if (sourceReadStatus == DirectTxReadStatus::kStaleOverwritten) {
                counters.txStaleOverwrittenReads.fetch_add(1, std::memory_order_relaxed);
            } else if (sourceReadStatus == DirectTxReadStatus::kUnderrun) {
                counters.txProducerAheadUnderruns.fetch_add(1, std::memory_order_relaxed);
            }

            if (sourceReadStatus == DirectTxReadStatus::kAvailable) {
                bool anyNonzero = false;
                for (uint32_t frame = 0; frame < metadata.framesPerPacket && !anyNonzero; ++frame) {
                    const int32_t* frameIn = directOutputReader_.Frame(readFrame + frame);
                    if (!frameIn) {
                        break;
                    }
                    for (uint32_t ch = 0; ch < metadata.pcmChannels; ++ch) {
                        if (frameIn[ch] != 0) {
                            anyNonzero = true;
                            break;
                        }
                    }
                }
                if (anyNonzero) {
                    counters.txPcmNonzeroPackets.fetch_add(1, std::memory_order_relaxed);
                } else {
                    counters.txPcmAllZeroPackets.fetch_add(1, std::memory_order_relaxed);
                }
            }

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
        if (ASFW::LogConfig::Shared().IsIsochTxVerifierEnabled()) {
            const int32_t* source = directOutputReader_.Frame(readFrame);
            const uint32_t* encoded = Direct::Tx::DirectTxPacketPayloadQuadlets(packetBytes);
            ASFW_LOG_RL(
                Isoch, "tx/source_timeline", 1000, OS_LOG_TYPE_DEFAULT,
                "IT TX SOURCE pkt=%u dbc=0x%02x syt=0x%04x scheduled=%llu completed=%llu source=%llu oldest=%llu written=%llu decision=%u src=[%08x %08x] wire=[%08x %08x]",
                metadata.packetIndex,
                metadata.cip.dbc,
                metadata.cip.syt,
                metadata.timelineFirstFrame,
                txCompletedSampleFrame_,
                readFrame,
                oldestValid,
                writtenEnd,
                static_cast<uint32_t>(sourceReadStatus),
                source ? static_cast<uint32_t>(source[0]) : 0,
                source && metadata.pcmChannels > 1 ? static_cast<uint32_t>(source[1]) : 0,
                encoded ? encoded[0] : 0,
                encoded && metadata.am824Slots > 1 ? encoded[1] : 0);
        }
        counters_.directTxPackets.fetch_add(1, std::memory_order_relaxed);
        return logAndReturn(true,
                            "success",
                            result.state,
                            result.blockingResult,
                            result.frames);
    }
}

bool IsochAudioTxPipeline::HasFatalFault() const noexcept {
    return directTxBinding_.control &&
           directTxBinding_.control->fatalReason.load(std::memory_order_acquire) !=
               ASFW::Audio::Runtime::FatalStreamReason::None;
}

void IsochAudioTxPipeline::RecordImmediateStop() noexcept {
    if (directTxBinding_.control) {
        directTxBinding_.control->counters.txImmediateStops.fetch_add(
            1, std::memory_order_relaxed);
    }
}

uint64_t IsochAudioTxPipeline::PreparationRequestGeneration() const noexcept {
    return directTxBinding_.control
        ? directTxBinding_.control->txPreparationRequests.RequestedGeneration()
        : 0;
}

uint64_t IsochAudioTxPipeline::PreparationHandledGeneration() const noexcept {
    return directTxBinding_.control
        ? directTxBinding_.control->txPreparationRequests.handledGeneration.load(
              std::memory_order_acquire)
        : 0;
}

void IsochAudioTxPipeline::MarkPreparationRequestHandled(
    const uint64_t generation,
    const uint64_t hostTicks) noexcept {
    if (!directTxBinding_.control) {
        return;
    }
    const uint64_t requestTicks =
        directTxBinding_.control->txPreparationRequests.requestHostTicks.load(
            std::memory_order_relaxed);
    const uint64_t latency =
        hostTicks >= requestTicks ? hostTicks - requestTicks : 0;
    directTxBinding_.control->txLastPreparationLatencyTicks.store(
        latency, std::memory_order_relaxed);
    uint64_t previousMax =
        directTxBinding_.control->txMaxPreparationLatencyTicks.load(
            std::memory_order_relaxed);
    while (latency > previousMax &&
           !directTxBinding_.control->txMaxPreparationLatencyTicks
                .compare_exchange_weak(
                    previousMax,
                    latency,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
    }
    directTxBinding_.control->txPreparationRequests.MarkHandled(generation, hostTicks);
    directTxBinding_.control->counters.txPreparationDrainPasses.fetch_add(
        1, std::memory_order_relaxed);
}

void IsochAudioTxPipeline::PublishFatalFault(
    const ASFW::Audio::Runtime::FatalStreamReason reason,
    const Tx::PreparedTxPayloadRequest& request,
    const uint64_t sourceFirstFrame,
    const uint64_t sourceEndFrame,
    const uint64_t oldestValidFrame,
    const uint64_t writtenEndFrame,
    const uint64_t preparedPayloadHash,
    const uint64_t completedPayloadHash) noexcept {
    auto* control = directTxBinding_.control;
    if (!control ||
        control->fatalReason.load(std::memory_order_acquire) !=
            ASFW::Audio::Runtime::FatalStreamReason::None) {
        return;
    }

    auto& snapshot = control->txFatalSnapshot;
    snapshot.packetGeneration.store(request.metadata.generation, std::memory_order_relaxed);
    snapshot.timelineFirstFrame.store(request.metadata.timelineFirstFrame,
                                      std::memory_order_relaxed);
    snapshot.sourceFirstFrame.store(sourceFirstFrame, std::memory_order_relaxed);
    snapshot.sourceEndFrame.store(sourceEndFrame, std::memory_order_relaxed);
    snapshot.oldestValidFrame.store(oldestValidFrame, std::memory_order_relaxed);
    snapshot.writtenEndFrame.store(writtenEndFrame, std::memory_order_relaxed);
    snapshot.packetIndex.store(request.packetIndex, std::memory_order_relaxed);
    snapshot.distanceToHardware.store(request.distanceToHardware,
                                      std::memory_order_relaxed);
    snapshot.slotState.store(static_cast<uint32_t>(request.metadata.state),
                             std::memory_order_relaxed);
    snapshot.dbc.store(request.metadata.dbc, std::memory_order_relaxed);
    snapshot.syt.store(request.metadata.syt, std::memory_order_relaxed);
    snapshot.preparedPayloadHash.store(preparedPayloadHash, std::memory_order_relaxed);
    snapshot.completedPayloadHash.store(completedPayloadHash, std::memory_order_relaxed);

    ASFW::Audio::Runtime::FatalStreamReason expected =
        ASFW::Audio::Runtime::FatalStreamReason::None;
    if (!control->fatalReason.compare_exchange_strong(
            expected, reason, std::memory_order_release, std::memory_order_relaxed)) {
        return;
    }
    control->fatalGeneration.fetch_add(1, std::memory_order_release);
    switch (reason) {
    case ASFW::Audio::Runtime::FatalStreamReason::TxReadAhead:
        control->counters.txReadAheadFaults.fetch_add(1, std::memory_order_relaxed);
        break;
    case ASFW::Audio::Runtime::FatalStreamReason::TxSourceOverwritten:
        control->counters.txSourceOverwrittenFaults.fetch_add(1, std::memory_order_relaxed);
        break;
    case ASFW::Audio::Runtime::FatalStreamReason::TxPreparationMissedDeadline:
        control->counters.txPreparationDeadlineFaults.fetch_add(1, std::memory_order_relaxed);
        break;
    case ASFW::Audio::Runtime::FatalStreamReason::TxSlotInvariant:
        control->counters.txSlotOwnershipFaults.fetch_add(1, std::memory_order_relaxed);
        break;
    case ASFW::Audio::Runtime::FatalStreamReason::TxPayloadMismatch:
        control->counters.txPayloadMismatchFaults.fetch_add(
            1, std::memory_order_relaxed);
        break;
    default:
        break;
    }

    ASFW_LOG(Audio,
             "ADK FATAL TX PREP reason=%u pkt=%u gen=%llu hwPkt=%u distance=%u writable=%d deadline=%d state=%u dbc=0x%02x syt=0x%04x timeline=%llu source=[%llu,%llu) valid=[%llu,%llu) src=[%08x %08x] wire=[%08x %08x] hash=0x%016llx/0x%016llx",
             static_cast<uint32_t>(reason),
             request.packetIndex,
             request.metadata.generation,
             request.hwPacketIndex,
             request.distanceToHardware,
             request.writable,
             request.deadline,
             static_cast<uint32_t>(request.metadata.state),
             request.metadata.dbc,
             request.metadata.syt,
             request.metadata.timelineFirstFrame,
             sourceFirstFrame,
             sourceEndFrame,
             oldestValidFrame,
             writtenEndFrame,
             static_cast<uint32_t>(request.metadata.firstSourceSamples[0]),
             static_cast<uint32_t>(request.metadata.firstSourceSamples[1]),
             request.metadata.firstEncodedWords[0],
             request.metadata.firstEncodedWords[1],
             preparedPayloadHash,
             completedPayloadHash);
}

Tx::PreparedTxPayloadResult IsochAudioTxPipeline::PreparePayload(
    const Tx::PreparedTxPayloadRequest& request) noexcept {
    Tx::PreparedTxPayloadResult out{};

    if (request.packetIndex >= producedPacketMetadata_.size()) {
        PublishFatalFault(ASFW::Audio::Runtime::FatalStreamReason::TxSlotInvariant,
                          request, 0, 0, 0, 0);
        out.action = Tx::PreparedTxAction::Fatal;
        return out;
    }

    if (!directTxBinding_.control) {
        return out;
    }

    auto& metadata = producedPacketMetadata_[request.packetIndex];
    if (!metadata.valid || !metadata.isData ||
        metadata.generation != request.metadata.generation ||
        metadata.timelineFirstFrame != request.metadata.timelineFirstFrame ||
        metadata.framesPerPacket != request.metadata.framesPerPacket) {
        PublishFatalFault(ASFW::Audio::Runtime::FatalStreamReason::TxSlotInvariant,
                          request, 0, 0, 0, 0);
        out.action = Tx::PreparedTxAction::Fatal;
        return out;
    }

    if (!IsPlaybackRingPathReady(metadata) || !txPcmPacketRing_.valid) {
        if (directTxBinding_.control) {
            directTxBinding_.control->counters.txStartupSilenceSlots.fetch_add(
                1, std::memory_order_relaxed);
        }
        out.action = Tx::PreparedTxAction::NoChange;
        return out;
    }

    if (!request.writable || request.payloadBytes == nullptr || request.payloadCapacityBytes == 0) {
        if (directTxBinding_.control) {
            directTxBinding_.control->counters.txStartupSilenceSlots.fetch_add(
                1, std::memory_order_relaxed);
        }
        out.action = Tx::PreparedTxAction::NoChange;
        return out;
    }

    const uint64_t oldestValid = directOutputReader_.OutputOldestValidFrame();
    const uint64_t writtenEnd = directOutputReader_.OutputWrittenEndFrame();

    // A binding can appear after the DMA ring was primed. Those existing slots
    // use the old zero-based timeline and must drain as startup silence.
    if (metadata.timelineFirstFrame < txOutputOffsetFrames_) {
        directTxBinding_.control->counters.txStartupSilenceSlots.fetch_add(
            1, std::memory_order_relaxed);
        ASFW_LOG_RL(
            Isoch, "tx/prebinding_slot", 1000, OS_LOG_TYPE_DEFAULT,
            "IT TX PRE-BINDING SLOT: pkt=%u timeline=%llu offset=%u state=%u; keeping startup silence",
            request.packetIndex,
            metadata.timelineFirstFrame,
            txOutputOffsetFrames_,
            static_cast<uint32_t>(metadata.preparationState));
        out.action = Tx::PreparedTxAction::NoChange;
        return out;
    }

    // Discontinuity diagnostics
    const uint64_t discontinuityGeneration =
        directTxBinding_.control->playbackRingDiscontinuityGeneration.load(
            std::memory_order_acquire);

    if (discontinuityGeneration != lastDiscontinuityGeneration_) {
        lastDiscontinuityGeneration_ = discontinuityGeneration;
        if (directTxBinding_.control) {
            directTxBinding_.control->counters.txTimelineDiscontinuities.fetch_add(
                1, std::memory_order_relaxed);
        }
        ASFW_LOG(
            Audio,
            "ADK TX DISCONTINUITY RECORDED disc=%llu pkt=%u writtenEnd=%llu oldest=%llu",
            discontinuityGeneration,
            request.packetIndex,
            writtenEnd,
            oldestValid);
    }

    if (metadata.timelineFirstFrame >
            std::numeric_limits<uint64_t>::max() - metadata.framesPerPacket) {
        PublishFatalFault(ASFW::Audio::Runtime::FatalStreamReason::TxSlotInvariant,
                          request, 0, 0, oldestValid, writtenEnd);
        out.action = Tx::PreparedTxAction::Fatal;
        return out;
    }
    const uint64_t audioFirst =
        metadata.timelineFirstFrame - txOutputOffsetFrames_;
    const uint64_t audioEnd = audioFirst + metadata.framesPerPacket;

    const uint32_t slot = txPcmPacketRing_.SlotForFrame(audioFirst);
    const auto& stamp = txPcmPacketRing_.slotStamps[slot];
    const uint32_t expectedGeneration =
        txPcmPacketRing_.GenerationForFrame(audioFirst);
    const bool covered =
        stamp.generation == expectedGeneration &&
        stamp.begin <= audioFirst &&
        stamp.end >= audioEnd;
    if (!covered) {
        ASFW_LOG_RL(Isoch, "tx/payload_uncovered", 100, OS_LOG_TYPE_DEFAULT,
                    "TX PAYLOAD UNCOVERED slot=%u expected=[%llu,%llu) gen=%u stamp=[%llu,%llu) stampGen=%u "
                    "timeline=[%llu,%llu) offset=%u audio=[%llu,%llu) written=[%llu,%llu) lagToWritten=%lld missToStamp=%lld",
                    slot, audioFirst, audioEnd, expectedGeneration,
                    stamp.begin, stamp.end, stamp.generation,
                    metadata.timelineFirstFrame, metadata.timelineFirstFrame + metadata.framesPerPacket,
                    txOutputOffsetFrames_,
                    audioFirst, audioEnd,
                    oldestValid, writtenEnd,
                    static_cast<int64_t>(writtenEnd) - static_cast<int64_t>(audioFirst),
                    static_cast<int64_t>(audioFirst) - static_cast<int64_t>(stamp.begin));
        if (audioFirst >= oldestValid) {
            out.action = Tx::PreparedTxAction::NoChange;
            out.sourceFirstFrame = audioFirst;
            out.sourceEndFrame = audioEnd;
            return out;
        }
    }

    const uint32_t* srcPayload = covered
        ? txPcmPacketRing_.PacketPayloadForTimeline(audioFirst)
        : txPcmPacketRing_.SilencePayload();
    if (!srcPayload) {
        PublishFatalFault(ASFW::Audio::Runtime::FatalStreamReason::TxSlotInvariant,
                          request, 0, 0, oldestValid, writtenEnd);
        out.action = Tx::PreparedTxAction::Fatal;
        return out;
    }

    // CIP header + Audio payload size verification
    uint32_t bytesWritten = 0;
    const uint32_t am824Slots = metadata.am824Slots == 0 ? metadata.pcmChannels : metadata.am824Slots;
    const Direct::Tx::DirectTxPacketHeaderRequest header{
        .sid = metadata.cip.sid,
        .am824Slots = am824Slots,
        .dbc = metadata.cip.dbc,
        .syt = metadata.cip.syt,
        .isNoData = false,
    };

    if (!Direct::Tx::BeginDirectTxPacket(header, request.payloadBytes, request.payloadCapacityBytes, bytesWritten)) {
        PublishFatalFault(ASFW::Audio::Runtime::FatalStreamReason::TxSlotInvariant,
                          request, 0, 0, oldestValid, writtenEnd);
        out.action = Tx::PreparedTxAction::Fatal;
        return out;
    }

    const uint32_t payloadQuadlets = metadata.framesPerPacket * am824Slots;
    const uint32_t payloadSizeBytes = payloadQuadlets * sizeof(uint32_t);
    if (bytesWritten + payloadSizeBytes > request.payloadCapacityBytes) {
        PublishFatalFault(ASFW::Audio::Runtime::FatalStreamReason::TxSlotInvariant,
                          request, 0, 0, oldestValid, writtenEnd);
        out.action = Tx::PreparedTxAction::Fatal;
        return out;
    }

    // Volatile copy to uncached DMA memory
    auto* dstPayload = reinterpret_cast<volatile uint32_t*>(request.payloadBytes + bytesWritten);
    for (uint32_t i = 0; i < payloadQuadlets; ++i) {
        dstPayload[i] = srcPayload[i];
    }

    // Audit and count wire payload
    uint32_t payloadFrameMask = 0;
    uint32_t payloadChannelMask = 0;
    int32_t payloadFirstNonzeroFrame = -1;
    int32_t payloadFirstNonzeroChannel = -1;
    const auto wireFormat = metadata.wireFormat;

    for (uint32_t f = 0; f < metadata.framesPerPacket; ++f) {
        for (uint32_t ch = 0; ch < metadata.pcmChannels; ++ch) {
            const uint32_t word = srcPayload[f * am824Slots + ch];
            bool wordIsNonzero = false;
            if (wireFormat == ASFW::Encoding::AudioWireFormat::kRawPcm24In32) {
                wordIsNonzero = (word != 0);
            } else {
                wordIsNonzero = ((OSSwapBigToHostInt32(word) & 0x00ffffffu) != 0);
            }

            if (wordIsNonzero) {
                payloadFrameMask |= (1u << f);
                payloadChannelMask |= (1u << ch);
                if (payloadFirstNonzeroFrame == -1) {
                    payloadFirstNonzeroFrame = static_cast<int32_t>(f);
                    payloadFirstNonzeroChannel = static_cast<int32_t>(ch);
                }
            }
        }
    }

    // Increment nonzero/all-zero counters based on actual payload
    if (directTxBinding_.control) {
        if (payloadFrameMask != 0) {
            directTxBinding_.control->counters.txPcmNonzeroPackets.fetch_add(
                1, std::memory_order_relaxed);
        } else {
            directTxBinding_.control->counters.txPcmAllZeroPackets.fetch_add(
                1, std::memory_order_relaxed);
        }

        directTxBinding_.control->counters.txPreparedPcmSlots.fetch_add(
            1, std::memory_order_relaxed);
        directTxBinding_.control->counters.txPcmFramesEncoded.fetch_add(
            metadata.framesPerPacket, std::memory_order_relaxed);

        uint32_t previousMin =
            directTxBinding_.control->txMinimumPreparationDistance.load(
                std::memory_order_relaxed);
        while (request.distanceToHardware < previousMin &&
               !directTxBinding_.control->txMinimumPreparationDistance
                    .compare_exchange_weak(
                        previousMin,
                        request.distanceToHardware,
                        std::memory_order_release,
                        std::memory_order_relaxed)) {
        }
    }

    out.firstSourceSamples[0] = 0;
    out.firstSourceSamples[1] = 0;
    out.firstEncodedWords[0] = srcPayload[0];
    out.firstEncodedWords[1] = am824Slots > 1 ? srcPayload[1] : 0;

    if (ASFW::LogConfig::Shared().IsIsochTxVerifierEnabled()) {
        ASFW_LOG_RL(
            Isoch, "tx/prepared_clip_payload", 1000, OS_LOG_TYPE_DEFAULT,
            "IT TX PACKET PAYLOAD: pkt=%u gen=%llu distance=%u dbc=0x%02x syt=0x%04x timeline=%llu wire=[%08x %08x] nonzero=%d fMask=0x%x chMask=0x%x srcFMask=0x0 srcChMask=0x0 (first f=%d ch=%d) pop=%d oldest=%llu written=%llu",
            request.packetIndex,
            request.metadata.generation,
            request.distanceToHardware,
            metadata.cip.dbc,
            metadata.cip.syt,
            metadata.timelineFirstFrame,
            out.firstEncodedWords[0],
            out.firstEncodedWords[1],
            (payloadFrameMask != 0) ? 1 : 0,
            payloadFrameMask,
            payloadChannelMask,
            payloadFirstNonzeroFrame,
            payloadFirstNonzeroChannel,
            covered ? 1 : 0,
            oldestValid,
            writtenEnd);
    }

    out.action = Tx::PreparedTxAction::Prepared;
    out.sourceFirstFrame = audioFirst;
    out.sourceEndFrame = audioEnd;

    metadata.sourceFirstFrame = audioFirst;
    metadata.sourceEndFrame = audioEnd;
    metadata.preparationState = Tx::PreparedTxSlotState::PcmPrepared;

    return out;
}

void IsochAudioTxPipeline::PopulateClipStyleTxRingFromWrittenRange() noexcept {
    if (!directOutputReader_.IsBound()) {
        return;
    }
    const uint64_t end = directOutputReader_.OutputWrittenEndFrame();
    const uint64_t oldestValid = directOutputReader_.OutputOldestValidFrame();
    uint64_t begin = lastPopulatedWriteEnd_;
    if (begin < oldestValid || begin > end) {
        begin = oldestValid;
    }
    if (begin >= end) {
        return;
    }

    const uint32_t capacity = txPcmPacketRing_.outputFrameCapacity;
    if (capacity == 0) {
        return;
    }

    // Limit populate range to capacity to avoid redundant wrap-around overwriting
    if (end - begin > capacity) {
        begin = end - capacity;
    }

    const uint32_t channels = assembler_.channelCount();
    const uint32_t am824Slots = assembler_.am824SlotCount();
    const auto format = assembler_.audioWireFormat();

    uint32_t frameMask = 0;
    uint32_t channelMask = 0;
    int32_t firstNonzeroFrameIdx = -1;
    int32_t firstNonzeroChannel = -1;

    for (uint64_t frame = begin; frame < end; ++frame) {
        const int32_t* srcFrame = directOutputReader_.Frame(frame);
        uint32_t* destPtr = txPcmPacketRing_.PayloadForAudioFrame(frame);
        if (!destPtr) {
            continue;
        }

        if (srcFrame) {
            Direct::Tx::EncodeDirectTxPcmFrame(srcFrame, channels, am824Slots, format, destPtr);
            for (uint32_t ch = 0; ch < channels; ++ch) {
                if (srcFrame[ch] != 0) {
                    frameMask |= (1u << ((frame - begin) % 32));
                    channelMask |= (1u << ch);
                    if (firstNonzeroFrameIdx == -1) {
                        firstNonzeroFrameIdx = static_cast<int32_t>(frame - begin);
                        firstNonzeroChannel = static_cast<int32_t>(ch);
                    }
                }
            }
        } else {
            Direct::Tx::EncodeDirectTxSilenceFrame(channels, am824Slots, format, destPtr);
        }

        const uint32_t slot = txPcmPacketRing_.SlotForFrame(frame);
        auto& stamp = txPcmPacketRing_.slotStamps[slot];
        const uint64_t packetFirst =
            txPcmPacketRing_.PacketFirstFrame(frame);
        const uint64_t packetEnd =
            packetFirst + txPcmPacketRing_.framesPerPacket;
        const uint32_t generation =
            txPcmPacketRing_.GenerationForFrame(frame);
        const bool samePacket =
            stamp.generation == generation &&
            stamp.begin >= packetFirst &&
            stamp.end <= packetEnd;

        if (!samePacket) {
            stamp.begin = frame;
            stamp.end = frame + 1;
            stamp.generation = generation;
        } else if (frame == stamp.end) {
            ++stamp.end;
        } else if (frame + 1 == stamp.begin) {
            --stamp.begin;
        }
    }

    lastPopulatedWriteEnd_ = end;
    MaybeLogTxFrameMatrix(begin, end);

    if (ASFW::LogConfig::Shared().GetIsochVerbosity() >= 2) {
        ASFW_LOG_RL(Isoch, "tx/clip_populate", 1000, OS_LOG_TYPE_DEFAULT,
                    "TX RING POPULATE: range=[%llu, %llu) count=%llu oldestValid=%llu frameMask=0x%x chMask=0x%x (first frameIdx=%d ch=%d)",
                    begin, end, end - begin, oldestValid, frameMask, channelMask, firstNonzeroFrameIdx, firstNonzeroChannel);
    }
}

void IsochAudioTxPipeline::MaybeLogTxFrameMatrix(uint64_t populatedBegin,
                                                 uint64_t populatedEnd) noexcept {
    if (!ASFW::LogConfig::Shared().IsIsochTxVerifierEnabled()) {
        return;
    }

    const uint32_t framesPerPacket = txPcmPacketRing_.framesPerPacket;
    const uint32_t channels = assembler_.channelCount();
    const uint32_t slots = txPcmPacketRing_.am824Slots;
    if (framesPerPacket == 0 || slots == 0 ||
        framesPerPacket > kTxFrameMatrixMaxFrames ||
        channels > kTxFrameMatrixMaxSlots ||
        slots > kTxFrameMatrixMaxSlots ||
        populatedEnd < framesPerPacket) {
        return;
    }

    const uint64_t packetFirst =
        txPcmPacketRing_.PacketFirstFrame(populatedEnd - framesPerPacket);
    const uint64_t packetEnd = packetFirst + framesPerPacket;
    if (packetFirst < populatedBegin || packetEnd > populatedEnd) {
        return;
    }

    const uint32_t ringSlot = txPcmPacketRing_.SlotForFrame(packetFirst);
    const auto& stamp = txPcmPacketRing_.slotStamps[ringSlot];
    const uint32_t expectedGeneration =
        txPcmPacketRing_.GenerationForFrame(packetFirst);
    if (stamp.generation != expectedGeneration ||
        stamp.begin > packetFirst ||
        stamp.end < packetEnd) {
        return;
    }

    const uint64_t now = ASFW::LogDetail::NowNs();
    if (lastTxFrameMatrixLogNs_ != 0 &&
        now - lastTxFrameMatrixLogNs_ < kTxFrameMatrixLogIntervalNs) {
        return;
    }
    lastTxFrameMatrixLogNs_ = now;

    const uint32_t* encodedPacket =
        txPcmPacketRing_.PacketPayloadForTimeline(packetFirst);
    if (!encodedPacket) {
        return;
    }

    std::array<int32_t,
               kTxFrameMatrixMaxFrames * kTxFrameMatrixMaxSlots>
        sourceMatrix{};
    std::array<uint32_t,
               kTxFrameMatrixMaxFrames * kTxFrameMatrixMaxSlots>
        encodedDestinationMatrix{};
    for (uint32_t frame = 0; frame < framesPerPacket; ++frame) {
        const int32_t* srcFrame =
            directOutputReader_.Frame(packetFirst + frame);
        if (srcFrame) {
            std::copy_n(
                srcFrame,
                channels,
                sourceMatrix.data() +
                    static_cast<size_t>(frame) * kTxFrameMatrixMaxSlots);
        }
        std::copy_n(
            encodedPacket + static_cast<size_t>(frame) * slots,
            slots,
            encodedDestinationMatrix.data() +
                static_cast<size_t>(frame) * kTxFrameMatrixMaxSlots);
    }

    ASFWDebugTxFrameMatrixBreakpoint(sourceMatrix.data(),
                                     encodedDestinationMatrix.data(),
                                     packetFirst,
                                     framesPerPacket,
                                     channels,
                                     slots);

    ASFW_LOG(
        Audio,
        "TX FRAME MATRIX BEGIN source=[%llu,%llu) frames=%u channels=%u slots=%u format=%u generation=%u",
        packetFirst,
        packetEnd,
        framesPerPacket,
        channels,
        slots,
        static_cast<uint32_t>(assembler_.audioWireFormat()),
        expectedGeneration);

    const uint32_t matrixSlots = std::max(channels, slots);
    for (uint32_t frame = 0; frame < framesPerPacket; ++frame) {
        const int32_t* srcFrame =
            sourceMatrix.data() +
            static_cast<size_t>(frame) * kTxFrameMatrixMaxSlots;
        const uint32_t* encodedFrame =
            encodedDestinationMatrix.data() +
            static_cast<size_t>(frame) * kTxFrameMatrixMaxSlots;
        for (uint32_t slotBase = 0; slotBase < matrixSlots;
             slotBase += kTxFrameMatrixChunkSlots) {
            const uint32_t slotCount =
                std::min(kTxFrameMatrixChunkSlots, matrixSlots - slotBase);
            LogTxFrameChunk(packetFirst,
                            frame,
                            slotBase,
                            slotCount,
                            srcFrame,
                            channels,
                            encodedFrame,
                            slots);
        }
    }

    ASFW_LOG(Audio,
             "TX FRAME MATRIX END source=[%llu,%llu)",
             packetFirst,
             packetEnd);
}

} // namespace ASFW::Isoch
