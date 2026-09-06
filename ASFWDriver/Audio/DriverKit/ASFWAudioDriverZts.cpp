// SPDX-License-Identifier: Apache-2.0
// Audio Engine V3: hardware-derived ZTS and explicit physical TX planning.

#include "ASFWAudioDevice.h"
#include "ASFWAudioDriverPrivate.hpp"
#include "../Wire/IEC61883/Syt.hpp"
#include "../Runtime/TxCompletionStampDrain.hpp"
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
using FillResult = ASFW::Protocols::Audio::DICE::TxSlotFillResult;

// One traversal of the OHCI IT descriptor ring, expressed in audio frames.
// 48 packets at the blocking cadence's 6 frames/packet average = 288, which is
// both the configured TX lead and the quantum the measured RTL displacement
// lands on. The alignment probe reports its step in these units so a whole-lap
// divergence is legible without arithmetic at the log.
inline constexpr uint32_t kTxRingFrames =
    ASFW::Shared::Isoch::IsochQueueGeometry::kTransmitInFlightPackets *
    (ASFW::Audio::Shared::AudioTimingGeometry::kCadenceBlockFrames /
     ASFW::Audio::Shared::AudioTimingGeometry::kCadenceBlockPackets);
static_assert(ASFW::Audio::Shared::AudioTimingGeometry::kCadenceBlockFrames %
                      ASFW::Audio::Shared::AudioTimingGeometry::
                          kCadenceBlockPackets ==
                  0,
              "cadence block must divide into whole frames per packet");

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
    // One implementation of the OUTPUT_LAST seconds[2:0] lift, shared with the
    // TX plan path. This used to carry its own copy whose only correction was
    // `completionRaw > correlationRaw`, with no tolerance at all -- so the
    // ordinary publish race (clockPair from the top of a refill pass, the
    // stamp from its bottom) could push an observation a full eight seconds
    // into the past.
    int64_t completionTicks = 0;
    int64_t correlationTicks = 0;
    ASFW::Audio::Shared::LiftCompletionAgainstCorrelation(
        completionCycleTimer, correlationCycleTimer,
        completionTicks, correlationTicks);

    if (!UnwrapBusTicks(static_cast<uint64_t>(correlationTicks),
                        ivars.runtime.txObservationBusTicksValid,
                        ivars.runtime.lastTxObservationBusTicks,
                        correlationBusTicks)) {
        return false;
    }

    // `age` is signed: a completion may sit microseconds AFTER its correlation
    // when the stamp comes from a newer pass than the clockPair read.
    const int64_t age = correlationTicks - completionTicks;
    const int64_t completionSigned =
        static_cast<int64_t>(correlationBusTicks) - age;
    if (completionSigned < 0) return false;
    completionBusTicks = static_cast<uint64_t>(completionSigned);
    // The next callback is ordered by completion, not by the slightly later
    // controller correlation read.
    ivars.runtime.lastTxObservationBusTicks = completionBusTicks;
    return true;
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

/// Expand one completion stamp against this wake's correlation anchor.
/// Failure here is a real clock fault; a stamp whose packet simply carries no
/// audio is not a failure and is filtered by the callers.
[[nodiscard]] bool ExpandCompletionStamp(
    ASFWAudioDriver_IVars& ivars,
    ASFW::Isoch::IsochTxQueueControl* queue,
    ASFW::Audio::Runtime::AudioTransportControlBlock* control,
    uint64_t stampIndex,
    uint32_t correlationCycleTimer,
    uint64_t& outPacketIndex,
    uint64_t& outCompletionBusTicks,
    uint64_t& outCorrelationBusTicks) noexcept {
    uint32_t completionCycleTimer = 0;
    if (!queue->ReadCompletionStamp(stampIndex, outPacketIndex,
                                    completionCycleTimer)) {
        return false;
    }
    if (ExpandCompletionAndCorrelation(ivars, completionCycleTimer,
                                       correlationCycleTimer,
                                       outCompletionBusTicks,
                                       outCorrelationBusTicks)) {
        return true;
    }
    const uint64_t failures =
        control->backendObservationConversionFailures.fetch_add(
            1, std::memory_order_relaxed) + 1;
    if (IsPowerOfTwo(failures)) {
        ASFW_LOG_ERROR(
            DirectAudio,
            "[BackendTiming] conversionFailure=%llu completion=0x%08x correlation=0x%08x",
            failures, completionCycleTimer, correlationCycleTimer);
    }
    return false;
}

