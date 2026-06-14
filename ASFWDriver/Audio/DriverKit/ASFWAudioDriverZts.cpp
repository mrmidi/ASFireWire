//
// ASFWAudioDriverZts.cpp
// ASFWDriver
//
// Ordered zero-timestamp queue drain for ASFWAudioDriver.
//

#include <new>

#include "ASFWAudioDevice.h"
#include "ASFWAudioDriverPrivate.hpp"
#include "../../Common/TimingUtils.hpp"
#include "../../Logging/Logging.hpp"

#include "../Config/TimingCursorPolicy.hpp"

#include <DriverKit/DriverKit.h>

namespace ASFW::Audio::DriverKit {

ASFW::Audio::Runtime::ZtsMirrorPublishResult PublishSharedZeroTimestampToHAL(ASFWAudioDriver_IVars& ivars,
                                                                             uint64_t throughGeneration,
                                                                             const char* reason,
                                                                             bool logSuccess) noexcept {
    auto* control = ivars.runtime.directAudioGraph.control;
    auto* audioDevice = ivars.audioDevice.get();
    if (!control || !audioDevice) {
        return ASFW::Audio::Runtime::ZtsMirrorPublishResult::NotReady;
    }

    const auto policy = ASFW::Audio::TimingCursorPolicy::MakeDice48kBlocking();
    const uint32_t P = policy.HalZeroTimestampPeriodFrames();
    if (P == 0) {
        return ASFW::Audio::Runtime::ZtsMirrorPublishResult::InvalidTimeline;
    }

    // The declared ZTS period is a host contract: each successive sample time
    // must advance by exactly P. Drain and publish every queued hardware anchor
    // in order; collapsing to the newest anchor creates multi-period jumps that
    // can prevent the HAL IO engine from establishing its cycle.
    (void)throughGeneration;
    bool published = false;
    bool firstPublication = false;
    uint64_t drained = 0;
    uint64_t publishedCount = 0;
    ASFW::Audio::Runtime::HostClockAnchorSample anchor{};
    ASFW::Audio::Runtime::HostClockAnchorSample newestPublished{};
    uint64_t lastSampleFrame =
        ivars.runtime.lastHalZeroTimestampSampleFrame.load(
            std::memory_order_relaxed);
    uint64_t lastHostTicks =
        ivars.runtime.lastHalZeroTimestampHostTicks.load(
            std::memory_order_relaxed);

    while (control->hostClockAnchor.TryPop(anchor)) {
        ++drained;
        if (anchor.hostTicks == 0 || anchor.hostNanosPerSampleQ8 == 0 ||
            (anchor.sampleFrame % P) != 0) {
            continue;
        }

        const bool firstAnchor = lastHostTicks == 0;
        const bool advancesExactly =
            firstAnchor ||
            (anchor.sampleFrame == lastSampleFrame + P &&
             anchor.hostTicks > lastHostTicks);
        if (!advancesExactly) {
            if (logSuccess) {
                ASFW_LOG(
                    DirectAudio,
                    "ADK ZTS skip discontinuity sample=%llu host=%llu expectedSample=%llu lastHost=%llu",
                    anchor.sampleFrame,
                    anchor.hostTicks,
                    lastSampleFrame + P,
                    lastHostTicks);
            }
            continue;
        }

        if (firstAnchor) {
            firstPublication = true;
        }
        audioDevice->UpdateCurrentZeroTimestamp(
            anchor.sampleFrame, anchor.hostTicks);
        lastSampleFrame = anchor.sampleFrame;
        lastHostTicks = anchor.hostTicks;
        newestPublished = anchor;
        ++publishedCount;
        published = true;
        control->hostClockAnchor.mirrorPublications.fetch_add(
            1, std::memory_order_relaxed);
        control->counters.CountRxAdkZtsPublished();
    }

    if (published) {
        ivars.runtime.lastHalZeroTimestampSampleFrame.store(
            lastSampleFrame, std::memory_order_relaxed);
        ivars.runtime.lastHalZeroTimestampHostTicks.store(
            lastHostTicks, std::memory_order_relaxed);
        ivars.runtime.lastHalZeroTimestampGeneration.store(
            control->hostClockAnchor.consumerCursor.load(
                std::memory_order_acquire),
            std::memory_order_release);
        if (logSuccess) {
            ASFW_LOG(
                DirectAudio,
                "ADK ZTS publish reason=%{public}s sample=%llu host=%llu period=%u drained=%llu published=%llu adkPeriod=%u",
                reason ? reason : "unknown",
                newestPublished.sampleFrame,
                newestPublished.hostTicks,
                P,
                drained,
                publishedCount,
                audioDevice->GetZeroTimestampPeriod());
        }

        if (firstPublication) {
            ASFW_LOG(DirectAudio,
                     "Core audio hardware ZTS ready guid=0x%016llx sampleFrame=%llu hostTicks=%llu",
                     ivars.device.guid,
                     newestPublished.sampleFrame,
                     newestPublished.hostTicks);
        }
        return ASFW::Audio::Runtime::ZtsMirrorPublishResult::Published;
    }
    return ASFW::Audio::Runtime::ZtsMirrorPublishResult::AlreadyPublished;
}

uint32_t PrepareTransmitSlots(ASFWAudioDriver_IVars& ivars,
                             uint64_t startPacketIndex,
                             uint64_t targetPacketIndex,
                             uint32_t maxToPrepare,
                             bool allowRecoveredClock) noexcept {
    const uint32_t numSlots = ivars.runtime.txSlotProvider.numSlots;
    auto* metadataRing = ivars.runtime.txSlotProvider.metadataRing;
    auto* directControl = ivars.runtime.directAudioGraph.control;
    if (numSlots == 0 || metadataRing == nullptr || directControl == nullptr) {
        return 0;
    }

    uint64_t nextPacketToPrepare = startPacketIndex;
    uint32_t preparedCount = 0;

    const auto failReplay =
        [&](ASFW::Audio::Runtime::FatalStreamReason reason) noexcept {
            directControl->fatalReason.store(
                reason, std::memory_order_release);
            directControl->fatalGeneration.fetch_add(
                1, std::memory_order_release);
            directControl->counters.txImmediateStops.fetch_add(
                1, std::memory_order_relaxed);
            if (auto* txControl =
                    ivars.runtime.txSlotProvider.controlBlock) {
                txControl->statusWord.store(
                    ASFW::IsochTransport::TxStreamStatus::
                        kUnderrunFatal,
                    std::memory_order_release);
            }
            ivars.runtime.txActive.store(
                false, std::memory_order_release);
        };

    if (allowRecoveredClock) {
        uint64_t completedPacketIndex = 0;
        int64_t rawCallbackPhase = 0;
        if (ivars.runtime.txExecutionTimeline.RawCallbackPhase(
                completedPacketIndex, rawCallbackPhase)) {
            const auto anchorResult =
                ivars.runtime.txAnchorTracker.ObserveCallbackPhase(
                    completedPacketIndex, rawCallbackPhase);
            if (anchorResult.resetRequired) {
                failReplay(
                    ASFW::Audio::Runtime::FatalStreamReason::
                        TxReplayUnavailable);
                return 0;
            }
        }
    }

    while (nextPacketToPrepare < targetPacketIndex &&
           preparedCount < maxToPrepare) {
        ASFW::Protocols::Audio::AMDTP::AmdtpTimingState timing{};
        timing.replayValid = true;
        timing.disposition =
            ASFW::Protocols::Audio::AMDTP::
                AmdtpPacketDisposition::NoData;

        if (allowRecoveredClock) {
            int64_t packetAnchorTicks = 0;
            if (!ivars.runtime.txExecutionTimeline.AnchorForPacket(
                    nextPacketToPrepare, packetAnchorTicks)) {
                directControl->txReplayUnderflows.fetch_add(
                    1, std::memory_order_relaxed);
                failReplay(
                    ASFW::Audio::Runtime::FatalStreamReason::
                        TxReplayUnavailable);
                break;
            }

            if (!ivars.runtime.txReplayReader.IsActive() &&
                !ivars.runtime.txReplayReader.Begin(
                    directControl->rxSequenceReplay)) {
                directControl->txReplayUnderflows.fetch_add(
                    1, std::memory_order_relaxed);
                failReplay(
                    ASFW::Audio::Runtime::FatalStreamReason::
                        TxReplayUnavailable);
                break;
            }

            ASFW::Audio::Runtime::RxSequenceEntry replay{};
            if (!ivars.runtime.txReplayReader.TryRead(
                    directControl->rxSequenceReplay, replay)) {
                directControl->txReplayUnderflows.fetch_add(
                    1, std::memory_order_relaxed);
                failReplay(
                    ASFW::Audio::Runtime::FatalStreamReason::
                        TxReplayUnavailable);
                break;
            }
            directControl->txReplayEntries.fetch_add(
                1, std::memory_order_relaxed);
            timing.replayDataBlocks = replay.dataBlocks;

            if (replay.dataBlocks != 0) {
                if (replay.sytOffset ==
                        ASFW::Audio::Runtime::
                            RxSequenceReplayState::kNoInfo ||
                    (replay.flags &
                     ASFW::Audio::Runtime::RxSequenceFlags::
                         kValidSyt) == 0) {
                    directControl->txReplayInvalidSyt.fetch_add(
                        1, std::memory_order_relaxed);
                    failReplay(
                        ASFW::Audio::Runtime::FatalStreamReason::
                            TxReplayInvalidSyt);
                    break;
                }

                timing.txClockValid = true;
                timing.disposition =
                    ASFW::Protocols::Audio::AMDTP::
                        AmdtpPacketDisposition::Data;
                timing.nextDataSyt =
                    ASFW::Audio::Runtime::
                        ComputeReplaySytFromTicks(
                            replay.sytOffset,
                            packetAnchorTicks,
                            directControl
                                ->txTransferDelayTicks.load(
                                    std::memory_order_relaxed));

                const int64_t sourcePresentationTicks =
                    ASFW::Timing::normalizeOffsetDomain(
                        ASFW::Timing::encodedTstampToOffsets(
                            replay.sourceCycleTimer) +
                        replay.sytOffset +
                        directControl
                            ->rxTransferDelayTicks.load(
                                std::memory_order_relaxed));
                const int64_t outputPresentationTicks =
                    ASFW::Timing::normalizeOffsetDomain(
                        packetAnchorTicks +
                        replay.sytOffset +
                        directControl
                            ->txTransferDelayTicks.load(
                                std::memory_order_relaxed));
                const int64_t presentationDeltaTicks =
                    ASFW::Timing::extOffsetDiff(
                        outputPresentationTicks,
                        sourcePresentationTicks);
                if (presentationDeltaTicks >= 0) {
                    constexpr uint32_t kFramesPerPacket =
                        ASFW::IsochTransport::
                            AudioTimingGeometry::
                                kFramesPerDataPacket;
                    const uint64_t projectedFrame =
                        replay.firstAudioFrame +
                        static_cast<uint64_t>(
                            presentationDeltaTicks /
                            ASFW::Timing::
                                kTicksPerSample48k);
                    const uint64_t alignedFrame =
                        (projectedFrame / kFramesPerPacket) *
                        kFramesPerPacket;
                    (void)ivars.runtime.txStreamEngine
                        .AlignFrameCursorOnce(alignedFrame);
                }
            }
        }

        if (!ivars.runtime.txStreamEngine.PrepareNextTransmitSlot(
                static_cast<uint32_t>(nextPacketToPrepare),
                timing)) {
            failReplay(
                ASFW::Audio::Runtime::FatalStreamReason::
                    TxReplayUnavailable);
            break;
        }

        const uint32_t slotIdx =
            static_cast<uint32_t>(
                nextPacketToPrepare % numSlots);
        const auto& meta = metadataRing[slotIdx];
        if (meta.payloadLength > 8) {
            directControl->counters.txDataPackets.fetch_add(
                1, std::memory_order_relaxed);
            directControl->counters.txValidSytPackets.fetch_add(
                1, std::memory_order_relaxed);
        } else {
            directControl->counters.txNoDataPackets.fetch_add(
                1, std::memory_order_relaxed);
            directControl->counters.txSytFfffPackets.fetch_add(
                1, std::memory_order_relaxed);
        }
        directControl->counters.txPackets.fetch_add(
            1, std::memory_order_relaxed);

        ++nextPacketToPrepare;
        ++preparedCount;
    }

    return preparedCount;
}

void PrefillTxRingBeforeStart(ASFWAudioDriver_IVars& ivars) noexcept {
    const uint32_t numSlots = ivars.runtime.txSlotProvider.numSlots;
    auto* metadataRing = ivars.runtime.txSlotProvider.metadataRing;
    if (numSlots == 0 || metadataRing == nullptr) {
        return;
    }

    // Commit one complete shared-ring lap before IT RUN. TxPreparationReady
    // has a dedicated queue, but action delivery and the startup handoff can
    // still be delayed. A lead-only prefill can therefore be consumed before
    // the producer gets its first turn. Steady state still targets completion
    // + kTxPreparationLeadPackets.
    ASFW::Protocols::Audio::AMDTP::AmdtpTimingState timing{};
    timing.replayValid = true;
    timing.txClockValid = false;
    timing.disposition =
        ASFW::Protocols::Audio::AMDTP::
            AmdtpPacketDisposition::NoData;

    uint32_t prepared = 0;
    for (uint64_t packetIndex = 0;
         packetIndex < numSlots;
         ++packetIndex) {
        if (!ivars.runtime.txStreamEngine.PrepareNextTransmitSlot(
                static_cast<uint32_t>(packetIndex), timing)) {
            break;
        }
        ++prepared;
    }

    ASFW_LOG(DirectAudio,
             "ADK DBG TX prefill seeded %u/%u committed NO-DATA packets before isoch start (steadyLead=%u)",
             prepared,
             numSlots,
             ASFW::IsochTransport::AudioTimingGeometry::
                 kTxPreparationLeadPackets);
}

} // namespace ASFW::Audio::DriverKit

