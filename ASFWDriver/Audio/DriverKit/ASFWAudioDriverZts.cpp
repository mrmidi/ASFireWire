// SPDX-License-Identifier: Apache-2.0
// Audio Engine V3: hardware-derived ZTS and explicit physical TX planning.

#include "ASFWAudioDevice.h"
#include "ASFWAudioDriverPrivate.hpp"
#include "../Wire/IEC61883/Syt.hpp"
#include "../../Common/TimingUtils.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/DriverKit.h>

#include <cstdint>
#include <limits>

namespace ASFW::Audio::DriverKit {
namespace {

using AmdtpDisposition =
    ASFW::Protocols::Audio::AMDTP::AmdtpPacketDisposition;
using TxPlan = ASFW::Protocols::Audio::AMDTP::TxPresentationPlan;
using PrepareResult = ASFW::Protocols::Audio::DICE::TxSlotPrepareResult;
using Timeline = ASFW::Audio::Runtime::HardwareSampleTimeline;

constexpr uint64_t kBusWrapTicks =
    static_cast<uint64_t>(ASFW::Timing::kFWTimeWrapSeconds) *
    ASFW::Timing::kTicksPerSecond;

// A replay read that fails because the reader and the producer have lost their
// shared frame of reference is recoverable by reseating the cursor, but the
// reader never deactivates itself, and Begin() is only attempted while it is
// inactive. Left alone the reader fails forever, every plan silently degrades
// to NO-DATA, and the stream stays up transmitting a frozen DBC.
//
// kAheadOfProducer is deliberately excluded: that is the ordinary case of TX
// running ahead of RX for a cycle, and it resolves itself on the next wake.
[[nodiscard]] bool IsRecoverableReplayDesync(
    ASFW::Audio::Runtime::RxSequenceReplayReadFailure failure) noexcept {
    using Failure = ASFW::Audio::Runtime::RxSequenceReplayReadFailure;
    switch (failure) {
    case Failure::kEpochChanged:
    case Failure::kHistoryOverwritten:
    case Failure::kSlotSequenceMismatch:
    case Failure::kSlotEpochMismatch:
    case Failure::kSlotChanged:
        return true;
    case Failure::kNone:
    case Failure::kReaderInactive:
    case Failure::kAheadOfProducer:
        return false;
    }
    return false;
}

[[nodiscard]] bool IsPowerOfTwo(uint64_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

void UpdateMaximum(std::atomic<uint64_t>& target, uint64_t value) noexcept {
    uint64_t previous = target.load(std::memory_order_relaxed);
    while (value > previous &&
           !target.compare_exchange_weak(previous, value,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
    }
}

[[nodiscard]] uint32_t HeadroomBucket(uint64_t cycles) noexcept {
    return cycles <= 6 ? 0 : cycles <= 12 ? 1 : cycles <= 24 ? 2
        : cycles <= 48 ? 3 : 4;
}

void TraceCycle(ASFW::Audio::Runtime::AudioTransportControlBlock& control,
                const TxPlan& plan,
                PrepareResult pcmResult,
                uint64_t completionCursor) noexcept {
    const uint64_t headroom = plan.cycleOrdinal > completionCursor
        ? plan.cycleOrdinal - completionCursor : 0;
    control.txDeadlineHeadroomHistogram[HeadroomBucket(headroom)].fetch_add(
        1, std::memory_order_relaxed);
    control.txCycleTrace.Publish({
        .epoch = plan.epoch,
        .cycleOrdinal = plan.cycleOrdinal,
        .firstAudioFrame = plan.firstAudioFrame,
        .presentationBusTicks = plan.presentationBusTicks,
        .prepareCycle = completionCursor,
        .publishCycle = completionCursor,
        .ownershipCycle = plan.cycleOrdinal >
                ASFW::Audio::Shared::AudioTimingGeometry::
                    kTxOwnershipGuardCycleSlots
            ? plan.cycleOrdinal -
                ASFW::Audio::Shared::AudioTimingGeometry::
                    kTxOwnershipGuardCycleSlots
            : 0,
        .completionCycle = 0,
        .frameCount = plan.frameCount,
        .pcmResult = static_cast<uint32_t>(pcmResult),
        .disposition = static_cast<uint32_t>(plan.disposition),
        .deadlineHeadroomCycles = headroom > UINT32_MAX
            ? UINT32_MAX : static_cast<uint32_t>(headroom),
    });
}

[[nodiscard]] bool UnwrapBusTicks(uint64_t rawTicks,
                                  bool& initialized,
                                  uint64_t& lastTicks,
                                  uint64_t& outTicks) noexcept {
    rawTicks %= kBusWrapTicks;
    uint64_t unwrapped = rawTicks;
    if (initialized) {
        unwrapped = (lastTicks / kBusWrapTicks) * kBusWrapTicks + rawTicks;
        if (unwrapped + kBusWrapTicks / 2 < lastTicks) {
            unwrapped += kBusWrapTicks;
        }
        if (unwrapped < lastTicks) return false;
    }
    initialized = true;
    lastTicks = unwrapped;
    outTicks = unwrapped;
    return true;
}

[[nodiscard]] uint16_t SytForPresentation(uint64_t busTicks) noexcept {
    const uint32_t cycle = static_cast<uint32_t>(
        (busTicks / ASFW::Timing::kTicksPerCycle) %
        ASFW::Timing::kCyclesPerSecond);
    const uint32_t offset = static_cast<uint32_t>(
        busTicks % ASFW::Timing::kTicksPerCycle);
    return ASFW::Protocols::Audio::IEC61883::SytFormatter::
        EncodeCycleOffset(cycle, offset);
}

[[nodiscard]] bool ExpandCompletionAndCorrelation(
    ASFWAudioDriver_IVars& ivars,
    uint32_t completionCycleTimer,
    uint32_t correlationCycleTimer,
    uint64_t& completionBusTicks,
    uint64_t& correlationBusTicks) noexcept {
    const auto completion = ASFW::Timing::decodeCycleTimer(
        completionCycleTimer);
    const auto correlation = ASFW::Timing::decodeCycleTimer(
        correlationCycleTimer);
    const uint64_t correlationRaw =
        static_cast<uint64_t>(correlation.seconds) *
            ASFW::Timing::kTicksPerSecond +
        static_cast<uint64_t>(correlation.cycle) *
            ASFW::Timing::kTicksPerCycle + correlation.offset;
    if (!UnwrapBusTicks(correlationRaw,
                        ivars.runtime.txObservationBusTicksValid,
                        ivars.runtime.lastTxObservationBusTicks,
                        correlationBusTicks)) {
        return false;
    }

    // OUTPUT_LAST exposes only seconds[2:0]. Lift it to the latest matching
    // eight-second window not later than the correlated controller read.
    uint32_t completionSeconds =
        (correlation.seconds & ~0x7U) | (completion.seconds & 0x7U);
    uint64_t completionRaw =
        static_cast<uint64_t>(completionSeconds) *
            ASFW::Timing::kTicksPerSecond +
        static_cast<uint64_t>(completion.cycle) *
            ASFW::Timing::kTicksPerCycle + completion.offset;
    constexpr uint64_t kEightSeconds =
        8ULL * ASFW::Timing::kTicksPerSecond;
    if (completionRaw > correlationRaw) {
        if (completionRaw < kEightSeconds) return false;
        completionRaw -= kEightSeconds;
    }
    const uint64_t age = correlationRaw - completionRaw;
    if (correlationBusTicks < age) return false;
    completionBusTicks = correlationBusTicks - age;
    // The next callback is ordered by completion, not by the slightly later
    // controller correlation read.
    ivars.runtime.lastTxObservationBusTicks = completionBusTicks;
    return true;
}

[[nodiscard]] bool IsPcmRetryable(PrepareResult result) noexcept {
    return result == PrepareResult::PcmNotYetPublished ||
           result == PrepareResult::PcmConcurrentRewrite;
}

[[nodiscard]] bool IsPcmFailure(PrepareResult result) noexcept {
    return result == PrepareResult::PcmNotYetPublished ||
           result == PrepareResult::PcmExpired ||
           result == PrepareResult::PcmWrongEpoch ||
           result == PrepareResult::PcmConcurrentRewrite ||
           result == PrepareResult::PcmInvalidRequest ||
           result == PrepareResult::PcmSourceUnavailable;
}

[[nodiscard]] ASFW::Audio::Runtime::TxContentFaultReason
ContentFaultReasonFor(PrepareResult result) noexcept {
    using Fault = ASFW::Audio::Runtime::TxContentFaultReason;
    switch (result) {
        case PrepareResult::PcmNotYetPublished:
            return Fault::kNotYetPublishedAtDeadline;
        case PrepareResult::PcmExpired:
            return Fault::kExpired;
        case PrepareResult::PcmConcurrentRewrite:
            return Fault::kConcurrentRewriteAtDeadline;
        case PrepareResult::PcmWrongEpoch:
        case PrepareResult::PcmInvalidRequest:
        case PrepareResult::PcmSourceUnavailable:
            return Fault::kInvalidSource;
        default:
            return Fault::kInvalidSource;
    }
}

void RecordMissedRange(ASFW::Audio::Runtime::AudioTransportControlBlock& control,
                       uint64_t packetIndex,
                       const TxPlan& plan,
                       PrepareResult reason,
                       uint64_t completionCursor,
                       uint64_t committedEnd) noexcept {
    const uint64_t count = control.txContentDeadlineNoData.fetch_add(
        1, std::memory_order_relaxed) + 1;
    control.counters.txPreparationDeadlineFaults.fetch_add(
        1, std::memory_order_relaxed);
    control.counters.txUnderruns.fetch_add(1, std::memory_order_relaxed);
    control.txMissedFrames.fetch_add(plan.frameCount,
                                     std::memory_order_relaxed);
    control.txContentFaultEvents.fetch_add(1, std::memory_order_relaxed);
    const auto faultReason = ContentFaultReasonFor(reason);
    uint32_t expected = static_cast<uint32_t>(
        ASFW::Audio::Runtime::TxContentFaultReason::kNone);
    if (control.txContentFirstFaultReason.compare_exchange_strong(
            expected,
            static_cast<uint32_t>(faultReason),
            std::memory_order_release, std::memory_order_relaxed)) {
        control.txContentFirstFaultPacket.store(packetIndex,
                                                 std::memory_order_relaxed);
        control.txContentFirstFaultAudioFrame.store(
            plan.firstAudioFrame, std::memory_order_relaxed);
        control.txContentFirstFaultOldestFrame.store(
            control.pcmPublicationTelemetry.oldestValidFrame.load(
                std::memory_order_relaxed), std::memory_order_relaxed);
        control.txContentFirstFaultWrittenEndFrame.store(
            control.pcmPublicationTelemetry.publishedEndFrame.load(
                std::memory_order_relaxed), std::memory_order_relaxed);
        control.txContentFirstFaultCompletionCursor.store(
            completionCursor, std::memory_order_relaxed);
        control.txContentFirstFaultCommittedEnd.store(
            committedEnd, std::memory_order_relaxed);
    }
    if (IsPowerOfTwo(count)) {
        ASFW_LOG_ERROR(
            DirectAudio,
            "[PcmCache] deadlineFailure=%llu result=%u epoch=%llu range=[%llu,%llu)",
            count, static_cast<uint32_t>(reason), plan.epoch,
            plan.firstAudioFrame, plan.firstAudioFrame + plan.frameCount);
        ASFW_LOG_ERROR(
            DirectAudio,
            "[TxDeadline] missed=%llu packet=%llu range=[%llu,%llu) pcm=%u cycle=%llu",
            count, packetIndex, plan.firstAudioFrame,
            plan.firstAudioFrame + plan.frameCount,
            static_cast<uint32_t>(reason), plan.cycleOrdinal);
    }
}

[[nodiscard]] bool PublishTimelineBoundary(
    ASFWAudioDriver_IVars& ivars,
    const ASFW::Audio::Runtime::HardwareZeroTimestamp& boundary,
    const char* source) noexcept {
    auto* control = ivars.runtime.directAudioGraph.control;
    if (!control) return false;
    const auto publication = control->PublishHostClockAnchor(
        boundary.sampleFrame, boundary.hostTicks,
        boundary.hostNanosPerSampleQ8);
    if (!publication.accepted) return false;
    control->counters.CountZtsPublished();
    control->hardwareTimeline.CountZtsPublication();
    (void)PublishSharedZeroTimestampToHAL(
        ivars, source, false, /*countAsRx=*/false);
    return true;
}

void HandlePendingTimelineEpoch(ASFWAudioDriver_IVars& ivars) noexcept {
    auto* control = ivars.runtime.directAudioGraph.control;
    if (!control) return;
    ASFW::Audio::Runtime::HardwareTimelineDiscontinuity reason{};
    if (!control->ConsumeTimelineEpochRequest(reason)) return;

    const uint64_t lastBoundary =
        control->hardwareTimeline.LastPublishedBoundary();
    const uint64_t baseFrame = Timeline::NextBoundaryAfter(lastBoundary);
    const uint32_t sampleRate = control->hardwareTimeline.SampleRateHz();
    const uint64_t epoch = control->hardwareTimeline.BeginEpoch(
        ASFW::Audio::Runtime::HardwareTimelineSource::Transmit,
        reason, sampleRate, baseFrame);
    if (epoch == 0) {
        const uint64_t failures =
            control->backendObservationConversionFailures.fetch_add(
                1, std::memory_order_relaxed) + 1;
        if (IsPowerOfTwo(failures)) {
            ASFW_LOG_ERROR(
                DirectAudio,
                "[TimelineEpoch] transitionFailure=%llu reason=%u rate=%u base=%llu",
                failures, static_cast<uint32_t>(reason), sampleRate,
                baseFrame);
        }
        return;
    }

    // Epoch invalidation is publication-only: backend cadence/DBC continues
    // in physical cycle order, while stale PCM identities become unreadable.
    ivars.runtime.pcmPublicationCache.BeginEpoch(epoch);
    control->txScheduledSampleFrame.store(baseFrame,
                                           std::memory_order_release);
    ASFW_LOG_ERROR(
        DirectAudio,
        "[TimelineEpoch] epoch=%llu reason=%u source=tx base=%llu lastBoundary=%llu rate=%u",
        epoch, static_cast<uint32_t>(reason), baseFrame, lastBoundary,
        sampleRate);
}

void ObserveTxHardware(ASFWAudioDriver_IVars& ivars,
                       uint64_t transportGeneration,
                       bool useMAudio) noexcept {
    auto* queue = ivars.runtime.txSlotProvider.queueControl;
    auto* control = ivars.runtime.directAudioGraph.control;
    if (!queue || !control || control->hardwareTimeline.Source() !=
            ASFW::Audio::Runtime::HardwareTimelineSource::Transmit) {
        return;
    }
    const uint64_t stampCount = queue->completionStampCount.load(
        std::memory_order_acquire);
    if (stampCount == 0) return;
    uint64_t packetIndex = 0;
    uint32_t completionCycleTimer = 0;
    if (!queue->ReadCompletionStamp(stampCount - 1, packetIndex,
                                    completionCycleTimer)) {
        return;
    }
    const auto* slot = ivars.runtime.txStreamEngine.Timeline().SlotByIndex(
        static_cast<uint32_t>(packetIndex));
    if (!slot || !slot->isData || slot->framesInPacket == 0 ||
        slot->epoch != control->hardwareTimeline.Epoch()) {
        // The M-Audio observer must still see group 2 even if that group's last
        // slot was NO-DATA. Use a zero-frame event for warm-up only.
        if (!useMAudio) return;
    }
    ASFW::Isoch::IsochTxClockPairSample pair{};
    if (!queue->clockPair.TryRead(pair) || pair.hostTimeMid == 0) return;
    uint64_t completionBusTicks = 0;
    uint64_t correlationBusTicks = 0;
    if (!ExpandCompletionAndCorrelation(
            ivars, completionCycleTimer, pair.cycleTimer32,
            completionBusTicks, correlationBusTicks)) {
        const uint64_t failures =
            control->backendObservationConversionFailures.fetch_add(
                1, std::memory_order_relaxed) + 1;
        if (IsPowerOfTwo(failures)) {
            ASFW_LOG_ERROR(
                DirectAudio,
                "[BackendTiming] conversionFailure=%llu completion=0x%08x correlation=0x%08x",
                failures, completionCycleTimer, pair.cycleTimer32);
        }
        return;
    }

    ASFW::Audio::Runtime::HardwarePresentationObservation observation{};
    bool ready = false;
    if (useMAudio) {
        const auto converted =
            ivars.runtime.mAudioPresentationObserver.ObserveHardwareWake(
                transportGeneration, completionBusTicks, correlationBusTicks,
                {.cycleTime = pair.cycleTimer32,
                 .hostTicks = pair.hostTimeMid},
                slot ? slot->firstAudioFrame : 0,
                slot ? slot->framesInPacket : 0);
        control->mAudioWarmupGroups.store(converted.groupCount,
                                          std::memory_order_relaxed);
        if (converted.captureReferencePlanted) {
            ASFW_LOG(DirectAudio,
                     "[MAudioTiming] capture-reference group=%u host=%llu",
                     converted.groupCount,
                     converted.captureReference.hostTicks);
        }
        ready = converted.observationReady;
        observation = converted.observation;
        if (ready) {
            control->mAudioTxDerivedObservations.fetch_add(
                1, std::memory_order_relaxed);
        }
    } else if (slot) {
        const uint32_t transfer = control->txTransferDelayTicks.load(
            std::memory_order_relaxed);
        observation = {
            .epoch = control->hardwareTimeline.Epoch(),
            .source = ASFW::Audio::Runtime::HardwareTimelineSource::Transmit,
            .sampleFrame = slot->firstAudioFrame,
            .frameCount = slot->framesInPacket,
            .presentationBusTicks = completionBusTicks + transfer,
            .correlationBusTicks = correlationBusTicks,
            .correlationHostTicks = pair.hostTimeMid,
        };
        ready = true;
    }
    if (!ready) return;
    control->backendObservationConversions.fetch_add(
        1, std::memory_order_relaxed);

    const uint64_t completionCursor = queue->completionCursor.load(
        std::memory_order_relaxed);
    if (slot) {
        control->txCycleTrace.Complete(
            slot->epoch, slot->cycleOrdinal, completionCursor);
    }
    const uint64_t completionLatency = completionCursor > packetIndex
        ? completionCursor - packetIndex : 0;
    UpdateMaximum(control->txCompletionLatencyMaxCycles, completionLatency);
    control->txCompletionLatencyHistogram[
        HeadroomBucket(completionLatency)].fetch_add(
            1, std::memory_order_relaxed);

    ASFW::Audio::Runtime::HardwareZeroTimestamp boundary{};
    const auto result = control->hardwareTimeline.Observe(observation,
                                                          &boundary);
    if (result ==
        ASFW::Audio::Runtime::HardwareObservationResult::BoundaryReady) {
        (void)PublishTimelineBoundary(
            ivars, boundary, useMAudio ? "maudio-tx" : "tx-fallback");
    } else if (result !=
                   ASFW::Audio::Runtime::HardwareObservationResult::Accepted &&
               result != ASFW::Audio::Runtime::
                             HardwareObservationResult::DuplicateBoundary) {
        const uint64_t failures =
            control->backendObservationConversionFailures.fetch_add(
                1, std::memory_order_relaxed) + 1;
        if (IsPowerOfTwo(failures)) {
            ASFW_LOG_ERROR(
                DirectAudio,
                "[BackendTiming] observationRejected=%llu result=%u epoch=%llu frame=%llu bus=%llu",
                failures, static_cast<uint32_t>(result), observation.epoch,
                observation.sampleFrame, observation.presentationBusTicks);
        }
    }
}

} // namespace

ASFW::Audio::Runtime::ZtsMirrorPublishResult PublishSharedZeroTimestampToHAL(
    ASFWAudioDriver_IVars& ivars,
    const char* reason,
    bool logSuccess,
    bool countAsRx) noexcept {
    auto* control = ivars.runtime.directAudioGraph.control;
    auto* audioDevice = ivars.audioDevice.get();
    if (!control || !audioDevice) {
        return ASFW::Audio::Runtime::ZtsMirrorPublishResult::NotReady;
    }
    const uint64_t lastGeneration =
        ivars.runtime.lastHalZeroTimestampGeneration.load(
            std::memory_order_acquire);
    ASFW::Audio::Runtime::HostClockAnchorSample anchor{};
    if (!control->hostClockAnchor.TryReadLatest(lastGeneration, anchor)) {
        return ASFW::Audio::Runtime::ZtsMirrorPublishResult::NoNewGeneration;
    }
    const bool first = ivars.runtime.lastHalZeroTimestampHostTicks.load(
        std::memory_order_relaxed) == 0;
    audioDevice->UpdateCurrentZeroTimestamp(anchor.sampleFrame,
                                            anchor.hostTicks);
    ivars.runtime.lastHalZeroTimestampSampleFrame.store(
        anchor.sampleFrame, std::memory_order_relaxed);
    ivars.runtime.lastHalZeroTimestampHostTicks.store(
        anchor.hostTicks, std::memory_order_relaxed);
    ivars.runtime.lastHalZeroTimestampGeneration.store(
        anchor.generation, std::memory_order_release);
    control->hostClockAnchor.mirrorPublications.fetch_add(
        1, std::memory_order_relaxed);
    if (countAsRx) control->counters.CountRxAdkZtsPublished();
    if (logSuccess || first) {
        ASFW_LOG(DirectAudio,
                 "[ZTS] %{public}s epoch=%llu sample=%llu host=%llu period=%u",
                 reason ? reason : "unknown",
                 control->hardwareTimeline.Epoch(), anchor.sampleFrame,
                 anchor.hostTicks, audioDevice->GetZeroTimestampPeriod());
    }
    return ASFW::Audio::Runtime::ZtsMirrorPublishResult::Published;
}

uint32_t PrepareTransmitSlots(ASFWAudioDriver_IVars& ivars,
                              uint64_t startPacketIndex,
                              uint64_t requiredPacketIndex,
                              bool useMAudioInternalTiming) noexcept {
    auto* control = ivars.runtime.directAudioGraph.control;
    auto* queue = ivars.runtime.txSlotProvider.queueControl;
    if (!control || !queue || ivars.runtime.txSlotProvider.numSlots == 0) {
        return 0;
    }
    const uint64_t epoch = control->hardwareTimeline.Epoch();
    uint64_t packetIndex = startPacketIndex;
    uint32_t prepared = 0;

    while (packetIndex < requiredPacketIndex &&
           prepared < ASFW::Audio::Shared::AudioTimingGeometry::
               kTxPreparedTargetCycleSlots) {
        // Kept as three separate outcomes so a rejection says which stage
        // produced it: no completion anchor at all, a negative anchor, or the
        // unwrapper refusing a backwards step against its high-water mark.
        int64_t normalizedTransmitTicks = 0;
        uint64_t transmitBusTicks = 0;
        const bool anchored = ivars.runtime.txExecutionTimeline.AnchorForPacket(
            packetIndex, normalizedTransmitTicks);
        const bool unwrapped = anchored && normalizedTransmitTicks >= 0 &&
            UnwrapBusTicks(
                static_cast<uint64_t>(normalizedTransmitTicks),
                ivars.runtime.txPlanBusTicksValid,
                ivars.runtime.lastTxPlanBusTicks,
                transmitBusTicks);
        const bool haveCycle = unwrapped;

        if (!haveCycle) {
            const uint64_t events = ++ivars.runtime.txNoCycleAnchorEvents;
            if (IsPowerOfTwo(events)) {
                ASFW_LOG_ERROR(
                    DirectAudio,
                    "[BackendTiming] noCycleAnchor=%llu packet=%llu anchored=%u raw=%lld lastPlanBus=%llu",
                    events, packetIndex, anchored ? 1u : 0u,
                    normalizedTransmitTicks,
                    ivars.runtime.lastTxPlanBusTicks);
            }
        }

        ASFW::Protocols::Audio::AMDTP::AmdtpTimingState timing{};
        timing.disposition = AmdtpDisposition::NoData;
        uint64_t presentationBusTicks = haveCycle ? transmitBusTicks : 0;
        bool replayPeeked = false;
        ASFW::Audio::Runtime::RxSequenceEntry replayEntry{};
        bool mAudioPlanActive = false;
        ASFW::Audio::Families::BeBoB::MAudio::InternalTxPacketPlan mAudioPlan{};

        if (useMAudioInternalTiming) {
            if (!ivars.runtime.mAudioInternalTxTiming.PreviewNextPacket(
                    mAudioPlan)) {
                break;
            }
            mAudioPlanActive = true;
            timing.hasExplicitPacketSchedule = true;
            timing.explicitDataBlocks = haveCycle && mAudioPlan.isData
                ? mAudioPlan.dataBlocks : 0;
            timing.disposition = timing.explicitDataBlocks != 0
                ? AmdtpDisposition::Data : AmdtpDisposition::NoData;
            if (timing.disposition == AmdtpDisposition::Data) {
                presentationBusTicks = transmitBusTicks +
                    mAudioPlan.sytOffsetTicks +
                    ivars.runtime.mAudioInternalTxTiming.TransferDelayTicks();
                timing.txClockValid = true;
                timing.nextDataSyt = SytForPresentation(presentationBusTicks);
            }
        } else {
            if (!ivars.runtime.txReplayReader.IsActive() &&
                control->rxSequenceReplay.IsEstablished()) {
                (void)ivars.runtime.txReplayReader.Begin(
                    control->rxSequenceReplay);
            }
            ASFW::Audio::Runtime::RxSequenceReplayReadDiagnostic replayDiag{};
            const bool peeked = ivars.runtime.txReplayReader.TryPeek(
                control->rxSequenceReplay, replayEntry, &replayDiag);
            if (!peeked && IsRecoverableReplayDesync(replayDiag.failure) &&
                control->rxSequenceReplay.IsEstablished() &&
                ivars.runtime.txReplayReader.Begin(control->rxSequenceReplay)) {
                // This packet stays NO-DATA; the reseated cursor serves the
                // next one. One cadence packet per resync is not observable.
                const uint64_t resyncs = ++ivars.runtime.txReplayResyncs;
                if (IsPowerOfTwo(resyncs)) {
                    ASFW_LOG_ERROR(
                        DirectAudio,
                        "[BackendTiming] replayResync=%llu packet=%llu failure=%{public}s reader=%llu producer=%llu readerEpoch=%u replayEpoch=%u",
                        resyncs, packetIndex,
                        ASFW::Audio::Runtime::RxSequenceReplayReadFailureName(
                            replayDiag.failure),
                        replayDiag.readerCursor, replayDiag.producerCursor,
                        replayDiag.readerEpoch, replayDiag.replayEpoch);
                }
            }
            if (peeked) {
                replayPeeked = true;
                control->txReplayEntries.fetch_add(1,
                                                    std::memory_order_relaxed);
                timing.replayValid = true;
                timing.replayDataBlocks = replayEntry.dataBlocks;
                const bool replayData = haveCycle &&
                    replayEntry.dataBlocks != 0 &&
                    (replayEntry.flags &
                     ASFW::Audio::Runtime::RxSequenceFlags::kValidSyt) != 0 &&
                    replayEntry.sytOffset !=
                        ASFW::Audio::Runtime::RxSequenceReplayState::kNoInfo;
                if (replayEntry.dataBlocks != 0 && !replayData) {
                    control->txReplayInvalidSyt.fetch_add(
                        1, std::memory_order_relaxed);
                    control->backendSytDiscontinuities.fetch_add(
                        1, std::memory_order_relaxed);
                }
                timing.disposition = replayData
                    ? AmdtpDisposition::Data : AmdtpDisposition::NoData;
                if (replayData) {
                    presentationBusTicks = transmitBusTicks +
                        replayEntry.sytOffset +
                        control->txTransferDelayTicks.load(
                            std::memory_order_relaxed);
                    timing.txClockValid = true;
                    timing.nextDataSyt = SytForPresentation(
                        presentationBusTicks);
                }
            } else if (control->hardwareTimeline.Source() ==
                           ASFW::Audio::Runtime::HardwareTimelineSource::Transmit &&
                       haveCycle) {
                // Output-only backend fallback. Cadence comes from the engine;
                // the packet's own physical cycle supplies presentation time.
                timing.disposition = AmdtpDisposition::Data;
                presentationBusTicks = transmitBusTicks +
                    control->txTransferDelayTicks.load(
                        std::memory_order_relaxed);
                timing.txClockValid = true;
                timing.nextDataSyt = SytForPresentation(presentationBusTicks);
            } else if (control->rxSequenceReplay.IsEstablished()) {
                control->txReplayUnderflows.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }

        TxPlan plan{};
        uint8_t wireBlocks = 0;
        uint16_t syt = 0xFFFF;
        if (!ivars.runtime.txStreamEngine.PreviewPresentationPlan(
                epoch, packetIndex, control->hardwareTimeline.NextTxFrame(),
                presentationBusTicks, timing, plan, wireBlocks, syt)) {
            break;
        }

        ASFW::Audio::Runtime::TxPresentationRange range{};
        if (plan.disposition == AmdtpDisposition::Data) {
            if (!control->hardwareTimeline.PreviewTxRange(
                    epoch, presentationBusTicks, plan.frameCount, range)) {
                // No observed hardware origin yet. This physical cycle is an
                // ordinary startup NO-DATA slot and consumes no content time.
                // Sustained counts mean the plan's bus time never reconciles
                // with the timeline's observations -- a domain fault, not a
                // startup transient -- so attribute it rather than dropping it
                // silently before the PCM cache is ever consulted.
                const uint64_t events =
                    ++ivars.runtime.txNoPresentationOriginEvents;
                if (IsPowerOfTwo(events)) {
                    const uint64_t observedBus =
                        control->hardwareTimeline.LastObservationBusTicks();
                    ASFW_LOG_ERROR(
                        DirectAudio,
                        "[BackendTiming] noPresentationOrigin=%llu packet=%llu presentBus=%llu observedBus=%llu lead=%lld obsValid=%u txCursor=%u obsFrame=%llu frames=%u source=%u",
                        events, packetIndex, presentationBusTicks, observedBus,
                        static_cast<int64_t>(presentationBusTicks) -
                            static_cast<int64_t>(observedBus),
                        control->hardwareTimeline.ObservationValid() ? 1u : 0u,
                        control->hardwareTimeline.TxCursorInitialized() ? 1u : 0u,
                        control->hardwareTimeline.LastObservationFrame(),
                        plan.frameCount,
                        static_cast<uint32_t>(
                            control->hardwareTimeline.Source()));
                }
                plan.disposition = AmdtpDisposition::NoData;
                plan.frameCount = 0;
                wireBlocks = 0;
                syt = 0xFFFF;
            } else {
                plan.firstAudioFrame = range.firstAudioFrame;
            }
        }

        const uint64_t completionCursor = queue->completionCursor.load(
            std::memory_order_acquire);
        PrepareResult result = ivars.runtime.txStreamEngine.PrepareTransmitSlot(
            static_cast<uint32_t>(packetIndex), plan, wireBlocks, syt);
        const PrepareResult initialResult = result;
        if (IsPcmFailure(result) && plan.frameCount != 0) {
            const uint64_t completion = queue->completionCursor.load(
                std::memory_order_acquire);
            const uint64_t headroom = packetIndex > completion
                ? packetIndex - completion : 0;
            const bool deadlineExpired = headroom <=
                ASFW::Audio::Shared::AudioTimingGeometry::
                    kTxOwnershipGuardCycleSlots;
            if (IsPcmRetryable(result) && !deadlineExpired) {
                control->txContentDeferrals.fetch_add(
                    1, std::memory_order_relaxed);
                TraceCycle(*control, plan, result, completionCursor);
                break;
            }
            const PrepareResult pcmFailure = result;
            TxPlan missed = plan;
            missed.disposition = AmdtpDisposition::NoData;
            result = ivars.runtime.txStreamEngine.PrepareTransmitSlot(
                static_cast<uint32_t>(packetIndex), missed, 0, 0xFFFF);
            if (result != PrepareResult::Prepared) break;
            if (ivars.runtime.txSecondaryActive &&
                ivars.runtime.txStreamEngineSecondary.PrepareTransmitSlot(
                    static_cast<uint32_t>(packetIndex), missed, 0, 0xFFFF) !=
                    PrepareResult::Prepared) {
                break;
            }
            plan = missed;
            RecordMissedRange(
                *control, packetIndex, plan, pcmFailure,
                completionCursor,
                queue->committedEnd.load(std::memory_order_acquire));
        } else if (result != PrepareResult::Prepared) {
            break;
        } else if (ivars.runtime.txSecondaryActive &&
                   ivars.runtime.txStreamEngineSecondary.PrepareTransmitSlot(
                       static_cast<uint32_t>(packetIndex), plan,
                       wireBlocks, syt) != PrepareResult::Prepared) {
            break;
        }

        if (plan.frameCount != 0) {
            range = {
                .epoch = plan.epoch,
                .firstAudioFrame = plan.firstAudioFrame,
                .frameCount = plan.frameCount,
                .presentationBusTicks = plan.presentationBusTicks,
            };
            if (!control->hardwareTimeline.CommitTxRange(range)) break;
            const uint64_t nextTxFrame =
                control->hardwareTimeline.NextTxFrame();
            control->txScheduledSampleFrame.store(
                nextTxFrame, std::memory_order_release);
            // How far the CoreAudio writer is staged beyond the frame the next
            // packet will consume. Committed margin cannot see this: it
            // measures our headroom against our own transmit deadline, so it
            // can read healthy while the writer has nothing left to give.
            const uint64_t publishedEnd =
                ivars.runtime.pcmPublicationCache.PublishedEndFrame();
            control->RecordProducerHeadroom(
                publishedEnd > nextTxFrame ? publishedEnd - nextTxFrame : 0);
        }
        if (mAudioPlanActive &&
            !ivars.runtime.mAudioInternalTxTiming.CommitPacket(
                mAudioPlan,
                plan.disposition == AmdtpDisposition::Data)) {
            break;
        }
        if (replayPeeked) ivars.runtime.txReplayReader.Advance();

        TraceCycle(*control, plan, initialResult, completionCursor);

        control->counters.txPackets.fetch_add(1, std::memory_order_relaxed);
        if (plan.disposition == AmdtpDisposition::Data) {
            control->counters.txDataPackets.fetch_add(
                1, std::memory_order_relaxed);
            control->counters.txValidSytPackets.fetch_add(
                1, std::memory_order_relaxed);
            control->counters.txPcmFramesEncoded.fetch_add(
                plan.frameCount, std::memory_order_relaxed);
        } else {
            control->counters.txNoDataPackets.fetch_add(
                1, std::memory_order_relaxed);
            control->counters.txSytFfffPackets.fetch_add(
                1, std::memory_order_relaxed);
        }
        ++packetIndex;
        ++prepared;
    }
    return prepared;
}

void RepublishTxRingForRestart(ASFWAudioDriver_IVars& ivars) noexcept {
    // A recovery restart re-arms the already-prepared transmit context, so the
    // StartIO prefill does not run again. Without this the producer's committed
    // cursor still holds the dead stream's high-water mark and the descriptor
    // prime rejects it ("committed prefill=751 must cover 48 descriptors within
    // 192 slots"), turning one TX fault into a stream that can never restart.
    //
    // Runs before ASFWAudioNub::RecoverAudioStreamingAfterTxFault() so the ring
    // is republished by the time the coordinator re-arms the context.
    if (auto* queue = ivars.runtime.txSlotProvider.queueControl) {
        queue->ResetProducerForStart();
    }
    if (auto* queue2 = ivars.runtime.txSlotProviderSecondary.queueControl) {
        queue2->ResetProducerForStart();
    }
    ivars.runtime.txStreamEngine.ResetForStart(0);
    if (ivars.runtime.txSecondaryActive) {
        ivars.runtime.txStreamEngineSecondary.ResetForStart(0);
    }
    // The new stream re-derives its bus-time origin; carrying the old
    // high-water mark would reject every anchor until it caught up.
    ivars.runtime.txPlanBusTicksValid = false;
    ivars.runtime.lastTxPlanBusTicks = 0;
    ivars.runtime.txObservationBusTicksValid = false;
    ivars.runtime.lastTxObservationBusTicks = 0;
    PrefillTxRingBeforeStart(ivars);
}

void PrefillTxRingBeforeStart(ASFWAudioDriver_IVars& ivars) noexcept {
    const uint32_t slots = ivars.runtime.txSlotProvider.numSlots;
    auto* control = ivars.runtime.directAudioGraph.control;
    if (slots == 0 || !control) return;
    const TxPlan noData{
        .epoch = control->hardwareTimeline.Epoch(),
        .cycleOrdinal = 0,
        .firstAudioFrame = 0,
        .frameCount = 0,
        .presentationBusTicks = 0,
        .disposition = AmdtpDisposition::NoData,
    };
    uint32_t prepared = 0;
    for (uint32_t packet = 0; packet < slots; ++packet) {
        TxPlan plan = noData;
        plan.cycleOrdinal = packet;
        if (ivars.runtime.txStreamEngine.PrepareTransmitSlot(
                packet, plan, 0, 0xFFFF) != PrepareResult::Prepared) {
            break;
        }
        if (ivars.runtime.txSecondaryActive &&
            ivars.runtime.txStreamEngineSecondary.PrepareTransmitSlot(
                packet, plan, 0, 0xFFFF) != PrepareResult::Prepared) {
            break;
        }
        ++prepared;
    }
    ASFW_LOG(DirectAudio,
             "[TxV3] prefill=%u/%u preparedTarget=%u ownershipGuard=%u",
             prepared, slots,
             ASFW::Audio::Shared::AudioTimingGeometry::
                 kTxPreparedTargetCycleSlots,
             ASFW::Audio::Shared::AudioTimingGeometry::
                 kTxOwnershipGuardCycleSlots);
}

} // namespace ASFW::Audio::DriverKit

void IMPL(ASFWAudioDriver, ZtsAnchorReady) {
    (void)action;
    (void)generation;
    if (!ivars || !ivars->audioDevice) return;
    (void)ASFW::Audio::DriverKit::PublishSharedZeroTimestampToHAL(
        *ivars, "rx", false);
}

void IMPL(ASFWAudioDriver, TxPreparationReady) {
    (void)action;
    (void)generation;
    if (!ivars || !ivars->runtime.txActive.load(std::memory_order_acquire)) {
        return;
    }
    auto* queue = ivars->runtime.txSlotProvider.queueControl;
    auto* control = ivars->runtime.directAudioGraph.control;
    if (!queue || !control) return;

    const uint64_t requested = queue->refillRequestGeneration.load(
        std::memory_order_acquire);
    const uint64_t handled = queue->refillHandledGeneration.load(
        std::memory_order_acquire);
    const bool hardwareWake = requested != handled;
    const uint64_t completion = queue->completionCursor.load(
        std::memory_order_acquire);
    const uint64_t committedBefore = queue->committedEnd.load(
        std::memory_order_acquire);
    const uint64_t target = completion +
        ASFW::Audio::Shared::AudioTimingGeometry::
            kTxPreparedTargetCycleSlots;
    const bool useMAudio =
        ASFW::Audio::Families::BeBoB::MAudio::UsesSpecialDuplexPolicy(
            ivars->resolvedProfile.Value().profileBuilder);

    ASFW::Audio::DriverKit::HandlePendingTimelineEpoch(*ivars);

    if (hardwareWake) {
        ASFW::Audio::DriverKit::ObserveTxHardware(
            *ivars, requested, useMAudio);
    }
    const uint32_t prepared =
        ASFW::Audio::DriverKit::PrepareTransmitSlots(
            *ivars, committedBefore, target,
            useMAudio && ivars->runtime.mAudioInternalTxTiming.IsArmed());
    const uint64_t committedAfter = queue->committedEnd.load(
        std::memory_order_acquire);
    const uint64_t margin = committedAfter > completion
        ? committedAfter - completion : 0;
    ASFW::Audio::DriverKit::UpdateMaximum(
        control->txPacketStoreHighWaterPackets, margin);
    control->txTransportCompletionCursor.store(completion,
                                                std::memory_order_relaxed);
    control->txTransportCommittedEnd.store(committedAfter,
                                            std::memory_order_relaxed);
    control->txCurrentCommittedMarginPackets.store(
        margin > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(margin),
        std::memory_order_relaxed);
    control->RecordCommittedMargin(margin);
    control->counters.txPreparationWakeDispatches.fetch_add(
        1, std::memory_order_relaxed);
    control->counters.txPreparationDrainPasses.fetch_add(
        1, std::memory_order_relaxed);

    const uint64_t now = mach_absolute_time();
    const uint64_t lastHeartbeat = control->txHeartbeatLastHostTicks.load(
        std::memory_order_relaxed);
    if (lastHeartbeat == 0 || now <= lastHeartbeat ||
        ASFW::Timing::hostTicksToNanos(now - lastHeartbeat) >=
            5'000'000'000ULL) {
        control->txHeartbeatLastHostTicks.store(now,
                                                std::memory_order_relaxed);
        // The engine owns the per-packet count; mirror it into the shared
        // block once per heartbeat so telemetry and STOPIO report it without
        // touching the hot path.
        control->counters.txSilenceSubstitutions.store(
            ivars->runtime.txStreamEngine.Counters()
                .pcmSilenceSubstitutions.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        ASFW_LOG(DirectAudio,
                 "[TxV3] epoch=%llu source=%u completion=%llu committed=%llu margin=%llu prepared=%u nextFrame=%llu cache=[%llu,%llu) noCycle=%llu noOrigin=%llu resync=%llu",
                 control->hardwareTimeline.Epoch(),
                 static_cast<uint32_t>(control->hardwareTimeline.Source()),
                 completion, committedAfter, margin, prepared,
                 control->hardwareTimeline.NextTxFrame(),
                 ivars->runtime.pcmPublicationCache.OldestValidFrame(),
                 ivars->runtime.pcmPublicationCache.PublishedEndFrame(),
                 ivars->runtime.txNoCycleAnchorEvents,
                 ivars->runtime.txNoPresentationOriginEvents,
                 ivars->runtime.txReplayResyncs);

        // [TxPrep] is the scheduling half of the heartbeat: how late the
        // preparation pass ran behind the CoreAudio request that asked for it,
        // and how much runway that left. Output safety must be derived from
        // this distribution rather than from the plan horizon, so it stays on
        // the coarse 5 s heartbeat rather than being anomaly-gated.
        control->CompleteTxInterval();
        const uint64_t latencyMaxTicks =
            control->txCompletedIntervalPreparationLatencyMaxTicks.load(
                std::memory_order_relaxed);
        const uint32_t marginMin =
            control->txCompletedIntervalMarginMinPackets.load(
                std::memory_order_relaxed);
        // Interval minimum, re-armed by CompleteTxInterval above. The
        // since-start watermark latches 0 during startup -- before the first
        // WriteEnd there is nothing staged -- and never recovers, so it says
        // nothing about the running stream.
        const uint32_t headroomMin =
            control->txCompletedIntervalProducerHeadroomMinFrames.load(
                std::memory_order_relaxed);
        const uint32_t headroomRunMin =
            control->txMinimumProducerHeadroomFrames.load(
                std::memory_order_relaxed);
        ASFW_LOG(DirectAudio,
                 "[TxPrep] wakes=%llu maxLatUs=%llu le750=%llu ge1500=%llu lat=[%llu,%llu,%llu,%llu,%llu,%llu] marginMin=%u marginMax=%u margin=[%llu,%llu,%llu,%llu,%llu] headroomMin=%u headroomRunMin=%u",
                 control->txPreparationLatencySamples.load(
                     std::memory_order_relaxed),
                 ASFW::Timing::hostTicksToNanos(latencyMaxTicks) /
                     ASFW::Audio::Shared::AudioTimingGeometry::
                         kNanosecondsPerMicrosecond,
                 control->txPreparationAtMost750Us.load(
                     std::memory_order_relaxed),
                 control->txPreparationAtLeast1500Us.load(
                     std::memory_order_relaxed),
                 control->txCompletedIntervalPreparationLatencyHistogram[0]
                     .load(std::memory_order_relaxed),
                 control->txCompletedIntervalPreparationLatencyHistogram[1]
                     .load(std::memory_order_relaxed),
                 control->txCompletedIntervalPreparationLatencyHistogram[2]
                     .load(std::memory_order_relaxed),
                 control->txCompletedIntervalPreparationLatencyHistogram[3]
                     .load(std::memory_order_relaxed),
                 control->txCompletedIntervalPreparationLatencyHistogram[4]
                     .load(std::memory_order_relaxed),
                 control->txCompletedIntervalPreparationLatencyHistogram[5]
                     .load(std::memory_order_relaxed),
                 marginMin == UINT32_MAX ? 0u : marginMin,
                 control->txCompletedIntervalMarginMaxPackets.load(
                     std::memory_order_relaxed),
                 control->txCompletedIntervalCommittedMarginHistogram[0]
                     .load(std::memory_order_relaxed),
                 control->txCompletedIntervalCommittedMarginHistogram[1]
                     .load(std::memory_order_relaxed),
                 control->txCompletedIntervalCommittedMarginHistogram[2]
                     .load(std::memory_order_relaxed),
                 control->txCompletedIntervalCommittedMarginHistogram[3]
                     .load(std::memory_order_relaxed),
                 control->txCompletedIntervalCommittedMarginHistogram[4]
                     .load(std::memory_order_relaxed),
                 headroomMin == UINT32_MAX ? 0u : headroomMin,
                 headroomRunMin == UINT32_MAX ? 0u : headroomRunMin);
    }
    if (committedAfter < target) {
        const uint64_t shortageCount =
            control->counters.txPreparedTargetShortfalls.fetch_add(
                1, std::memory_order_relaxed) + 1;
        if (ASFW::Audio::DriverKit::IsPowerOfTwo(shortageCount)) {
            ASFW_LOG_ERROR(DirectAudio,
                           "[TxOwnership] short=%llu completion=%llu committed=%llu target=%llu prepared=%u",
                           shortageCount, completion, committedAfter, target,
                           prepared);
        }
    }

    queue->MarkRefillHandled(requested);
    const uint64_t audioGeneration =
        control->txPreparationRequests.RequestedGeneration();
    const uint64_t pendingRequestTicks =
        control->txPreparationRequests.MarkHandled(audioGeneration, now);
    if (pendingRequestTicks != 0 && now > pendingRequestTicks) {
        const uint64_t latencyTicks = now - pendingRequestTicks;
        control->RecordPreparationLatency(
            latencyTicks,
            ASFW::Timing::hostTicksToNanos(latencyTicks) /
                ASFW::Audio::Shared::AudioTimingGeometry::
                    kNanosecondsPerMicrosecond);
    }
    control->txPreparationRequests.FinishWake();
    if (control->txPreparationRequests.NeedsHandling() &&
        control->txPreparationRequests.TryScheduleWake() &&
        ivars->device.audioNub) {
        if (ivars->device.audioNub->RequestTxPreparation(
                control->txPreparationRequests.RequestedGeneration()) !=
            kIOReturnSuccess) {
            control->txPreparationRequests.FinishWake();
        }
    }
}