/// Submit one packet's presentation observation and publish any ZTS boundary it
/// crosses.
void SubmitTxObservation(
    ASFWAudioDriver_IVars& ivars,
    ASFW::Audio::Runtime::AudioTransportControlBlock* control,
    const ASFW::Audio::Runtime::HardwarePresentationObservation& observation,
    const char* reason) noexcept {
    control->backendObservationConversions.fetch_add(
        1, std::memory_order_relaxed);
    ASFW::Audio::Runtime::HardwareZeroTimestamp boundary{};
    const auto result = control->hardwareTimeline.Observe(observation,
                                                          &boundary);
    if (result ==
        ASFW::Audio::Runtime::HardwareObservationResult::BoundaryReady) {
        (void)PublishTimelineBoundary(ivars, boundary, reason);
        return;
    }
    if (result == ASFW::Audio::Runtime::HardwareObservationResult::Accepted ||
        result ==
            ASFW::Audio::Runtime::HardwareObservationResult::DuplicateBoundary) {
        return;
    }
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

/// One ledger interval, on the coarse heartbeat. The ledger's rule is that a
/// nominal is not an observation, so the line always carries the sample count
/// and the unresolved count beside the distribution: a narrow spread over three
/// samples, or one that dropped most of its observations, must not read like a
/// well-behaved interval.
void LogLedgerInterval(const char* name,
                       const ASFW::Audio::Runtime::LedgerIntervalStats& stats)
    noexcept {
    const uint64_t samples = stats.samples.load(std::memory_order_relaxed);
    const uint64_t unresolved = stats.unresolved.load(std::memory_order_relaxed);
    const uint64_t invalid = stats.invalid.load(std::memory_order_relaxed);
    if (samples == 0 && unresolved == 0 && invalid == 0 &&
        stats.pending.load(std::memory_order_relaxed) == 0) {
        return;
    }
    const uint64_t minMicros = stats.minMicros.load(std::memory_order_relaxed);
    // `inv` is the reversed-endpoint count: both ends were recovered and the
    // span still did not order, which means one of them is stamped at the wrong
    // event. n+pend+unres+inv is every candidate the site considered.
    ASFW_LOG(DirectAudio,
             "[Ledger] %{public}s n=%llu pend=%llu unres=%llu inv=%llu min=%llu mean=%llu max=%llu us hist=[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu]",
             name, samples, stats.pending.load(std::memory_order_relaxed),
             unresolved, invalid,
             samples == 0 ? 0 : minMicros,
             stats.MeanMicros(),
             stats.maxMicros.load(std::memory_order_relaxed),
             stats.histogram[0].load(std::memory_order_relaxed),
             stats.histogram[1].load(std::memory_order_relaxed),
             stats.histogram[2].load(std::memory_order_relaxed),
             stats.histogram[3].load(std::memory_order_relaxed),
             stats.histogram[4].load(std::memory_order_relaxed),
             stats.histogram[5].load(std::memory_order_relaxed),
             stats.histogram[6].load(std::memory_order_relaxed),
             stats.histogram[7].load(std::memory_order_relaxed));
}

/// Bus ticks run at 24.576 MHz; host ticks are converted through the timebase.
[[nodiscard]] constexpr uint64_t BusTicksToMicros(uint64_t ticks) noexcept {
    return ticks / 24U;  // 24.576 ticks/us; the truncation is under 3%.
}

/// Account every packet whose payload choice became final since the last wake:
/// I1 against the publication that supplied its frames, and a finality stamp so
/// I2 can close against the controller's transmission timestamp later.
void RecordLedgerFinality(
    ASFWAudioDriver_IVars& ivars,
    ASFW::Audio::Runtime::AudioTransportControlBlock* control,
    const ASFW::Protocols::Audio::AMDTP::AmdtpPacketTimeline& timeline,
    uint64_t timelineEpoch,
    const ASFW::Isoch::IsochTxClockPairSample& pair) noexcept {
    auto* queue = ivars.runtime.txSlotProvider.queueControl;
    if (!queue) return;
    const uint64_t finalizedEnd =
        queue->finalizedEnd.load(std::memory_order_acquire);
    const uint64_t alreadySeen =
        control->ledgerObservedFinalizedEnd.load(std::memory_order_relaxed);
    if (finalizedEnd <= alreadySeen) return;

    // Bound the catch-up: a frontier that jumped further than the stamp ring
    // retains cannot be attributed packet by packet anyway.
    uint64_t first = alreadySeen;
    if (alreadySeen == 0 ||
        finalizedEnd - alreadySeen > ASFW::Audio::Runtime::kLedgerStampSlots) {
        first = finalizedEnd > ASFW::Audio::Runtime::kLedgerStampSlots
                    ? finalizedEnd - ASFW::Audio::Runtime::kLedgerStampSlots
                    : 0;
    }
    for (uint64_t packet = first; packet < finalizedEnd; ++packet) {
        const auto* slot =
            timeline.SlotByIndex(static_cast<uint32_t>(packet));
        if (!slot || !slot->isData || slot->framesInPacket == 0 ||
            slot->epoch != timelineEpoch) {
            continue;
        }
        uint64_t publishedAt = 0;
        const auto lookup = control->ledgerOutputPublication.Lookup(
            slot->firstAudioFrame, publishedAt);
        if (lookup != ASFW::Audio::Runtime::LedgerLookup::Resolved ||
            pair.hostTimeMid < publishedAt) {
            control->ledgerI1WriteToFinality.Count(lookup);
            continue;
        }
        control->ledgerI1WriteToFinality.Record(
            ASFW::Timing::hostTicksToNanos(pair.hostTimeMid - publishedAt) /
            1000U);
    }
    control->ledgerObservedFinalizedEnd.store(finalizedEnd,
                                              std::memory_order_relaxed);
}

void ObserveTxHardware(ASFWAudioDriver_IVars& ivars,
                       uint64_t transportGeneration,
                       bool useMAudio) noexcept {
    auto* queue = ivars.runtime.txSlotProvider.queueControl;
    auto* control = ivars.runtime.directAudioGraph.control;
    if (!queue || !control) return;
    // Whether the sample timeline is driven from TX decides who may publish a
    // boundary. It does not decide whether the TX path's own intervals happen:
    // I1 and I2 are facts about our finality frontier and the controller's
    // transmission, and on an RX-clocked device -- which is the normal case --
    // returning here measured neither of them at all.
    const bool txDrivesTimeline =
        control->hardwareTimeline.Source() ==
        ASFW::Audio::Runtime::HardwareTimelineSource::Transmit;
    const uint64_t stampCount = queue->completionStampCount.load(
        std::memory_order_acquire);
    if (stampCount == 0) {
        // The queue was re-armed; its stamp count restarts from zero and so
        // must our cursor, or the whole next stream reads as already drained.
        ivars.runtime.txCompletionStampCursor = 0;
        return;
    }
    const auto drain = ASFW::Audio::Runtime::PlanTxCompletionStampDrain(
        ivars.runtime.txCompletionStampCursor, stampCount,
        ASFW::Isoch::kIsochTxCompletionStampSlots);
    if (drain.missed != 0) {
        const uint64_t missed =
            control->backendCompletionStampsMissed.fetch_add(
                drain.missed, std::memory_order_relaxed) + drain.missed;
        if (IsPowerOfTwo(missed)) {
            ASFW_LOG_ERROR(
                DirectAudio,
                "[BackendTiming] completionStampsMissed=%llu first=%llu count=%llu",
                missed, drain.first, stampCount);
        }
    }
    if (drain.Empty()) {
        ivars.runtime.txCompletionStampCursor = stampCount;
        return;
    }
    const uint64_t cursor = drain.first;

    // One correlation anchor per wake: the stamps differ in when their packet
    // completed, not in which host/bus pair anchors this pass.
    ASFW::Isoch::IsochTxClockPairSample pair{};
    if (!queue->clockPair.TryRead(pair) || pair.hostTimeMid == 0) return;

    const auto& timeline = ivars.runtime.txStreamEngine.Timeline();
    const uint64_t timelineEpoch = control->hardwareTimeline.Epoch();

    // I1 (E0->E1). Finality is our own frontier, so the moment it passes a
    // packet is only knowable where the frontier is read -- here.
    RecordLedgerFinality(ivars, control, timeline, timelineEpoch, pair);
    // The start endpoint of I2 (E1->E2). It needs this wake's bus time, which
    // only exists once a stamp has been expanded, so the frontier is captured
    // here and stamped on the first expansion below.
    const uint64_t finalizedEndAtWake =
        queue->finalizedEnd.load(std::memory_order_acquire);
    bool finalityStamped = false;

    if (useMAudio && txDrivesTimeline) {
        // The M-Audio warm-up state machine counts transport wakes, not
        // packets: ObserveHardwareWake refuses a second call for the same
        // transport generation, so draining into it would discard every stamp
        // after the first. Hand it the newest DATA packet of this wake instead.
        // That keeps one group per wake while removing the defect that a
        // trailing NO-DATA packet hid the audio which completed beside it.
        uint64_t completionBusTicks = 0;
        uint64_t correlationBusTicks = 0;
        uint64_t sampleFrame = 0;
        uint32_t frameCount = 0;
        bool haveStamp = false;
        for (uint64_t stampIndex = stampCount; stampIndex-- > cursor;) {
            uint64_t packetIndex = 0;
            uint64_t stampCompletion = 0;
            uint64_t stampCorrelation = 0;
            if (!ExpandCompletionStamp(ivars, queue, control, stampIndex,
                                       pair.cycleTimer32, packetIndex,
                                       stampCompletion, stampCorrelation)) {
                continue;
            }
            if (!haveStamp) {
                // Newest readable stamp: the fallback zero-frame warm-up event
                // if this whole wake turns out to carry no audio.
                completionBusTicks = stampCompletion;
                correlationBusTicks = stampCorrelation;
                haveStamp = true;
            }
            const auto* slot = timeline.SlotByIndex(
                static_cast<uint32_t>(packetIndex));
            if (!slot || !slot->isData || slot->framesInPacket == 0 ||
                slot->epoch != timelineEpoch) {
                continue;
            }
            completionBusTicks = stampCompletion;
            correlationBusTicks = stampCorrelation;
            sampleFrame = slot->firstAudioFrame;
            frameCount = slot->framesInPacket;
            break;
        }
        ivars.runtime.txCompletionStampCursor = stampCount;
        if (!haveStamp) return;

        const auto converted =
            ivars.runtime.mAudioPresentationObserver.ObserveHardwareWake(
                transportGeneration, completionBusTicks, correlationBusTicks,
                {.cycleTime = pair.cycleTimer32, .hostTicks = pair.hostTimeMid},
                sampleFrame, frameCount);
        control->mAudioWarmupGroups.store(converted.groupCount,
                                          std::memory_order_relaxed);
        if (converted.captureReferencePlanted) {
            ASFW_LOG(DirectAudio,
                     "[MAudioTiming] capture-reference group=%u host=%llu",
                     converted.groupCount,
                     converted.captureReference.hostTicks);
        }
        if (!converted.observationReady) return;
        control->mAudioTxDerivedObservations.fetch_add(
            1, std::memory_order_relaxed);
        SubmitTxObservation(ivars, control, converted.observation, "maudio-tx");
        return;
    }

    // Generic TX-derived clock: every completed DATA packet is an observation.
    // The timeline publishes a ZTS boundary only when the boundary falls inside
    // the packet it was given, so a packet that is never submitted is a
    // boundary that is never published -- which is why reading the newest stamp
    // alone lost five boundaries in six.
    const uint32_t transfer =
        control->txTransferDelayTicks.load(std::memory_order_relaxed);
    const uint64_t completionCursor =
        queue->completionCursor.load(std::memory_order_relaxed);
    for (uint64_t stampIndex = cursor; stampIndex < stampCount; ++stampIndex) {
        uint64_t packetIndex = 0;
        uint64_t completionBusTicks = 0;
        uint64_t correlationBusTicks = 0;
        if (!ExpandCompletionStamp(ivars, queue, control, stampIndex,
                                   pair.cycleTimer32, packetIndex,
                                   completionBusTicks, correlationBusTicks)) {
            continue;
        }
        if (!finalityStamped) {
            // One stamp per wake, recorded before any lookup consults the ring.
            // Omitting this is what left I2 with every sample unresolved: the
            // ring was read and reset but never written.
            //
            // Take the frontier and the instant from transport's own seal. This
            // wake's correlation time is when we noticed finality, not when it
            // happened, and dating the ring with it understates I2 by however
            // long the notification took. Fall back to the wake only when no
            // seal has been published yet, so the ring is still written.
            uint64_t sealFrontier = 0;
            uint32_t sealCycleTimer = 0;
            uint64_t sealBusTicks = 0;
            uint64_t ignoredCorrelation = 0;
            if (queue->ReadFinalitySeal(sealFrontier, sealCycleTimer) &&
                sealCycleTimer != 0 &&
                ExpandCompletionAndCorrelation(ivars, sealCycleTimer,
                                               pair.cycleTimer32, sealBusTicks,
                                               ignoredCorrelation)) {
                control->ledgerTxFinality.Record(sealFrontier, sealBusTicks);
            } else {
                control->ledgerTxFinality.Record(finalizedEndAtWake,
                                                 correlationBusTicks);
            }
            finalityStamped = true;
        }

        const auto* slot = timeline.SlotByIndex(
            static_cast<uint32_t>(packetIndex));
        if (!slot || !slot->isData || slot->framesInPacket == 0 ||
            slot->epoch != timelineEpoch) {
            continue;
        }

        // I2 (E1->E2). Both endpoints are bus-domain, so this is finality to
        // wire and excludes the delay in noticing the completion.
        uint64_t finalityBusTicks = 0;
        const auto finalityLookup =
            control->ledgerTxFinality.Lookup(packetIndex, finalityBusTicks);
        if (finalityLookup == ASFW::Audio::Runtime::LedgerLookup::Resolved &&
            completionBusTicks >= finalityBusTicks) {
            control->ledgerI2FinalityToTransmit.Record(
                BusTicksToMicros(completionBusTicks - finalityBusTicks));
        } else {
            control->ledgerI2FinalityToTransmit.Count(finalityLookup);
        }

        control->txCycleTrace.Complete(slot->epoch, slot->cycleOrdinal,
                                       completionCursor);
        const uint64_t completionLatency = completionCursor > packetIndex
            ? completionCursor - packetIndex : 0;
        UpdateMaximum(control->txCompletionLatencyMaxCycles, completionLatency);
        control->txCompletionLatencyHistogram[
            HeadroomBucket(completionLatency)].fetch_add(
                1, std::memory_order_relaxed);

        const ASFW::Audio::Runtime::HardwarePresentationObservation observation{
            .epoch = timelineEpoch,
            .source = ASFW::Audio::Runtime::HardwareTimelineSource::Transmit,
            .sampleFrame = slot->firstAudioFrame,
            .frameCount = slot->framesInPacket,
            .presentationBusTicks = completionBusTicks + transfer,
            .correlationBusTicks = correlationBusTicks,
            .correlationHostTicks = pair.hostTimeMid,
        };
        if (txDrivesTimeline) {
            SubmitTxObservation(ivars, control, observation, "tx-fallback");
        }
    }
    ivars.runtime.txCompletionStampCursor = stampCount;
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
        // Arming cannot fail for content reasons any more: the packet is
        // encoded from silence and content, if it arrives in time, replaces the
        // sample words later. The deferral / convert-to-NO-DATA branch that
        // used to live here is gone with the contract that needed it -- a
        // planned DATA packet now always transmits as DATA, carrying either
        // real content or the silence it was armed with, and its absolute frame
        // range is consumed either way.
        const PrepareResult result =
            ivars.runtime.txStreamEngine.PrepareTransmitSlot(
                static_cast<uint32_t>(packetIndex), plan, wireBlocks, syt);
        const PrepareResult initialResult = result;
        if (result != PrepareResult::Prepared) {
            break;
        } else if (ivars.runtime.txSecondaryActive &&
                   ivars.runtime.txStreamEngineSecondary.PrepareTransmitSlot(
                       static_cast<uint32_t>(packetIndex), plan,
                       wireBlocks, syt) != PrepareResult::Prepared) {
            break;
        }

        if (plan.frameCount != 0) {
            // The presentation lead this packet carries: how far after its own
            // transmit time the device is told to present it. This is the only
            // part of the reported output latency the driver can measure -- the
            // remainder is the device's analogue delay -- so it is recorded
            // rather than assumed by the profile that reports it.
            if (haveCycle && plan.presentationBusTicks >= transmitBusTicks) {
                const int64_t leadTicks =
                    static_cast<int64_t>(plan.presentationBusTicks) -
                    static_cast<int64_t>(transmitBusTicks);
                control->txLastLeadTicks.store(leadTicks,
                                               std::memory_order_relaxed);
                int64_t seen = control->txMinimumLeadTicks.load(
                    std::memory_order_relaxed);
                while (leadTicks < seen &&
                       !control->txMinimumLeadTicks.compare_exchange_weak(
                           seen, leadTicks, std::memory_order_relaxed,
                           std::memory_order_relaxed)) {
                }
                seen = control->txMaximumLeadTicks.load(
                    std::memory_order_relaxed);
                while (leadTicks > seen &&
                       !control->txMaximumLeadTicks.compare_exchange_weak(
                           seen, leadTicks, std::memory_order_relaxed,
                           std::memory_order_relaxed)) {
                }
            }
            range = {
                .epoch = plan.epoch,
                .firstAudioFrame = plan.firstAudioFrame,
                .frameCount = plan.frameCount,
                .presentationBusTicks = plan.presentationBusTicks,
            };
            // --- TX/RX alignment probes -------------------------------------
            //
            // The 288-frame RTL displacement has three candidate causes that
            // no startup-only trace can separate: a wrong receive-derived seed,
            // execution time lost mid-epoch, and a loopback correlator locking
            // onto a repeat of the 288-frame ring content. These two lines
            // decide between them. Both are receive-sourced only: a Transmit
            // timeline initialises its cursor at BeginEpoch and never seeds.
            //
            // Read the answer as: constant nonzero delta => the seed;
            // delta growing in 288-frame steps => mid-epoch loss; delta
            // always zero => the displacement is not here, and aliasing
            // returns to the front.
            const bool seedingNow =
                !control->hardwareTimeline.TxCursorInitialized();
            if (!control->hardwareTimeline.CommitTxRange(range)) break;
            if (control->hardwareTimeline.Source() ==
                ASFW::Audio::Runtime::HardwareTimelineSource::Receive) {
                if (seedingNow) {
                    // Once per epoch, at the flip. Everything the seed
                    // expression consumed, plus the completion coordinates the
                    // presentation time was extrapolated from.
                    ASFW_LOG(
                        DirectAudio,
                        "[TxSeed] epoch=%llu first=%llu presentBus=%llu obsFrame=%llu obsBus=%llu nominal=%u frames=%u packet=%llu completionCursor=%llu",
                        range.epoch, range.firstAudioFrame,
                        range.presentationBusTicks,
                        control->hardwareTimeline.LastObservationFrame(),
                        control->hardwareTimeline.LastObservationBusTicks(),
                        control->hardwareTimeline.NominalBusTicksPerFrame(),
                        range.frameCount, packetIndex, completionCursor);
                    ivars.runtime.txAlignmentDeltaFrames = 0;
                    ivars.runtime.txAlignmentValid = false;
                } else {
                    // Re-evaluate the seed expression against the *current*
                    // observation for this packet's own presentation time. At
                    // the seed the two agree by construction; afterwards
                    // PreviewTxRange stops consulting the observation at all,
                    // so any divergence is the cursor drifting away from the
                    // hardware it was placed against.
                    uint64_t projected = 0;
                    if (control->hardwareTimeline
                            .ProjectFirstFrameFromObservation(
                                range.presentationBusTicks, projected)) {
                        const int64_t delta =
                            static_cast<int64_t>(range.firstAudioFrame) -
                            static_cast<int64_t>(projected);
                        // Anomaly-only: one line when the divergence moves, not
                        // one per packet. A clean run prints nothing here.
                        if (!ivars.runtime.txAlignmentValid ||
                            delta != ivars.runtime.txAlignmentDeltaFrames) {
                            const int64_t previous =
                                ivars.runtime.txAlignmentValid
                                    ? ivars.runtime.txAlignmentDeltaFrames
                                    : 0;
                            const int64_t step = delta - previous;
                            const uint32_t nominal =
                                control->hardwareTimeline
                                    .NominalBusTicksPerFrame();
                            ASFW_LOG(
                                DirectAudio,
                                "[TxAlign] epoch=%llu delta=%lld step=%lld ringFrames=%u stepLaps=%lld cursorFirst=%llu projected=%llu presentBus=%llu obsFrame=%llu obsBus=%llu nominal=%u packet=%llu completionCursor=%llu",
                                range.epoch, delta, step, kTxRingFrames,
                                step / static_cast<int64_t>(kTxRingFrames),
                                range.firstAudioFrame, projected,
                                range.presentationBusTicks,
                                control->hardwareTimeline.LastObservationFrame(),
                                control->hardwareTimeline
                                    .LastObservationBusTicks(),
                                nominal, packetIndex, completionCursor);
                            ivars.runtime.txAlignmentDeltaFrames = delta;
                            ivars.runtime.txAlignmentValid = true;
                        }
                    }
                }
            }
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
    ivars.runtime.txFillCursor = 0;
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

    // Content pass. Arming above fixed the wire geometry for a deep horizon;
    // this fills in samples until transport makes their payload choice final.
    // Bound descriptors remain fillable: transport can repoint their mutable
    // payload tail without changing the invariant packet prefix.
    {
        const uint64_t frozen =
            queue->finalizedEnd.load(std::memory_order_acquire);
        // Everything the frontier passed that this loop never filled now
        // transmits the silence it was armed with. Attribute it before the
        // cursor skips over it: a stream that plays but is quietly half silence
        // must not look identical to one that is not.
        while (ivars->runtime.txFillCursor < frozen) {
            const auto packet =
                static_cast<uint32_t>(ivars->runtime.txFillCursor);
            ivars->runtime.txStreamEngine.NoteFrozenWithoutContent(packet);
            if (ivars->runtime.txSecondaryActive) {
                ivars->runtime.txStreamEngineSecondary
                    .NoteFrozenWithoutContent(packet);
            }
            ++ivars->runtime.txFillCursor;
        }
        while (ivars->runtime.txFillCursor < committedAfter) {
            const auto packet =
                static_cast<uint32_t>(ivars->runtime.txFillCursor);
            const ASFW::Audio::DriverKit::FillResult primary =
                ivars->runtime.txStreamEngine.FillTransmitSlot(packet);
            if (primary == ASFW::Audio::DriverKit::FillResult::ContentUnavailable) break;
            if (primary == ASFW::Audio::DriverKit::FillResult::Filled) {
                // Streams sharing one presentation plan commit together or not
                // at all: real content on one and silence on its sibling for
                // the same frame range is worse than silence on both.
                const bool secondaryReady =
                    !ivars->runtime.txSecondaryActive ||
                    ivars->runtime.txStreamEngineSecondary.FillTransmitSlot(
                        packet) ==
                        ASFW::Audio::DriverKit::FillResult::Filled;
                if (secondaryReady) {
                    if (ivars->runtime.txStreamEngine.CommitFill(packet) &&
                        ivars->runtime.txSecondaryActive) {
                        (void)ivars->runtime.txStreamEngineSecondary.CommitFill(
                            packet);
                    }
                }
            }
            ++ivars->runtime.txFillCursor;
        }
    }
    const uint64_t margin = committedAfter > completion
        ? committedAfter - completion : 0;
    ASFW::Audio::DriverKit::UpdateMaximum(
        control->txPacketStoreHighWaterPackets, margin);
    control->txTransportCompletionCursor.store(completion,
                                                std::memory_order_relaxed);
    control->txTransportCommittedEnd.store(committedAfter,
                                            std::memory_order_relaxed);
    // The status belongs to this mirror and was the one field never refreshed,
    // so diagnostics read its initialisation value and reported a live stream
    // as stopped while the cursors beside it advanced. IsochTxQueueStatus and
    // the diagnostic encoding share their numbering by construction.
    control->txTransportStatus.store(
        static_cast<uint32_t>(queue->statusWord.load(std::memory_order_acquire)),
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

        {
            const int64_t leadTicks =
                control->txLastLeadTicks.load(std::memory_order_relaxed);
            const int64_t minLead =
                control->txMinimumLeadTicks.load(std::memory_order_relaxed);
            const int64_t maxLead =
                control->txMaximumLeadTicks.load(std::memory_order_relaxed);
            const uint32_t rate = control->hardwareTimeline.SampleRateHz();
            const int64_t ticksPerFrame = rate != 0
                ? static_cast<int64_t>(24'576'000U / rate) : 512;
            ASFW_LOG(DirectAudio,
                     "[TxLead] ticks=%lld min=%lld max=%lld frames=%lld rate=%u",
                     leadTicks, minLead == INT64_MAX ? 0 : minLead,
                     maxLead == INT64_MIN ? 0 : maxLead,
                     ticksPerFrame != 0 ? leadTicks / ticksPerFrame : 0,
                     rate);
        }
        {
            const auto& fill = ivars->runtime.txStreamEngine.Counters();
            const uint32_t minRebindDistance =
                queue->minimumLatePayloadRebindDistance.load(
                    std::memory_order_relaxed);
            ASFW_LOG(DirectAudio,
                     // `filled` is the producer's optimistic count: it rises
                     // when a publication is accepted, which is not the same as
                     // the wire carrying it. `lost` is transport's count of
                     // accepted publications it then sealed on the armed image,
                     // so filled-minus-lost is the truthful content figure and
                     // the one the latency ledger should read.
                     "[TxFill] filled=%llu lost=%llu tooLate=%llu unavailable=%llu silentData=%llu cursor=%llu finalized=%llu mapped=%llu committed=%llu rebound=%llu rejected=%llu missedDeadline=%llu stampsMissed=%llu minRebindDistance=%u",
                     fill.lateFillsPublished.load(std::memory_order_relaxed),
                     queue->latePayloadLostPublicationCount.load(
                         std::memory_order_relaxed),
                     fill.lateFillsTooLate.load(std::memory_order_relaxed),
                     fill.lateFillsUnavailable.load(std::memory_order_relaxed),
                     fill.pcmSilenceSubstitutions.load(std::memory_order_relaxed),
                     ivars->runtime.txFillCursor,
                     queue->finalizedEnd.load(std::memory_order_relaxed),
                     queue->mappedEnd.load(std::memory_order_relaxed),
                     committedAfter,
                     queue->latePayloadRebindCount.load(
                         std::memory_order_relaxed),
                     queue->latePayloadRebindRejectedCount.load(
                         std::memory_order_relaxed),
                     // Rebinds abandoned because the controller had reached the
                     // packet by the time the store was authorised, and
                     // completion stamps that aged out before the observer
                     // drained them. Both are written on the fix paths added
                     // with them and were readable nowhere.
                     queue->latePayloadRebindMissedDeadlineCount.load(
                         std::memory_order_relaxed),
                     control->backendCompletionStampsMissed.load(
                         std::memory_order_relaxed),
                     minRebindDistance == UINT32_MAX ? 0 :
                         minRebindDistance);
        }

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

        // The four ledger intervals ride the same coarse heartbeat rather than
        // being anomaly-gated: a distribution that only appears when it is
        // already bad cannot establish what normal looks like, which is the
        // one thing these exist to do. The buckets are cumulative for the run,
        // so successive lines are a running shape, not a per-interval sample.
        using ASFW::Audio::DriverKit::LogLedgerInterval;
        LogLedgerInterval("I1 write->final ", control->ledgerI1WriteToFinality);
        LogLedgerInterval("I2 final->wire  ", control->ledgerI2FinalityToTransmit);
        LogLedgerInterval("J3 recv->decode ", control->ledgerJ3ReceiveToDecode);
        LogLedgerInterval("J4 decode->read ", control->ledgerJ4DecodeToRead);
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