void IMPL(ASFWAudioDriver, ZtsAnchorReady)
{
    (void)action;
    if (!ivars || !ivars->audioDevice) {
        return;
    }

    (void)ASFW::Audio::DriverKit::PublishSharedZeroTimestampToHAL(
        *ivars, generation, "rx-action", false);
}

void IMPL(ASFWAudioDriver, TxPreparationReady)
{
    (void)action;
    if (!ivars ||
        !ivars->runtime.txActive.load(
            std::memory_order_acquire)) {
        return;
    }

    auto* txControl = ivars->runtime.txSlotProvider.controlBlock;
    const uint32_t numSlots = ivars->runtime.txSlotProvider.numSlots;
    if (!txControl || numSlots == 0) {
        return;
    }

    const uint64_t requested =
        txControl->preparationRequestGeneration.load(
            std::memory_order_acquire);
    if (generation > requested) {
        return;
    }

    const uint64_t completionCursor =
        txControl->completionCursor.load(std::memory_order_acquire);
    const uint64_t exposeCursor =
        txControl->exposeCursor.load(std::memory_order_acquire);
    const uint64_t targetPacketIndex =
        completionCursor +
        ASFW::IsochTransport::AudioTimingGeometry::
            kTxPreparationLeadPackets;

    const bool replayEstablished =
        ivars->runtime.directAudioGraph.control &&
        ivars->runtime.directAudioGraph.control
            ->rxSequenceReplay.IsEstablished();
    const uint32_t slotsPrepared =
        ASFW::Audio::DriverKit::PrepareTransmitSlots(
            *ivars,
            exposeCursor,
            targetPacketIndex,
            ASFW::IsochTransport::AudioTimingGeometry::
                kTxPreparationLeadPackets,
            replayEstablished);

    // [TxPrepRange] Refill-coverage instrumentation. Answers the decisive
    // question: did the producer's range reach `target` this wake, or stop
    // short and leave a hole the IT refill ISR will later trip on? The producer
    // loop is linear in absolute packet index, so `prepareUntil` is exactly
    // `base + slotsPrepared`; `firstMissingAbs` is the first slot NOT covered
    // when it stopped short of target (UINT64_MAX = fully covered).
    {
        const uint64_t prepareBaseAbs = exposeCursor;
        const uint64_t prepareUntilAbs = exposeCursor + slotsPrepared;
        const uint64_t requestedSpan =
            targetPacketIndex > prepareBaseAbs
                ? targetPacketIndex - prepareBaseAbs
                : 0;
        const bool stoppedShort = prepareUntilAbs < targetPacketIndex;
        const uint64_t firstMissingAbs =
            stoppedShort ? prepareUntilAbs : UINT64_MAX;
        const uint64_t committedMargin =
            prepareUntilAbs > completionCursor
                ? prepareUntilAbs - completionCursor
                : 0;
        // Log every wake that stopped short of target (the hole-producing
        // case), plus a coarse heartbeat. A clean run prints only heartbeats;
        // any [TxPrepRange] line with short=1 / firstMissing!=-1 before an IT
        // FATAL proves the underrun is refill-coverage, not scheduling margin.
        if (stoppedShort || (slotsPrepared == 0) ||
            (txControl->preparationRequestCount.load(
                 std::memory_order_relaxed) %
             1024) == 0) {
            ASFW_LOG(
                DirectAudio,
                "[TxPrepRange] retiredAbs=%llu prepareBaseAbs=%llu "
                "prepareUntilAbs=%llu leadTargetAbs=%llu reqSpan=%llu "
                "prepared=%u short=%d firstMissingAbs=%lld marginAfter=%llu "
                "replay=%d",
                completionCursor,
                prepareBaseAbs,
                prepareUntilAbs,
                targetPacketIndex,
                requestedSpan,
                slotsPrepared,
                stoppedShort ? 1 : 0,
                stoppedShort ? static_cast<int64_t>(firstMissingAbs)
                             : static_cast<int64_t>(-1),
                committedMargin,
                replayEstablished ? 1 : 0);
        }
    }

    if (auto* directControl =
            ivars->runtime.directAudioGraph.control) {
        const uint64_t now = mach_absolute_time();
        const uint64_t requestedAt =
            txControl->preparationRequestHostTicks.load(
                std::memory_order_relaxed);
        const uint64_t latency =
            now >= requestedAt ? now - requestedAt : 0;
        const uint64_t latencyNanos =
            ASFW::Timing::hostTicksToNanos(latency);
        directControl->txLastPreparationLatencyTicks.store(
            latency, std::memory_order_relaxed);
        directControl->txPreparationLatencySamples.fetch_add(
            1, std::memory_order_relaxed);
        if (latencyNanos <= 750000) {
            directControl->txPreparationAtMost750Us.fetch_add(
                1, std::memory_order_relaxed);
        }
        if (latencyNanos >= 1500000) {
            directControl->txPreparationAtLeast1500Us.fetch_add(
                1, std::memory_order_relaxed);
        }
        uint64_t previousMax =
            directControl->txMaxPreparationLatencyTicks.load(
                std::memory_order_relaxed);
        while (latency > previousMax &&
               !directControl->txMaxPreparationLatencyTicks
                    .compare_exchange_weak(
                        previousMax,
                        latency,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
        }
        const uint64_t distance =
            targetPacketIndex > exposeCursor
                ? targetPacketIndex - exposeCursor
                : 0;
        const uint32_t boundedDistance =
            distance > UINT32_MAX
                ? UINT32_MAX
                : static_cast<uint32_t>(distance);
        uint32_t previousMin =
            directControl->txMinimumPreparationDistance.load(
                std::memory_order_relaxed);
        while (boundedDistance < previousMin &&
               !directControl->txMinimumPreparationDistance
                    .compare_exchange_weak(
                        previousMin,
                        boundedDistance,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
        }
        const uint64_t committedMargin =
            exposeCursor > completionCursor
                ? exposeCursor - completionCursor
                : 0;
        const uint32_t boundedMargin =
            committedMargin > UINT32_MAX
                ? UINT32_MAX
                : static_cast<uint32_t>(committedMargin);
        const uint32_t committedMarginFloorBefore =
            directControl->txMinimumCommittedMarginPackets.load(
                std::memory_order_relaxed);
        uint32_t previousMargin = committedMarginFloorBefore;
        while (boundedMargin < previousMargin &&
               !directControl->txMinimumCommittedMarginPackets
                    .compare_exchange_weak(
                        previousMargin,
                        boundedMargin,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
        }

        // [TxPrep] Surface the cross-queue preparation health to the log. The
        // refill ISR trips kUnderrunFatal once committedMargin falls to the
        // hardware-owned ring depth, so emit on every new committed-margin low
        // (captures the descent to 0 right before a FATAL), on every wake that
        // exceeds the 1.5 ms slack budget (the actual stall that causes it),
        // and on a coarse heartbeat. See documentation/ZTS_AND_SYT.md §13.
        const uint32_t minCommittedMargin =
            directControl->txMinimumCommittedMarginPackets.load(
                std::memory_order_relaxed);
        const uint64_t maxLatencyNanos = ASFW::Timing::hostTicksToNanos(
            directControl->txMaxPreparationLatencyTicks.load(
                std::memory_order_relaxed));
        const uint64_t wakeSamples =
            directControl->txPreparationLatencySamples.load(
                std::memory_order_relaxed);
        constexpr uint32_t kCommittedMarginDangerPackets =
            ASFW::IsochTransport::AudioTimingGeometry::kTxHardwareRingPackets;
        const bool newCommittedMarginLow =
            boundedMargin < committedMarginFloorBefore;
        const bool slackBudgetExceeded = latencyNanos >= 1500000;
        if (newCommittedMarginLow || slackBudgetExceeded ||
            (wakeSamples % 1024) == 0) {
            ASFW_LOG(
                DirectAudio,
                "[TxPrep] margin=%u min=%u lead=%u distMin=%u lastLatUs=%llu "
                "maxLatUs=%llu late1500=%llu wakes=%llu%s",
                boundedMargin,
                minCommittedMargin,
                ASFW::IsochTransport::AudioTimingGeometry::
                    kTxPreparationLeadPackets,
                directControl->txMinimumPreparationDistance.load(
                    std::memory_order_relaxed),
                latencyNanos / 1000,
                maxLatencyNanos / 1000,
                directControl->txPreparationAtLeast1500Us.load(
                    std::memory_order_relaxed),
                wakeSamples,
                boundedMargin <= kCommittedMarginDangerPackets ? " DANGER"
                                                               : "");
        }

        directControl->txPreparationRequests.requestedGeneration.store(
            requested, std::memory_order_relaxed);
        directControl->txPreparationRequests.requestHostTicks.store(
            requestedAt, std::memory_order_relaxed);
        directControl->counters.txPreparationWakeRequests.store(
            txControl->preparationRequestCount.load(
                std::memory_order_relaxed),
            std::memory_order_relaxed);
        directControl->counters.txPreparationWakeDispatches.fetch_add(
            1, std::memory_order_relaxed);
        directControl->counters.txPreparationWakeCoalesced.store(
            txControl->preparationCoalescedCount.load(
                std::memory_order_relaxed),
            std::memory_order_relaxed);
        directControl->counters.txPreparationDrainPasses.fetch_add(
            1, std::memory_order_relaxed);
        directControl->txPreparationRequests.MarkHandled(
            requested, now);
    }

    txControl->MarkPreparationHandled(requested);
}
