// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "DirectAudioReceiveConsumer.hpp"

#include "../../../../Common/TimingUtils.hpp"
#include "../../../../Logging/Logging.hpp"
#include "../../../Shared/AudioTimingGeometry.hpp"

#include <DriverKit/IOLib.h>

#include <cstring>
#include <utility>

namespace ASFW::AudioEngine::Direct::Rx {

namespace {

constexpr size_t kIsochReceivePrefixBytes = 8;
constexpr size_t kCipHeaderBytes = 8;

[[nodiscard]] uint32_t LoadLittleEndianQuadlet(const uint8_t* bytes) noexcept {
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return OSSwapLittleToHostInt32(value);
}

[[nodiscard]] uint32_t LoadBigEndianQuadlet(const uint8_t* bytes) noexcept {
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return OSSwapBigToHostInt32(value);
}

} // namespace

const char* DirectAudioReceiveConsumer::ReplayResetReasonName(
    ReplayResetReason reason) noexcept {
    switch (reason) {
        case ReplayResetReason::kEmptyCompletion:
            return "empty-completion";
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
}

void DirectAudioReceiveConsumer::SetTimingLossCallback(
    TimingLossCallback callback) noexcept {
    timingLossCallback_ = std::move(callback);
}

void DirectAudioReceiveConsumer::SetZtsAnchorReadyCallback(
    ZtsAnchorReadyCallback callback) noexcept {
    ztsAnchorReadyCallback_ = std::move(callback);
}

void DirectAudioReceiveConsumer::OnReceiveActivated() noexcept {
    secondaryAnchored_ = false;
    secondaryAnchorEpoch_ = 0;
    absoluteFrameCursor_ = 0;
    primeCaptureDelayLine_ = true;
    captureMapRejectedLogBudget_ = kCaptureMapRejectedLogBudget;
    lastDbc_ = 0;
    dbcInitialized_ = false;
    ztsPublishCount_ = 0;
    timestampValidCount_ = 0;
    timestampInvalidCount_ = 0;
    cadenceEstablishedLogged_ = false;
    replayResetForStart_ = false;
    replayCycleInitialized_ = false;
    lastReplayCycleOrdinal_ = 0;
    busTicksInitialized_ = false;
    lastUnwrappedBusTicks_ = 0;
    zeroDataBlockSizeCaptureLogBudget_ = kZeroDataBlockSizeCaptureLogBudget;
    zeroDataBlockSizeCaptureCount_ = 0;
    headerOnlyNoDataTransitionLogBudget_ = kHeaderOnlyNoDataTransitionLogBudget;
    receivedWirePayloadLogBudget_ = kReceivedWirePayloadLogBudget;
    unwrittenStatusLogBudget_ = kUnwrittenStatusLogBudget;
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
    // The cached generation described the view we just dropped, so it must not
    // survive the quiesce. The audio side only bumps its generation when the
    // *binding* changes, not per stream start: across a stop/start of the same
    // endpoint it republishes the same generation. Leaving the cache set makes
    // the next BeginReceiveBatch() take its "nothing changed" early return, so
    // the writer is never rebound and every packet then decodes down the
    // kInvalidBinding path — which drops PCM while advancing the frame cursor
    // and incrementing no reject counter. Capture goes silent with completely
    // healthy telemetry.
    lastBindingGeneration_ = 0;
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

    if (snapshot.valid && snapshot.HasInput()) {
        inputView_.endpointId = snapshot.endpointId;
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
            bootstrapResetLogBudget_ = kBootstrapResetLogBudget;
            zeroDataBlockSizeCaptureLogBudget_ = kZeroDataBlockSizeCaptureLogBudget;
            zeroDataBlockSizeCaptureCount_ = 0;
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
        // A completed OHCI descriptor is one receive-cycle outcome even when
        // it contains no packet image. Silently dropping it hides the cycle
        // loss from replay and was one candidate trigger for the DICE TX
        // cursor jump. Attribute it once and invalidate recovered timing; a
        // secondary stream never mutates the master's replay state.
        if (!configuration_.isSecondary && inputView_.control) {
            inputView_.control->rxPacketsSeen.fetch_add(
                1, std::memory_order_relaxed);
            inputView_.control->rxEmptyCompletions.fetch_add(
                1, std::memory_order_relaxed);
            ResetReplayEpochForDiscontinuity(
                ReplayResetReason::kEmptyCompletion,
                {
                    .descriptorIndex = packet.descriptorIndex,
                    .payloadBytes = 0,
                    .drainCycleTimer = batch.drainCycleTimer,
                    .packetStatus = packet.transferStatus,
                    .sampleFrame = absoluteFrameCursor_,
                });
        }
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

    if (pendingCursorCorrectionFrames_ != 0) {
        absoluteFrameCursor_ = static_cast<uint64_t>(
            static_cast<int64_t>(absoluteFrameCursor_) +
            pendingCursorCorrectionFrames_);
        pendingCursorCorrectionFrames_ = 0;
    }

    const uint32_t channels = configuration_.streamChannels > 0
        ? configuration_.streamChannels
        : inputView_.memory.inputChannels;
    const bool primeDelayLine = primeCaptureDelayLine_;
    const auto result = processor_.ProcessPacket(
        packet.payload.data(), packet.payload.size(), absoluteFrameCursor_, channels,
        inputView_.deviceToHostAm824Slots, configuration_.wireFormat,
        configuration_.channelOffset, !configuration_.isSecondary,
        configuration_.captureChannelMap, primeDelayLine);
    if (primeDelayLine && result.framesDecoded != 0) {
        primeCaptureDelayLine_ = false;
    }
    if (result.mapRejected && captureMapRejectedLogBudget_ != 0) {
        --captureMapRejectedLogBudget_;
        ASFW_LOG_ERROR(DirectAudio,
                       "[RxChannelMap] rejected: channels=%u dbs=%u mapSize=%u — "
                       "decoding in wire order",
                       channels, result.dbs,
                       configuration_.captureChannelMap.slotCount);
    }
    const bool acceptedHeaderOnlyNoDataTransition =
        IsAcceptedHeaderOnlyNoDataTransition(packet, result);
    if (!configuration_.isSecondary && result.hasValidCip && result.syt != 0xffff) {
        LogReceivedWirePayload(packet, result);
    }
    if (result.status == DirectRxWriteStatus::kZeroDataBlockSize &&
        !acceptedHeaderOnlyNoDataTransition) {
        LogZeroDataBlockSizeEvidence(batch, packet, result);
    }
    // Attribute every decoded packet before the reject branch returns; the
    // master stream only, so a second slice cannot double-count.
    if (!configuration_.isSecondary && inputView_.control) {
        auto* counters = inputView_.control;
        counters->rxPacketsSeen.fetch_add(1, std::memory_order_relaxed);
        switch (result.status) {
            case DirectRxWriteStatus::kShortPacket:
                counters->rxShortPackets.fetch_add(1, std::memory_order_relaxed);
                break;
            case DirectRxWriteStatus::kInvalidCipHeader:
                counters->rxInvalidCipHeaders.fetch_add(1, std::memory_order_relaxed);
                break;
            case DirectRxWriteStatus::kZeroDataBlockSize:
                counters->rxZeroDataBlockSize.fetch_add(1, std::memory_order_relaxed);
                break;
            case DirectRxWriteStatus::kGeometryMismatch:
                counters->rxGeometryMismatch.fetch_add(1, std::memory_order_relaxed);
                break;
            default:
                break;
        }
        if (result.hasValidCip) {
            if (result.syt == 0xffff) {
                counters->rxNoDataPackets.fetch_add(1, std::memory_order_relaxed);
            } else {
                counters->rxDataPackets.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    if (result.status == DirectRxWriteStatus::kAvailable ||
        result.status == DirectRxWriteStatus::kInvalidBinding) {
        // kInvalidBinding advances the cursor deliberately — the timeline must
        // stay continuous across an unbound window — but it wrote no PCM. Folded
        // into the success branch unattributed it is indistinguishable from
        // "wrote everything", which is how a fully silent capture kept reporting
        // green counters. Name it; the cursor behaviour is unchanged.
        if (!configuration_.isSecondary &&
            result.status == DirectRxWriteStatus::kInvalidBinding &&
            unwrittenStatusLogBudget_ != 0) {
            --unwrittenStatusLogBudget_;
            ASFW_LOG_ERROR(DirectAudio,
                           "[RxUnwritten] status=invalid-binding frame=%llu "
                           "frames=%u writerBound=%d budgetLeft=%u",
                           absoluteFrameCursor_, result.framesDecoded,
                           inputWriter_.IsBound() ? 1 : 0,
                           unwrittenStatusLogBudget_);
        }
        absoluteFrameCursor_ += result.framesDecoded;
    } else if (acceptedHeaderOnlyNoDataTransition) {
        ObserveAcceptedHeaderOnlyNoDataTransition(batch, packet, result);
        return;
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

    // Back-date software handling to the controller-observed packet event.
    uint64_t packetHostTicks = batch.drainHostTicks;
    // True when `packetHostTicks` is the controller's receive instant rather
    // than a forward-dated estimate; only then is it a usable F3 for J3.
    bool packetReceiveTicksValid = false;
    if (timestamp.ageTicks >= 0) {
        const uint64_t ageHostTicks = ::ASFW::Timing::nanosToHostTicks(
            ::ASFW::Isoch::Rx::FireWireTicksToNanos(
                static_cast<uint64_t>(timestamp.ageTicks)));
        packetHostTicks = batch.drainHostTicks > ageHostTicks
            ? batch.drainHostTicks - ageHostTicks
            : batch.drainHostTicks;
        packetReceiveTicksValid = true;
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

    const auto cycleFields = ::ASFW::Timing::decodeCycleTimer(timestamp.cycleTimer);
    const uint32_t cycleOrdinal = cycleFields.seconds * ::ASFW::Timing::kCyclesPerSecond +
        cycleFields.cycle;
    constexpr uint32_t kCycleDomain =
        ::ASFW::Timing::kFWTimeWrapSeconds * ::ASFW::Timing::kCyclesPerSecond;
    // Ahead of the continuity check on purpose. A receive-cycle gap is the
    // commonest way frames go missing, and its handler resets the cadence ring
    // -- so measuring after it would blind the comparison to exactly the case it
    // exists for, and for the 513 packets the ring then takes to re-establish.
    if (result.framesDecoded != 0 && result.hasValidCip &&
        result.syt != 0xffff && inputView_.control) {
        ::ASFW::Driver::RxSytCadence::Snapshot preCadence{};
        if (inputView_.control->rxSytCadence.TrySnapshot(preCadence) &&
            preCadence.established) {
            const int64_t currentPhase = ::ASFW::Timing::normalizeOffsetDomain(
                ::ASFW::Timing::extendTstampFromCycleTimer(timestamp.cycleTimer,
                                                           result.syt));
            MeasureCursorAgainstPhase(
                preCadence, currentPhase,
                absoluteFrameCursor_ - result.framesDecoded,
                result.framesDecoded);
        }
    }

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
        // A chain seed is no longer a replay-epoch reset. The old escalation
        // fired on a non-advancing SYT delta, which a burst loss of six or more
        // data packets produces by aliasing rather than by any fault the epoch
        // reset repairs -- see RxSytCadence::Observe. Loss is now measured
        // directly by the phase comparison below, so this only has to be
        // counted.
        if (inputView_.control->rxSytCadence.Observe(
                result.syt, timestamp.cycleTimer) ==
            ::ASFW::Driver::RxSytCadence::Update::kSeeded) {
            inputView_.control->rxCadenceSeeds.fetch_add(
                1, std::memory_order_relaxed);
            // A restart invalidates the reference pair: its phase predates the
            // break, so a delta measured against it would report the break as
            // lost frames.
            cursorPhaseAnchorValid_ = false;
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
    }

    const uint64_t packetFirstFrame =
        absoluteFrameCursor_ - result.framesDecoded;
    // F4 for both J3 and J4, sampled here because here is where the frames
    // actually become visible to a reader.
    //
    // `batch.drainHostTicks` is read once in IsochReceiveContext before
    // DrainCompleted begins, so it predates decoding this packet and every
    // packet ahead of it in the same drain. Dating F4 with it made J3 too short
    // and J4 too long by the same amount -- the sum was right and the split was
    // not. One clock read per published packet is the cost of the split being
    // true; the receive instant it is measured against is still the
    // controller's, so dispatch delay stays inside J3 where it belongs.
    if (result.framesDecoded != 0 && inputView_.control) {
        const uint64_t publishHostTicks = mach_absolute_time();
        inputView_.control->ledgerCaptureDecode.Record(absoluteFrameCursor_,
                                                       publishHostTicks);
        if (packetReceiveTicksValid && publishHostTicks > packetHostTicks) {
            inputView_.control->ledgerJ3ReceiveToDecode.Record(
                ::ASFW::Timing::hostTicksToNanos(
                    publishHostTicks - packetHostTicks) / 1000U);
        }
    }
    if (result.framesDecoded != 0 && packetHostTicks != 0 &&
        clockPublisher_.IsBound() && cadence.established &&
        result.hasValidCip && result.syt != 0xffff &&
        inputView_.control->hardwareTimeline.Source() ==
            ::ASFW::Audio::Runtime::HardwareTimelineSource::Receive) {
        const uint64_t rawBusTicks =
            static_cast<uint64_t>(cycleFields.seconds) *
                ::ASFW::Timing::kTicksPerSecond +
            static_cast<uint64_t>(cycleFields.cycle) *
                ::ASFW::Timing::kTicksPerCycle + cycleFields.offset;
        constexpr uint64_t kBusWrapTicks =
            static_cast<uint64_t>(::ASFW::Timing::kFWTimeWrapSeconds) *
            ::ASFW::Timing::kTicksPerSecond;
        uint64_t packetBusTicks = rawBusTicks;
        if (busTicksInitialized_) {
            const uint64_t base = (lastUnwrappedBusTicks_ / kBusWrapTicks) *
                kBusWrapTicks;
            packetBusTicks = base + rawBusTicks;
            if (packetBusTicks + kBusWrapTicks / 2 < lastUnwrappedBusTicks_) {
                packetBusTicks += kBusWrapTicks;
            }
        }
        busTicksInitialized_ = true;
        lastUnwrappedBusTicks_ = packetBusTicks;
        // The replay entry carries a phase, not a duration -- see
        // ComputePresentationLeadTicks. Adding the raw sum here put every
        // anchor 49,152 ticks (2 ms, 96 frames) late whenever the replay
        // helper's unsigned lift had fired, which for a device whose SYT lead
        // sits below the configured transfer delay is every single packet.
        const uint64_t presentationBusTicks = packetBusTicks +
            ::ASFW::Audio::Runtime::ComputePresentationLeadTicks(
                replayEntry.sytOffset,
                inputView_.control->rxTransferDelayTicks.load(
                    std::memory_order_relaxed));
        ::ASFW::Audio::Runtime::HardwareZeroTimestamp boundary{};
        const auto observed = inputView_.control->hardwareTimeline.Observe({
            .epoch = inputView_.control->hardwareTimeline.Epoch(),
            .source = ::ASFW::Audio::Runtime::HardwareTimelineSource::Receive,
            .sampleFrame = packetFirstFrame,
            .frameCount = result.framesDecoded,
            .presentationBusTicks = presentationBusTicks,
            .correlationBusTicks = packetBusTicks,
            .correlationHostTicks = packetHostTicks,
        }, &boundary);
        if (observed !=
            ::ASFW::Audio::Runtime::HardwareObservationResult::BoundaryReady) {
            return;
        }
        const auto publish = clockPublisher_.Publish(
            boundary.sampleFrame, boundary.hostTicks,
            boundary.hostNanosPerSampleQ8);
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
                    .sampleFrame = boundary.sampleFrame,
                });
        } else {
            ++ztsPublishCount_;
            inputView_.control->hardwareTimeline.CountZtsPublication();
            if (publish.notifyConsumer && ztsAnchorReadyCallback_) {
                ztsAnchorReadyCallback_(publish.notificationGeneration);
            }
            ::ASFW::Audio::Runtime::ZtsTelemetryRecord record{};
            record.publishCount = ztsPublishCount_;
            record.sampleFrame = boundary.sampleFrame;
            record.hostTicks = boundary.hostTicks;
            record.rawHostTicks = packetHostTicks;
            record.drainHostTicks = batch.drainHostTicks;
            record.ageTicks = timestamp.ageTicks;
            record.drainCycleTimer = batch.drainCycleTimer;
            record.rxCycleTimer = timestamp.cycleTimer;
            record.descriptorIndex = packet.descriptorIndex;
            record.framesDecoded = result.framesDecoded;
            record.hostNanosPerSampleQ8 = boundary.hostNanosPerSampleQ8;
            record.rawRxTs = result.receiveCycleTimestamp;
            record.syt = result.syt;
            record.kind = static_cast<uint8_t>(ztsPublishCount_ == 1
                ? ::ASFW::Audio::Runtime::ZtsEventKind::kSeed
                : ::ASFW::Audio::Runtime::ZtsEventKind::kUpdate);
            ztsTelemetry_.Record(record);
        }
    }
}

// Compare where the cursor thinks it is against where the device's own SYT
// phase puts it, and correct the difference.
//
// The cursor is an accumulator: it only ever advances by frames that decoded.
// Every path that returns without advancing -- an empty completion, a rejected
// packet, a DMA drop the controller never reported -- silently shortens the
// timeline, and nothing downstream can tell a shortened timeline from a slow
// device. The SYT phase is the independent witness: in blocking mode it steps by
// a fixed amount per data packet, so phase and frame count measure the same
// quantity by two different routes and any gap between them is frames that did
// not arrive.
//
// This is not rate recovery. For a device whose SYT is a pure function of its
// own frame counter -- the Apogee Duet measurably is, to better than 0.0001 ppm
// over 13 minutes -- the phase contributes exactly zero information about rate.
// It contributes all of the information about loss, which is why the comparison
// is worth making on exactly such a device.
void DirectAudioReceiveConsumer::MeasureCursorAgainstPhase(
    const ::ASFW::Driver::RxSytCadence::Snapshot& cadence,
    int64_t currentPhaseTicks,
    uint64_t packetFirstFrame,
    uint32_t framesDecoded) noexcept {
    if (!inputView_.control) {
        return;
    }
    if (!cursorPhaseAnchorValid_) {
        cursorPhaseAnchorTicks_ = currentPhaseTicks;
        cursorPhaseAnchorFrame_ = packetFirstFrame;
        cursorPhaseAnchorValid_ = true;
        return;
    }

    const int64_t phaseDelta = ::ASFW::Timing::extOffsetDiff(
        currentPhaseTicks, cursorPhaseAnchorTicks_);
    int64_t expectedFrames = 0;
    if (!::ASFW::Driver::FramesForPhaseDelta(phaseDelta,
                                             cadence.rollingCadenceTicks,
                                             framesDecoded, expectedFrames)) {
        return;
    }

    const int64_t observedFrames =
        static_cast<int64_t>(packetFirstFrame) -
        static_cast<int64_t>(cursorPhaseAnchorFrame_);
    const int64_t drift = observedFrames - expectedFrames;

    // Re-anchor once per ZTS period. extOffsetDiff resolves an eight-second
    // domain and is only unambiguous well inside half of it, so the window has
    // to be bounded whether or not anything was wrong; one ZTS period is three
    // and a half orders of magnitude inside that limit and lines the reference
    // pair up with the anchor we publish.
    const bool windowElapsed =
        observedFrames >= static_cast<int64_t>(
            ::ASFW::Audio::Runtime::HardwareSampleTimeline::
                kZeroTimestampPeriodFrames);

    // One whole data packet is the smallest loss that can occur, so anything
    // below it is the integer division and not an event. Correcting on rounding
    // noise would walk the cursor.
    if (drift <= -static_cast<int64_t>(framesDecoded) ||
        drift >= static_cast<int64_t>(framesDecoded)) {
        pendingCursorCorrectionFrames_ = -drift;
        inputView_.control->rxCursorCorrections.fetch_add(
            1, std::memory_order_relaxed);
        inputView_.control->rxCursorCorrectedFrames.fetch_add(
            -drift, std::memory_order_relaxed);
        const int64_t magnitude = drift < 0 ? -drift : drift;
        int64_t worst = inputView_.control->rxCursorMaxCorrectionFrames.load(
            std::memory_order_relaxed);
        while (magnitude > worst &&
               !inputView_.control->rxCursorMaxCorrectionFrames
                    .compare_exchange_weak(worst, magnitude,
                                           std::memory_order_relaxed)) {
        }
        if (cursorCorrectionLogBudget_ != 0) {
            --cursorCorrectionLogBudget_;
            ASFW_LOG_ERROR(
                DirectAudio,
                "[RxCursor] drift=%lld frames observed=%lld expected=%lld "
                "phaseDelta=%lld cadence=%u seeds=%u frame=%llu",
                static_cast<long long>(drift),
                static_cast<long long>(observedFrames),
                static_cast<long long>(expectedFrames),
                static_cast<long long>(phaseDelta),
                cadence.rollingCadenceTicks, cadence.seedCount,
                static_cast<unsigned long long>(packetFirstFrame));
        }
        // The correction lands on the next packet, so the reference pair has to
        // move with it or the same gap would be re-reported for a whole window.
        cursorPhaseAnchorTicks_ = currentPhaseTicks;
        cursorPhaseAnchorFrame_ =
            static_cast<uint64_t>(static_cast<int64_t>(packetFirstFrame) - drift);
        return;
    }

    if (windowElapsed) {
        cursorPhaseAnchorTicks_ = currentPhaseTicks;
        cursorPhaseAnchorFrame_ = packetFirstFrame;
    }
}

bool DirectAudioReceiveConsumer::IsAcceptedHeaderOnlyNoDataTransition(
    const ::ASFW::Isoch::IsochReceivePacket& packet,
    const RxAudioPacketProcessorResult& result) const noexcept {
    return configuration_.acceptHeaderOnlyNoDataTransition &&
           !configuration_.isSecondary &&
           result.status == DirectRxWriteStatus::kZeroDataBlockSize &&
           result.hasValidCip && result.dbs == 0 && result.syt == 0xffff &&
           packet.payload.size() == kIsochReceivePrefixBytes + kCipHeaderBytes;
}

void DirectAudioReceiveConsumer::ObserveAcceptedHeaderOnlyNoDataTransition(
    const ::ASFW::Isoch::IsochReceiveBatch& batch,
    const ::ASFW::Isoch::IsochReceivePacket& packet,
    const RxAudioPacketProcessorResult& result) noexcept {
    // The packet carries no audio frame and must not enter the replay queue,
    // but its OHCI receive timestamp preserves the one-cycle continuity check
    // for the next usable packet. That is the precise narrow exception: do
    // not turn any other DBS=0 packet into a healthy stream.
    ::ASFW::Isoch::Rx::ExpandedReceiveTimestamp timestamp{};
    if (result.hasReceiveCycleTimestamp &&
        ::ASFW::Isoch::Rx::ExpandReceiveTimestamp(
            result.receiveCycleTimestamp, batch.drainCycleTimer, timestamp)) {
        const auto fields = ::ASFW::Timing::decodeCycleTimer(timestamp.cycleTimer);
        lastReplayCycleOrdinal_ =
            fields.seconds * ::ASFW::Timing::kCyclesPerSecond + fields.cycle;
        replayCycleInitialized_ = true;
    }

    if (headerOnlyNoDataTransitionLogBudget_ == 0) {
        return;
    }
    --headerOnlyNoDataTransitionLogBudget_;
    ASFW_LOG(DirectAudio,
             "[MAudioRxNoData] accepted desc=%u bytes=%zu drain=0x%08x rawTs=0x%04x "
             "syt=0x%04x remaining=%u",
             packet.descriptorIndex, packet.payload.size(), batch.drainCycleTimer,
             result.receiveCycleTimestamp, result.syt, headerOnlyNoDataTransitionLogBudget_);
}

void DirectAudioReceiveConsumer::LogReceivedWirePayload(
    const ::ASFW::Isoch::IsochReceivePacket& packet,
    const RxAudioPacketProcessorResult& result) noexcept {
    constexpr size_t kHeaderBytes = kIsochReceivePrefixBytes + kCipHeaderBytes;
    if (receivedWirePayloadLogBudget_ == 0 || result.dbs == 0 ||
        packet.payload.size() <= kHeaderBytes) {
        return;
    }

    const size_t dbs = result.dbs;
    const size_t payloadBytes = packet.payload.size() - kHeaderBytes;
    if (payloadBytes < dbs * sizeof(uint32_t)) {
        return;
    }
    const uint32_t channels = configuration_.streamChannels != 0
        ? configuration_.streamChannels
        : inputView_.memory.inputChannels;
    if (channels == 0 || channels > dbs) {
        return;
    }
    --receivedWirePayloadLogBudget_;

    const auto* blocks = packet.payload.data() + kHeaderBytes;
    const size_t events = payloadBytes / (dbs * sizeof(uint32_t));

    // Full label byte per slot of the first data block, slot 0 in the least
    // significant byte. A nibble is not enough: 0x40 (MBLA, accepted), 0x00 (raw
    // unlabelled) and 0x80 (MIDI) all share the low nibble 0, and telling them
    // apart is the entire point. Two words cover the 11-slot geometry.
    uint64_t labelsLow = 0;
    uint32_t labelsHigh = 0;
    for (size_t slot = 0; slot < dbs && slot < 12; ++slot) {
        const uint32_t label =
            (LoadBigEndianQuadlet(blocks + slot * sizeof(uint32_t)) >> 24U) & 0xFFU;
        if (slot < 8) {
            labelsLow |= static_cast<uint64_t>(label) << (slot * 8U);
        } else {
            labelsHigh |= label << ((slot - 8U) * 8U);
        }
    }

    // Peak magnitude taken WITHOUT consulting the label, exactly as Linux's
    // read_pcm_s32 does. If this is non-zero while the decoded audio is silent,
    // the label gate is eating real samples rather than the device sending none.
    uint32_t maxAbs24 = 0;
    uint32_t contentMask = 0;
    for (size_t event = 0; event < events; ++event) {
        const auto* block = blocks + event * dbs * sizeof(uint32_t);
        for (uint32_t ch = 0; ch < channels; ++ch) {
            const uint32_t quadlet = LoadBigEndianQuadlet(
                block + ch * sizeof(uint32_t));
            const uint32_t raw = quadlet & 0x00FFFFFFu;
            if (raw != 0) {
                contentMask |= 1U << ch;
            }
            int32_t sample = static_cast<int32_t>(raw);
            if ((sample & 0x800000) != 0) {
                sample |= static_cast<int32_t>(0xFF000000u);
            }
            const uint32_t magnitude = sample < 0
                ? static_cast<uint32_t>(-static_cast<int64_t>(sample))
                : static_cast<uint32_t>(sample);
            maxAbs24 = magnitude > maxAbs24 ? magnitude : maxAbs24;
        }
    }

    // `contentMask` bit N set means slot N carried a non-zero 24-bit value in
    // this packet. Cross it against the label of the same slot: a slot with
    // content whose label is not 0x40 is audio this decoder is discarding.
    ASFW_LOG(DirectAudio,
             "[RxWire] dbs=%u ch=%u events=%zu labels=0x%08x%016llx "
             "contentMask=0x%03x maxAbs24=%u syt=0x%04x",
             result.dbs, channels, events, labelsHigh,
             static_cast<unsigned long long>(labelsLow), contentMask, maxAbs24,
             result.syt);
}

void DirectAudioReceiveConsumer::LogZeroDataBlockSizeEvidence(
    const ::ASFW::Isoch::IsochReceiveBatch& batch,
    const ::ASFW::Isoch::IsochReceivePacket& packet,
    const RxAudioPacketProcessorResult& result) noexcept {
    // ProcessPacket has already proved that the span holds an 8-byte OHCI
    // receive prefix and an 8-byte CIP header. Keep this guard here so the
    // evidence path remains memory-safe if that contract ever changes.
    if (zeroDataBlockSizeCaptureLogBudget_ == 0 ||
        packet.payload.size() < kIsochReceivePrefixBytes + kCipHeaderBytes) {
        return;
    }

    const auto* bytes = packet.payload.data();
    const uint32_t prefix0 = LoadLittleEndianQuadlet(bytes);
    const uint32_t prefix1 = LoadLittleEndianQuadlet(bytes + sizeof(uint32_t));
    const uint32_t cip0 = LoadBigEndianQuadlet(bytes + kIsochReceivePrefixBytes);
    const uint32_t cip1 = LoadBigEndianQuadlet(
        bytes + kIsochReceivePrefixBytes + sizeof(uint32_t));
    const uint32_t sourceNodeId = (cip0 >> 24) & 0x3fu;
    const uint32_t expectedDbs = configuration_.am824Slots != 0
        ? configuration_.am824Slots
        : inputView_.deviceToHostAm824Slots;
    const uint32_t configuredChannels = configuration_.streamChannels != 0
        ? configuration_.streamChannels
        : inputView_.memory.inputChannels;
    const uint32_t capture = ++zeroDataBlockSizeCaptureCount_;
    --zeroDataBlockSizeCaptureLogBudget_;

    // `prefixLE` is the controller-owned 8-byte IR prefix, decoded as two
    // little-endian quadlets. `cipBE` is the following on-wire CIP header,
    // decoded as two big-endian quadlets. The pair makes a four-byte offset
    // error visible without asking the transport to understand CIP.
    ASFW_LOG_ERROR(
        DirectAudio,
        "[RxCipDbsZero] capture=%u desc=%u xfer=0x%04x residual=%u bytes=%zu "
        "drain=0x%08x rawTs=0x%04x tsValid=%u cfgDbs=%u channels=%u",
        capture, packet.descriptorIndex, packet.transferStatus, packet.residualCount,
        packet.payload.size(), batch.drainCycleTimer, result.receiveCycleTimestamp,
        result.hasReceiveCycleTimestamp ? 1U : 0U, expectedDbs, configuredChannels);
    ASFW_LOG_ERROR(
        DirectAudio,
        "[RxCipDbsZero] capture=%u prefixLE=[0x%08x,0x%08x] "
        "cipBE=[0x%08x,0x%08x] sid=%u dbs=%u dbc=%u fmt=0x%02x fdf=0x%02x syt=0x%04x",
        capture, prefix0, prefix1, cip0, cip1, sourceNodeId,
        result.dbs, result.dbc, static_cast<unsigned>((cip1 >> 24) & 0x3fu),
        result.fdf, result.syt);
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
    cadenceEstablishedLogged_ = false;
    replayCycleInitialized_ = false;
    dbcInitialized_ = false;
    // Before this was gated on `wasEstablished` alone, which made the one record
    // that explains a reset unreachable in the only case where nothing else
    // explains it: a stream that never established. A device that is rejected on
    // every packet, or that never sends one, produced total silence here. Emit a
    // bounded number of bring-up records too — budget is re-armed per start, so
    // a healthy stream still logs nothing and a stuck one cannot flood at the
    // 8 kHz packet rate.
    const bool logBootstrap = !wasEstablished && bootstrapResetLogBudget_ > 0;
    if (logBootstrap) {
        --bootstrapResetLogBudget_;
    }
    if (wasEstablished || logBootstrap) {
        ASFW_LOG_ERROR(
            DirectAudio,
            "[RxReplayReset] phase=%{public}s epoch=%llu reason=%{public}s desc=%u bytes=%u "
            "drain=0x%08x rawTs=0x%04x syt=0x%04x expectedCycle=%u observedCycle=%u "
            "status=%u frame=%llu validTs=%llu invalidTs=%llu",
            wasEstablished ? "established" : "bootstrap",
            resetEpoch, ReplayResetReasonName(reason), context.descriptorIndex,
            context.payloadBytes, context.drainCycleTimer, context.receiveCycleTimestamp,
            context.syt, context.expectedCycleOrdinal, context.observedCycleOrdinal,
            context.packetStatus, context.sampleFrame, timestampValidCount_, timestampInvalidCount_);
    }
    if (wasEstablished && timingLossCallback_) {
        if (control->hardwareTimeline.Source() ==
            ::ASFW::Audio::Runtime::HardwareTimelineSource::Receive) {
            control->RequestTimelineEpoch(
                ::ASFW::Audio::Runtime::
                    HardwareTimelineDiscontinuity::PresentationLoss);
        }
        timingLossCallback_();
    }
}

bool DirectAudioReceiveConsumer::IsReplayEstablished() const noexcept {
    const auto* control = inputView_.control;
    return control && control->rxSequenceReplay.IsEstablished();
}

void DirectAudioReceiveConsumer::PerformMaintenance(
    ::ASFW::Isoch::IsochConsumerMaintenanceKind kind,
    uint32_t budget) {
    switch (kind) {
        case ::ASFW::Isoch::IsochConsumerMaintenanceKind::kTelemetryDrain:
            DrainReceiveTelemetry(budget);
            break;
        case ::ASFW::Isoch::IsochConsumerMaintenanceKind::kTraceSnapshot:
            LogTransmitTimingTrace();
            break;
    }
}

void DirectAudioReceiveConsumer::DrainReceiveTelemetry(uint32_t maxRecords) {
    const uint32_t rate = inputView_.sampleRateHz;
    const uint64_t dropped = ztsTelemetry_.Drain(
        maxRecords, [this, rate](const ::ASFW::Audio::Runtime::ZtsTelemetryRecord& record) {
            if (!ztsTelemetryLogGate_.ShouldEmit(record, rate)) {
                return;
            }
            ASFW_LOG(Zts,
                     "%{public}s count=%llu frame=%llu host=%llu drainHost=%llu "
                     "drainCycle=0x%08x rxCycle=0x%08x age=%lld rawRxTs=0x%04x "
                     "syt=0x%04x desc=%u dec=%u rate=%u rateQ8=%u",
                     record.kind == static_cast<uint8_t>(::ASFW::Audio::Runtime::ZtsEventKind::kSeed)
                         ? "SEED" : "UPD",
                     record.publishCount, record.sampleFrame, record.hostTicks,
                     record.drainHostTicks, record.drainCycleTimer, record.rxCycleTimer,
                     record.ageTicks, record.rawRxTs, record.syt, record.descriptorIndex,
                     record.framesDecoded, rate, record.hostNanosPerSampleQ8);
        });
    if (dropped != 0) {
        ASFW_LOG(Zts, "drain overflow: dropped=%llu (capacity=%u)", dropped,
                 ::ASFW::Audio::Runtime::ZtsTelemetryRing::kCapacity);
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
