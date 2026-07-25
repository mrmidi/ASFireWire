// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "DirectAudioReceiveConsumer.hpp"

#include "../../../../Common/TimingUtils.hpp"
#include "../../../../Logging/Logging.hpp"
#include "../../../../Shared/Isoch/AudioTimingGeometry.hpp"

#include <utility>

namespace ASFW::AudioEngine::Direct::Rx {

const char* DirectAudioReceiveConsumer::ReplayResetReasonName(
    ReplayResetReason reason) noexcept {
    switch (reason) {
        case ReplayResetReason::kPacketProcessorStatus:
            return "packet-status";
        case ReplayResetReason::kInvalidReceiveTimestamp:
            return "invalid-rx-timestamp";
        case ReplayResetReason::kReceiveCycleGap:
            return "receive-cycle-gap";
        case ReplayResetReason::kSytCadenceRejected:
            return "syt-cadence-rejected";
        case ReplayResetReason::kClockAnchorRejected:
            return "clock-anchor-rejected";
    }
    return "unknown";
}

DirectAudioReceiveConsumer::DirectAudioReceiveConsumer(
    ::ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
    Configuration configuration) noexcept
    : bindingSource_(bindingSource)
    , configuration_(configuration) {}

void DirectAudioReceiveConsumer::SetBindingSource(
    ::ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource) noexcept {
    bindingSource_ = bindingSource;
    lastBindingGeneration_ = 0;
    payloadWriterTelemetryAggregator_.Reset();
}

void DirectAudioReceiveConsumer::SetTimingLossCallback(
    TimingLossCallback callback) noexcept {
    timingLossCallback_ = std::move(callback);
}

void DirectAudioReceiveConsumer::SetZtsAnchorReadyCallback(
    ZtsAnchorReadyCallback callback) noexcept {
    ztsAnchorReadyCallback_ = std::move(callback);
}

void DirectAudioReceiveConsumer::SetReplayReadyCallback(
    ReplayReadyCallback callback) noexcept {
    replayReadyCallback_ = std::move(callback);
}

void DirectAudioReceiveConsumer::OnReceiveActivated() noexcept {
    secondaryAnchored_ = false;
    secondaryAnchorEpoch_ = 0;
    absoluteFrameCursor_ = 0;
    cursorInitialized_ = false;
    lastDbc_ = 0;
    dbcInitialized_ = false;
    ztsPublishCount_ = 0;
    timestampValidCount_ = 0;
    timestampInvalidCount_ = 0;
    cadenceEstablishedLogged_ = false;
    replayReadyNotified_ = false;
    replayResetForStart_ = false;
    replayCycleInitialized_ = false;
    lastReplayCycleOrdinal_ = 0;
    ztsTelemetry_.Reset();
    ztsTelemetryLogGate_.Reset();
    prevLoggedAnchorFrame_ = 0;
    prevLoggedAnchorHostTicks_ = 0;
    prevLoggedAnchorRate_ = 0;
    prevLoggedAnchorValid_ = false;
}

void DirectAudioReceiveConsumer::OnReceiveQuiesced() noexcept {
    // The transport has observed OHCI ACTIVE clear before this callback. It is
    // now safe to release views into the audio-owned binding on a later rebind.
    inputWriter_.Unbind();
    clockPublisher_.Unbind();
    inputView_ = {};
    replayResetForStart_ = false;
}

void DirectAudioReceiveConsumer::BeginReceiveBatch(
    const ::ASFW::Isoch::IsochReceiveBatch&) noexcept {
    if (!bindingSource_) {
        return;
    }

    ::ASFW::Audio::Runtime::DirectAudioBindingSnapshot snapshot{};
    if (!bindingSource_->CopyDirectAudioBinding(snapshot)) {
        if (lastBindingGeneration_ != 0) {
            inputWriter_.Unbind();
            clockPublisher_.Unbind();
            inputView_ = {};
            replayResetForStart_ = false;
            lastBindingGeneration_ = 0;
        }
        return;
    }
    if (snapshot.generation == lastBindingGeneration_) {
        return;
    }

    payloadWriterTelemetryAggregator_.Reset();
    if (snapshot.valid && snapshot.HasInput()) {
        inputView_.guid = 0;
        inputView_.sampleRateHz = snapshot.sampleRateHz;
        inputView_.memory.inputBase = snapshot.inputBase;
        inputView_.memory.inputFrameCapacity = snapshot.inputFrames;
        inputView_.memory.inputChannels = snapshot.inputChannels;
        inputView_.memory.storage =
            ::ASFW::Audio::Runtime::AudioSampleStorage::kFloat32Native;
        inputView_.control = snapshot.control;
        inputView_.deviceToHostAm824Slots =
            configuration_.am824Slots > 0 ? configuration_.am824Slots : snapshot.inputChannels;
        inputView_.hostToDeviceAm824Slots = snapshot.outputChannels;
        inputView_.streamMode = ::ASFW::Audio::Runtime::AudioStreamMode::kUnknown;
        inputView_.hostToDeviceWireFormat = ::ASFW::Audio::Runtime::AudioWireFormat::kAM824;

        if (!configuration_.isSecondary && inputView_.control) {
            inputView_.control->rxSytCadence.Reset();
            inputView_.control->rxSequenceReplay.Reset();
            inputView_.control->rxReplayEpochResets.fetch_add(1, std::memory_order_relaxed);
            replayResetForStart_ = true;
        }
        inputWriter_.Bind(&inputView_);
        if (!configuration_.isSecondary) {
            clockPublisher_.Bind(&inputView_);
        }
    } else {
        inputWriter_.Unbind();
        clockPublisher_.Unbind();
        inputView_ = {};
        replayResetForStart_ = false;
    }
    lastBindingGeneration_ = snapshot.generation;
}

void DirectAudioReceiveConsumer::ConsumePacket(
    const ::ASFW::Isoch::IsochReceiveBatch& batch,
    const ::ASFW::Isoch::IsochReceivePacket& packet) noexcept {
    if (packet.payload.empty()) {
        return;
    }

    // This is deliberately content-side work: the transport neither chooses a
    // decoder nor advances an audio frame cursor.
    if (configuration_.isSecondary && inputView_.control) {
        const uint64_t epoch = inputView_.control->rxReplayEpochResets.load(
            std::memory_order_acquire);
        if (!secondaryAnchored_ || epoch != secondaryAnchorEpoch_) {
            const uint64_t masterEnd = inputView_.control->inputProducedEndFrame.load(
                std::memory_order_acquire);
            if (masterEnd == 0) {
                return;
            }
            absoluteFrameCursor_ = masterEnd;
            secondaryAnchored_ = true;
            secondaryAnchorEpoch_ = epoch;
        }
    }

    const uint32_t channels = configuration_.streamChannels > 0
        ? configuration_.streamChannels
        : inputView_.memory.inputChannels;
    const auto result = processor_.ProcessPacket(
        packet.payload.data(), packet.payload.size(), absoluteFrameCursor_, channels,
        inputView_.deviceToHostAm824Slots, configuration_.wireFormat,
        configuration_.channelOffset, !configuration_.isSecondary);
    if (result.status == DirectRxWriteStatus::kAvailable ||
        result.status == DirectRxWriteStatus::kInvalidBinding) {
        absoluteFrameCursor_ += result.framesDecoded;
    } else {
        ResetReplayEpochForDiscontinuity(
            ReplayResetReason::kPacketProcessorStatus,
            {
                .descriptorIndex = packet.descriptorIndex,
                .payloadBytes = static_cast<uint32_t>(packet.payload.size()),
                .drainCycleTimer = batch.drainCycleTimer,
                .receiveCycleTimestamp = result.receiveCycleTimestamp,
                .syt = result.syt,
                .packetStatus = static_cast<uint32_t>(result.status),
                .sampleFrame = absoluteFrameCursor_,
            });
        return;
    }

    if (configuration_.isSecondary || !inputView_.control) {
        return;
    }

    if (result.hasValidCip) {
        if (dbcInitialized_) {
            inputView_.control->rxDbcFrameCount.fetch_add(
                static_cast<uint8_t>(result.dbc - lastDbc_),
                std::memory_order_relaxed);
        }
        lastDbc_ = result.dbc;
        dbcInitialized_ = true;
    }

    ::ASFW::Isoch::Rx::ExpandedReceiveTimestamp timestamp{};
    const bool validTimestamp = result.hasReceiveCycleTimestamp &&
        ::ASFW::Isoch::Rx::ExpandReceiveTimestamp(
            result.receiveCycleTimestamp, batch.drainCycleTimer, timestamp);
    if (!validTimestamp) {
        ++timestampInvalidCount_;
        ResetReplayEpochForDiscontinuity(
            ReplayResetReason::kInvalidReceiveTimestamp,
            {
                .descriptorIndex = packet.descriptorIndex,
                .payloadBytes = static_cast<uint32_t>(packet.payload.size()),
                .drainCycleTimer = batch.drainCycleTimer,
                .receiveCycleTimestamp = result.receiveCycleTimestamp,
                .syt = result.syt,
                .sampleFrame = absoluteFrameCursor_,
            });
        return;
    }

    ++timestampValidCount_;
    const auto cycleFields = ::ASFW::Timing::decodeCycleTimer(timestamp.cycleTimer);
    const uint32_t cycleOrdinal = cycleFields.seconds * ::ASFW::Timing::kCyclesPerSecond +
        cycleFields.cycle;
    constexpr uint32_t kCycleDomain =
        ::ASFW::Timing::kFWTimeWrapSeconds * ::ASFW::Timing::kCyclesPerSecond;
    if (replayCycleInitialized_ &&
        cycleOrdinal != (lastReplayCycleOrdinal_ + 1) % kCycleDomain) {
        ResetReplayEpochForDiscontinuity(
            ReplayResetReason::kReceiveCycleGap,
            {
                .descriptorIndex = packet.descriptorIndex,
                .payloadBytes = static_cast<uint32_t>(packet.payload.size()),
                .drainCycleTimer = batch.drainCycleTimer,
                .receiveCycleTimestamp = result.receiveCycleTimestamp,
                .syt = result.syt,
                .expectedCycleOrdinal = (lastReplayCycleOrdinal_ + 1) % kCycleDomain,
                .observedCycleOrdinal = cycleOrdinal,
                .sampleFrame = absoluteFrameCursor_,
            });
    }
    lastReplayCycleOrdinal_ = cycleOrdinal;
    replayCycleInitialized_ = true;

    ::ASFW::Audio::Runtime::RxSequenceEntry replayEntry{};
    replayEntry.firstAudioFrame = absoluteFrameCursor_ - result.framesDecoded;
    replayEntry.sourceCycleTimer = timestamp.cycleTimer;
    replayEntry.dataBlocks = static_cast<uint16_t>(result.framesDecoded);
    replayEntry.dbc = result.dbc;
    if (result.hasValidCip) {
        replayEntry.flags |= ::ASFW::Audio::Runtime::RxSequenceFlags::kValidCip;
    }
    if (result.hasValidCip && result.syt != 0xffff) {
        const bool cadenceAccepted = inputView_.control->rxSytCadence.Observe(
            result.syt, timestamp.cycleTimer);
        if (!cadenceAccepted && inputView_.control->rxSequenceReplay.IsEstablished()) {
            ResetReplayEpochForDiscontinuity(
                ReplayResetReason::kSytCadenceRejected,
                {
                    .descriptorIndex = packet.descriptorIndex,
                    .payloadBytes = static_cast<uint32_t>(packet.payload.size()),
                    .drainCycleTimer = batch.drainCycleTimer,
                    .receiveCycleTimestamp = result.receiveCycleTimestamp,
                    .syt = result.syt,
                    .observedCycleOrdinal = cycleOrdinal,
                    .sampleFrame = absoluteFrameCursor_,
                });
        }
        replayEntry.sytOffset = ::ASFW::Audio::Runtime::ComputeReplaySytOffset(
            result.syt, timestamp.cycleTimer,
            inputView_.control->rxTransferDelayTicks.load(std::memory_order_relaxed));
        replayEntry.flags |= ::ASFW::Audio::Runtime::RxSequenceFlags::kValidSyt;
    }
    inputView_.control->rxSequenceReplay.Publish(replayEntry);
    inputView_.control->rxReplayEntries.fetch_add(1, std::memory_order_relaxed);

    ::ASFW::Driver::RxSytCadence::Snapshot cadence{};
    if (inputView_.control->rxSytCadence.TrySnapshot(cadence) && cadence.established) {
        if (!inputView_.control->rxSequenceReplay.IsEstablished()) {
            (void)inputView_.control->rxSequenceReplay.MarkEstablished();
        }
        if (!replayReadyNotified_ && replayReadyCallback_) {
            replayReadyNotified_ = true;
            replayReadyCallback_();
        }
    }

    uint64_t packetHostTicks = batch.drainHostTicks;
    if (timestamp.ageTicks >= 0) {
        const uint64_t ageHostTicks = ::ASFW::Timing::nanosToHostTicks(
            ::ASFW::Isoch::Rx::FireWireTicksToNanos(
                static_cast<uint64_t>(timestamp.ageTicks)));
        packetHostTicks = batch.drainHostTicks > ageHostTicks
            ? batch.drainHostTicks - ageHostTicks
            : batch.drainHostTicks;
    } else {
        ++negativeAgeCount_;
        if (-timestamp.ageTicks >=
            static_cast<int64_t>(::ASFW::Timing::kTicksPerCycle)) {
            ++largeNegativeAgeCount_;
        }
        packetHostTicks += ::ASFW::Timing::nanosToHostTicks(
            ::ASFW::Isoch::Rx::FireWireTicksToNanos(
                static_cast<uint64_t>(-timestamp.ageTicks)));
    }

    const uint64_t packetFirstFrame = absoluteFrameCursor_ - result.framesDecoded;
    constexpr uint64_t kZtsPeriodFrames =
        ::ASFW::IsochTransport::AudioTimingGeometry::kHalZeroTimestampPeriodFrames;
    const uint32_t nanosPerSampleQ8 = inputView_.sampleRateHz == 0 ? 0 :
        static_cast<uint32_t>((1'000'000'000ULL << 8) / inputView_.sampleRateHz);
    if (kZtsPeriodFrames != 0 && result.framesDecoded != 0 &&
        (packetFirstFrame % kZtsPeriodFrames) == 0 && packetHostTicks != 0 &&
        nanosPerSampleQ8 != 0 && clockPublisher_.IsBound() && cadence.established) {
        const auto publish = clockPublisher_.Publish(packetFirstFrame, packetHostTicks,
                                                     nanosPerSampleQ8);
        if (!publish.accepted) {
            ResetReplayEpochForDiscontinuity(
                ReplayResetReason::kClockAnchorRejected,
                {
                    .descriptorIndex = packet.descriptorIndex,
                    .payloadBytes = static_cast<uint32_t>(packet.payload.size()),
                    .drainCycleTimer = batch.drainCycleTimer,
                    .receiveCycleTimestamp = result.receiveCycleTimestamp,
                    .syt = result.syt,
                    .observedCycleOrdinal = cycleOrdinal,
                    .sampleFrame = packetFirstFrame,
                });
        } else {
            ++ztsPublishCount_;
            if (publish.notifyConsumer && ztsAnchorReadyCallback_) {
                ztsAnchorReadyCallback_(publish.notificationGeneration);
            }
            ::ASFW::Isoch::Rx::ZtsTelemetryRecord record{};
            record.publishCount = ztsPublishCount_;
            record.sampleFrame = packetFirstFrame;
            record.hostTicks = packetHostTicks;
            record.rawHostTicks = packetHostTicks;
            record.drainHostTicks = batch.drainHostTicks;
            record.ageTicks = timestamp.ageTicks;
            record.drainCycleTimer = batch.drainCycleTimer;
            record.rxCycleTimer = timestamp.cycleTimer;
            record.descriptorIndex = packet.descriptorIndex;
            record.framesDecoded = result.framesDecoded;
            record.hostNanosPerSampleQ8 = nanosPerSampleQ8;
            record.rawRxTs = result.receiveCycleTimestamp;
            record.syt = result.syt;
            record.kind = static_cast<uint8_t>(ztsPublishCount_ == 1
                ? ::ASFW::Isoch::Rx::ZtsEventKind::kSeed
                : ::ASFW::Isoch::Rx::ZtsEventKind::kUpdate);
            ztsTelemetry_.Record(record);
        }
    }
}

void DirectAudioReceiveConsumer::ResetReplayEpochForDiscontinuity(
    ReplayResetReason reason,
    const ReplayResetContext& context) noexcept {
    auto* control = inputView_.control;
    if (!control || !replayResetForStart_) {
        replayCycleInitialized_ = false;
        return;
    }
    const bool wasEstablished = control->rxSequenceReplay.IsEstablished();
    control->rxSytCadence.Reset();
    control->rxSequenceReplay.Reset();
    const uint64_t resetEpoch =
        control->rxReplayEpochResets.fetch_add(1, std::memory_order_relaxed) + 1;
    replayReadyNotified_ = false;
    cadenceEstablishedLogged_ = false;
    replayCycleInitialized_ = false;
    dbcInitialized_ = false;
    if (wasEstablished) {
        ASFW_LOG_ERROR(
            DirectAudio,
            "[RxReplayReset] epoch=%llu reason=%{public}s desc=%u bytes=%u "
            "drain=0x%08x rawTs=0x%04x syt=0x%04x expectedCycle=%u observedCycle=%u "
            "status=%u frame=%llu validTs=%llu invalidTs=%llu",
            resetEpoch, ReplayResetReasonName(reason), context.descriptorIndex,
            context.payloadBytes, context.drainCycleTimer, context.receiveCycleTimestamp,
            context.syt, context.expectedCycleOrdinal, context.observedCycleOrdinal,
            context.packetStatus, context.sampleFrame, timestampValidCount_, timestampInvalidCount_);
    }
    if (wasEstablished && timingLossCallback_) {
        timingLossCallback_();
    }
}

bool DirectAudioReceiveConsumer::IsReplayEstablished() const noexcept {
    const auto* control = inputView_.control;
    return control && control->rxSequenceReplay.IsEstablished();
}

void DirectAudioReceiveConsumer::DrainReceiveTelemetry(uint32_t maxRecords) {
    const uint32_t rate = inputView_.sampleRateHz;
    const uint64_t dropped = ztsTelemetry_.Drain(
        maxRecords, [this, rate](const ::ASFW::Isoch::Rx::ZtsTelemetryRecord& record) {
            if (!ztsTelemetryLogGate_.ShouldEmit(record, rate)) {
                return;
            }
            ASFW_LOG(Zts,
                     "%{public}s count=%llu frame=%llu host=%llu drainHost=%llu "
                     "drainCycle=0x%08x rxCycle=0x%08x age=%lld rawRxTs=0x%04x "
                     "syt=0x%04x desc=%u dec=%u rate=%u rateQ8=%u",
                     record.kind == static_cast<uint8_t>(::ASFW::Isoch::Rx::ZtsEventKind::kSeed)
                         ? "SEED" : "UPD",
                     record.publishCount, record.sampleFrame, record.hostTicks,
                     record.drainHostTicks, record.drainCycleTimer, record.rxCycleTimer,
                     record.ageTicks, record.rawRxTs, record.syt, record.descriptorIndex,
                     record.framesDecoded, rate, record.hostNanosPerSampleQ8);
        });
    if (dropped != 0) {
        ASFW_LOG(Zts, "drain overflow: dropped=%llu (capacity=%u)", dropped,
                 ::ASFW::Isoch::Rx::ZtsTelemetryRing::kCapacity);
    }
}

void DirectAudioReceiveConsumer::DrainPayloadTelemetry() {
    auto* control = inputView_.control;
    if (!control) {
        return;
    }
    const uint64_t errorGeneration =
        control->ioCallbackErrorGeneration.load(std::memory_order_acquire);
    if (errorGeneration != control->ioCallbackErrorReportedGeneration.load(
                               std::memory_order_relaxed)) {
        ASFW_LOG(DirectAudio,
                 "[AudioIO] callback returned kr=0x%08x operation=%u objectId=%u "
                 "frameCount=%u sampleTime=%llu hostTime=%llu errors=%llu",
                 control->ioLastError.load(std::memory_order_relaxed),
                 control->ioLastErrorOperation.load(std::memory_order_relaxed),
                 control->ioLastErrorObjectId.load(std::memory_order_relaxed),
                 control->ioLastErrorFrameCount.load(std::memory_order_relaxed),
                 control->ioLastErrorSampleTime.load(std::memory_order_relaxed),
                 control->ioLastErrorHostTime.load(std::memory_order_relaxed),
                 errorGeneration);
        control->ioCallbackErrorReportedGeneration.store(errorGeneration,
                                                          std::memory_order_release);
    }

    payloadWriterTelemetryAggregator_.BeginDrain();
    ::ASFW::Audio::Runtime::PayloadWriterTelemetryRecord firstRecord{};
    ::ASFW::Audio::Runtime::PayloadWriterTelemetryRecord firstDeficitRecord{};
    ::ASFW::Audio::Runtime::PayloadWriterTelemetryRecord lastRecord{};
    bool haveFirstRecord = false;
    bool haveFirstDeficitRecord = false;
    bool haveLastRecord = false;
    const uint64_t dropped = control->payloadWriterTelemetry.Drain(
        [this, &firstRecord, &haveFirstRecord,
         &firstDeficitRecord, &haveFirstDeficitRecord,
         &lastRecord, &haveLastRecord](
            const ::ASFW::Audio::Runtime::PayloadWriterTelemetryRecord& record) {
            payloadWriterTelemetryAggregator_.Observe(record);
            if (!haveFirstRecord) {
                firstRecord = record;
                haveFirstRecord = true;
            }
            if (!haveFirstDeficitRecord && record.exposureDeficitFrames != 0) {
                firstDeficitRecord = record;
                haveFirstDeficitRecord = true;
            }
            lastRecord = record;
            haveLastRecord = true;
        });
    const auto& summary = payloadWriterTelemetryAggregator_.Summary();
    if (haveLastRecord && summary.HasAnomaly()) {
        ASFW_LOG_RING_ONLY(
            DirectAudio,
            ::ASFW::Logging::LogLevel::Warning,
            "[PayloadWriter] delta v=%llu w=%llu noPkt=%llu outside=%llu race=%llu tx=%llu under=%llu/%llu maxDef=%llu",
            summary.visitedDelta,
            summary.writtenDelta,
            summary.withoutPacketDelta,
            summary.outsidePacketDelta,
            summary.racedReuseDelta,
            summary.wroteIntoTransmittedDelta,
            summary.underExposureCallsDelta,
            summary.underExposureFramesDelta,
            summary.maxExposureDeficitFrames);
        ASFW_LOG_RING_ONLY(
            DirectAudio,
            ::ASFW::Logging::LogLevel::Warning,
            "[PayloadWriter] last sample=%llu comp=%llu pkt=%llu aligned=%u epoch=%llu prepared=%llu/%llu/%llu acquire=%llu ring=%llu/%llu",
            lastRecord.sampleTime,
            lastRecord.completionCursor,
            lastRecord.packetizerNextAudioFrame,
            lastRecord.packetizerFrameCursorAligned ? 1u : 0u,
            lastRecord.packetizerCursorEpoch,
            lastRecord.dataPacketsPrepared,
            lastRecord.noDataPacketsPrepared,
            lastRecord.packetsPrepared,
            lastRecord.slotAcquireFailures,
            lastRecord.playbackRingReadFrame,
            lastRecord.playbackRingWriteFrame);
        if (haveFirstDeficitRecord) {
            ASFW_LOG_RING_ONLY(
                DirectAudio,
                ::ASFW::Logging::LogLevel::Warning,
                "[PayloadWriter] deficit sample=%llu write=%llu exposed=%llu d=%llu comp=%llu target=%llu gen=%llu/%llu wake=%u",
                firstDeficitRecord.sampleTime,
                firstDeficitRecord.writeEndFrame,
                firstDeficitRecord.exposedFrameEnd,
                firstDeficitRecord.exposureDeficitFrames,
                firstDeficitRecord.completionCursor,
                firstDeficitRecord.txPreparationTargetFrameEnd,
                firstDeficitRecord.txPreparationRequestedGeneration,
                firstDeficitRecord.txPreparationHandledGeneration,
                firstDeficitRecord.txPreparationWakeScheduled ? 1u : 0u);
        } else if (haveFirstRecord) {
            ASFW_LOG_RING_ONLY(
                DirectAudio,
                ::ASFW::Logging::LogLevel::Warning,
                "[PayloadWriter] first sample=%llu write=%llu exposed=%llu comp=%llu pkt=%llu aligned=%u epoch=%llu",
                firstRecord.sampleTime,
                firstRecord.writeEndFrame,
                firstRecord.exposedFrameEnd,
                firstRecord.completionCursor,
                firstRecord.packetizerNextAudioFrame,
                firstRecord.packetizerFrameCursorAligned ? 1u : 0u,
                firstRecord.packetizerCursorEpoch);
        }
    }
    if (dropped != 0) {
        ASFW_LOG(DirectAudio, "[PayloadWriter] drain overflow: dropped=%llu (capacity=%u)",
                 dropped, ::ASFW::Audio::Runtime::PayloadWriterTelemetryRing::kCapacity);
    }
}

void DirectAudioReceiveConsumer::LogTransmitTimingTrace() {
    auto* control = inputView_.control;
    if (!control) {
        return;
    }
    ::ASFW::Audio::Runtime::TxSytTraceSample sample{};
    uint64_t decisions = 0;
    if (!control->txSytTrace.ReadLatest(sample, decisions)) {
        return;
    }
    ASFW_LOG(TxSyt,
             "obsCyc=%u rxSyt=0x%04x sytOffDelayFree=%u +txDelay=%u outCyc=%u "
             "=> txSyt=0x%04x (cyc=%u off=0x%03x) pkt=%llu decisions=%llu",
             sample.sourceCycle, sample.observedRxSyt, sample.sytOffsetDelayFree,
             sample.txDelayTicks, sample.outCycle, sample.txSyt,
             (static_cast<uint32_t>(sample.txSyt) >> 12) & 0x0fu,
             static_cast<uint32_t>(sample.txSyt) & 0x0fffu, sample.packetIndex, decisions);
}

} // namespace ASFW::AudioEngine::Direct::Rx
