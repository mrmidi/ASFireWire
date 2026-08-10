//
// ASFWAudioDriverZts.cpp
// ASFWDriver
//
// Latest observed zero-timestamp publication for ASFWAudioDriver.
//

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <new>

#include "ASFWAudioDevice.h"
#include "ASFWAudioDriverPrivate.hpp"
#include "../Runtime/TxContentRecoveryPolicy.hpp"
#include "../../Common/TimingUtils.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/DriverKit.h>

namespace ASFW::Audio::DriverKit {
namespace {

} // namespace

ASFW::Audio::Runtime::ZtsMirrorPublishResult PublishSharedZeroTimestampToHAL(
    ASFWAudioDriver_IVars& ivars,
    const char* reason,
    bool logSuccess) noexcept {
    auto* control = ivars.runtime.directAudioGraph.control;
    auto* audioDevice = ivars.audioDevice.get();
    if (!control || !audioDevice) {
        return ASFW::Audio::Runtime::ZtsMirrorPublishResult::NotReady;
    }

    const uint64_t lastGeneration =
        ivars.runtime.lastHalZeroTimestampGeneration.load(
            std::memory_order_acquire);
    ASFW::Audio::Runtime::HostClockAnchorSample anchor{};
    if (!control->hostClockAnchor.TryReadLatest(
            lastGeneration, anchor)) {
        return ASFW::Audio::Runtime::ZtsMirrorPublishResult::
            NoNewGeneration;
    }

    const bool firstPublication =
        ivars.runtime.lastHalZeroTimestampHostTicks.load(
            std::memory_order_relaxed) == 0;
    audioDevice->UpdateCurrentZeroTimestamp(
        anchor.sampleFrame, anchor.hostTicks);
    ivars.runtime.lastHalZeroTimestampSampleFrame.store(
        anchor.sampleFrame, std::memory_order_relaxed);
    ivars.runtime.lastHalZeroTimestampHostTicks.store(
        anchor.hostTicks, std::memory_order_relaxed);
    ivars.runtime.lastHalZeroTimestampGeneration.store(
        anchor.generation, std::memory_order_release);
    control->hostClockAnchor.mirrorPublications.fetch_add(
        1, std::memory_order_relaxed);
    control->counters.CountRxAdkZtsPublished();

    if (logSuccess) {
        ASFW_LOG(
            DirectAudio,
            "ADK ZTS publish reason=%{public}s generation=%llu sample=%llu host=%llu adkPeriod=%u",
            reason ? reason : "unknown",
            anchor.generation,
            anchor.sampleFrame,
            anchor.hostTicks,
            audioDevice->GetZeroTimestampPeriod());
    }

    if (firstPublication) {
        ASFW_LOG(
            DirectAudio,
            "Core audio hardware ZTS ready guid=0x%016llx sampleFrame=%llu hostTicks=%llu",
            ivars.device.guid,
            anchor.sampleFrame,
            anchor.hostTicks);
    }
    return ASFW::Audio::Runtime::ZtsMirrorPublishResult::Published;
}

uint32_t PrepareTransmitSlots(ASFWAudioDriver_IVars& ivars,
                             uint64_t startPacketIndex,
                             uint64_t requiredPacketIndex,
                             uint64_t limitPacketIndex,
                             uint32_t maxToPrepare,
                             uint64_t targetFrameEnd,
                             bool allowRecoveredClock) noexcept {
    const uint32_t numSlots = ivars.runtime.txSlotProvider.numSlots;
    auto* metadataRing = ivars.runtime.txSlotProvider.metadataRing;
    auto* directControl = ivars.runtime.directAudioGraph.control;
    if (directControl == nullptr) {
        return 0;
    }

    uint64_t nextPacketToPrepare = startPacketIndex;
    uint32_t preparedCount = 0;

    const auto recordContentFault =
        [&](ASFW::Audio::Runtime::TxContentFaultReason reason,
            uint64_t packetIndex,
            uint64_t audioFrame) noexcept {
            directControl->txContentFaultEvents.fetch_add(
                1, std::memory_order_relaxed);
            if (directControl->txContentFirstFaultReason.load(
                    std::memory_order_acquire) !=
                static_cast<uint32_t>(
                    ASFW::Audio::Runtime::TxContentFaultReason::kNone)) {
                return;
            }
            directControl->txContentFirstFaultPacket.store(
                packetIndex, std::memory_order_relaxed);
            directControl->txContentFirstFaultAudioFrame.store(
                audioFrame, std::memory_order_relaxed);
            directControl->txContentFirstFaultOldestFrame.store(
                ivars.runtime.txPcmStagingRing.OldestValidFrame(),
                std::memory_order_relaxed);
            directControl->txContentFirstFaultWrittenEndFrame.store(
                ivars.runtime.txPcmStagingRing.WrittenEndFrame(),
                std::memory_order_relaxed);
            directControl->txContentFirstFaultCompletionCursor.store(
                ivars.runtime.txSlotProvider.queueControl
                    ? ivars.runtime.txSlotProvider.queueControl
                          ->completionCursor.load(std::memory_order_relaxed)
                    : 0,
                std::memory_order_relaxed);
            directControl->txContentFirstFaultCommittedEnd.store(
                ivars.runtime.txSlotProvider.queueControl
                    ? ivars.runtime.txSlotProvider.queueControl
                          ->committedEnd.load(std::memory_order_relaxed)
                    : 0,
                std::memory_order_relaxed);
            directControl->txContentFirstFaultReason.store(
                static_cast<uint32_t>(reason),
                std::memory_order_release);
        };

    const auto failProducer =
        [&](ASFW::Audio::Runtime::TxProducerFaultStage stage,
            ASFW::Audio::Runtime::TxProducerFaultReason producerReason,
            ASFW::Audio::Runtime::FatalStreamReason runtimeReason,
            uint64_t packetIndex) noexcept {
            auto* txControl =
                ivars.runtime.txSlotProvider.queueControl;
            const uint64_t completionCursor =
                txControl
                    ? txControl->completionCursor.load(
                          std::memory_order_acquire)
                    : 0;
            const uint64_t committedEnd =
                txControl
                    ? txControl->committedEnd.load(
                          std::memory_order_acquire)
                    : 0;

            ASFW::Audio::Runtime::TxProducerFaultRecord failure{
                .stage = stage,
                .reason = producerReason,
                .packetIndex = packetIndex,
                .rangeStart = startPacketIndex,
                .rangeTarget = limitPacketIndex,
                .preparedCount = preparedCount,
                .completionCursor = completionCursor,
                .committedEnd = committedEnd,
                .replayProducerCursor =
                    directControl->rxSequenceReplay.ProducerCursor(),
                .replayEpoch =
                    directControl->rxSequenceReplay.Epoch(),
            };
            const uint64_t producerGeneration =
                directControl->txProducerFault.Publish(failure);

            directControl->fatalReason.store(
                runtimeReason, std::memory_order_release);
            const uint64_t runtimeGeneration =
                directControl->fatalGeneration.fetch_add(
                    1, std::memory_order_release) +
                1;
            directControl->counters.txImmediateStops.fetch_add(
                1, std::memory_order_relaxed);

            ASFW_LOG(
                DirectAudio,
                "[TxProducerFatal] stage=%{public}s reason=%{public}s "
                "producerGen=%llu runtimeReason=%u runtimeGen=%llu "
                "packet=%llu range=[%llu,%llu) prepared=%u "
                "completion=%llu committedEnd=%llu replayProducer=%llu "
                "replayEpoch=%u",
                ASFW::Audio::Runtime::TxProducerFaultStageName(stage),
                ASFW::Audio::Runtime::TxProducerFaultReasonName(
                    producerReason),
                producerGeneration,
                static_cast<uint32_t>(runtimeReason),
                runtimeGeneration,
                packetIndex,
                startPacketIndex,
                limitPacketIndex,
                preparedCount,
                completionCursor,
                committedEnd,
                failure.replayProducerCursor,
                failure.replayEpoch);

            if (txControl) {
                txControl->statusWord.store(
                    ASFW::Isoch::IsochTxQueueStatus::kProducerFault,
                    std::memory_order_release);
            }
            ivars.runtime.txActive.store(
                false, std::memory_order_release);
        };

    if (numSlots == 0 || metadataRing == nullptr ||
        ivars.runtime.txSlotProvider.queueControl == nullptr) {
        failProducer(
            ASFW::Audio::Runtime::TxProducerFaultStage::kPreflight,
            ASFW::Audio::Runtime::TxProducerFaultReason::
                kInvalidTransport,
            ASFW::Audio::Runtime::FatalStreamReason::
                InvalidGeometry,
            startPacketIndex);
        return 0;
    }

    auto frameTargetSatisfied = [&]() noexcept {
        return targetFrameEnd == 0 ||
               ivars.runtime.txStreamEngine.Timeline().FinalizedFrameEnd() >=
                   targetFrameEnd;
    };

    while (nextPacketToPrepare < limitPacketIndex &&
           preparedCount < maxToPrepare) {
        if (nextPacketToPrepare >= requiredPacketIndex &&
            frameTargetSatisfied()) {
            break;
        }

        ASFW::Protocols::Audio::AMDTP::AmdtpTimingState timing{};
        bool replayEntryPeeked = false;
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
                failProducer(
                    ASFW::Audio::Runtime::TxProducerFaultStage::
                        kExecutionAnchor,
                    ASFW::Audio::Runtime::TxProducerFaultReason::
                        kReplayUnavailable,
                    ASFW::Audio::Runtime::FatalStreamReason::
                        TxReplayUnavailable,
                    nextPacketToPrepare);
                break;
            }

            // A replay stall is transient, not fatal. RX bumps its replay epoch
            // on every rebind/discontinuity (aggregate StartIO/StopIO churn, a
            // packet gap), which invalidates the reader's epoch, and the reader
            // can momentarily outrun the producer. Killing TX here would leave the
            // stream permanently silent -- the timing-loss recovery is health-gated
            // when the device clock is fine (see DiceAudioBackend), and even ungated
            // a coordinator restart cannot re-prime TX. Persistent unavailability
            // degrades to silence, which is the correct "nothing to send yet"
            // state, not a stream death.
            if (!ivars.runtime.txReplayReader.IsActive()) {
                (void)ivars.runtime.txReplayReader.Begin(
                    directControl->rxSequenceReplay);
            }

            ASFW::Audio::Runtime::RxSequenceEntry replay{};
            ASFW::Audio::Runtime::RxSequenceReplayReadDiagnostic replayDiagnostic{};
            bool replayReadable = ivars.runtime.txReplayReader.TryPeek(
                directControl->rxSequenceReplay, replay,
                &replayDiagnostic);
            // A reader that fell out of the bounded 512-entry RX history is
            // repositionable, not faulted: Begin() re-anchors kReadDelay
            // behind the live producer and the skipped entries only shift
            // NODATA placement, which IEC 61883-6 blocking permits (DBC
            // continuity is packetizer-owned; the SYT offset drifts sub-tick
            // across the skipped span). Frame-cursor alignment must NOT
            // re-arm for this: re-projecting abandons the established
            // host-frame mapping and orphans every host frame behind the new
            // cursor (the all-zero-payload Duet zombie of 2026-07-19).
            if (!replayReadable &&
                replayDiagnostic.failure ==
                    ASFW::Audio::Runtime::RxSequenceReplayReadFailure::
                        kHistoryOverwritten) {
                if (ivars.runtime.txReplayReader.Begin(
                        directControl->rxSequenceReplay)) {
                    replayReadable = ivars.runtime.txReplayReader.TryPeek(
                        directControl->rxSequenceReplay, replay,
                        &replayDiagnostic);
                }
                bool selfHealed = false;
                const bool masterAligned = ivars.runtime.txStreamEngine.IsFrameCursorAligned();
                const bool secondaryAligned = ivars.runtime.txSecondaryActive && ivars.runtime.txStreamEngineSecondary.IsFrameCursorAligned();
                if (masterAligned || secondaryAligned) {
                    const uint64_t finalizedFrame =
                        ivars.runtime.txStreamEngine.Timeline().FinalizedFrameEnd();
                    const uint64_t writeFrame =
                        directControl->txExposureSampleWriteFrame.load(
                            std::memory_order_relaxed);
                    if (writeFrame > finalizedFrame) {
                        if (masterAligned) {
                            ivars.runtime.txStreamEngine.ReArmFrameCursorAlignment();
                        }
                        if (secondaryAligned) {
                            ivars.runtime.txStreamEngineSecondary
                                .ReArmFrameCursorAlignment();
                        }
                        selfHealed = true;
                    }
                }
                ASFW_LOG_RING_ONLY_RL(
                    DirectAudio,
                    "tx-replay-reclamp",
                    1000u,
                    ::ASFW::Logging::LogLevel::Warning,
                    "[TxReplay] reclamped pkt=%llu cur=%llu prod=%llu ok=%u selfHealed=%u",
                    nextPacketToPrepare,
                    replayDiagnostic.readerCursor,
                    replayDiagnostic.producerCursor,
                    replayReadable ? 1u : 0u,
                    selfHealed ? 1u : 0u);
            }
            if (replayReadable) {
                replayEntryPeeked = true;
                timing.replayDataBlocks = replay.dataBlocks;
            } else {
                const int64_t replayDistance =
                    replayDiagnostic.readerCursor >= replayDiagnostic.producerCursor
                        ? static_cast<int64_t>(replayDiagnostic.readerCursor -
                                               replayDiagnostic.producerCursor)
                        : -static_cast<int64_t>(replayDiagnostic.producerCursor -
                                                replayDiagnostic.readerCursor);
                // This is the primary discriminator for a TX silence: it says
                // whether RX had not produced this entry yet, had overwritten it,
                // reset its epoch, or changed the slot while it was being read.
                ASFW_LOG_RING_ONLY_RL(
                    DirectAudio,
                    "tx-replay-read",
                    1000u,
                    ::ASFW::Logging::LogLevel::Warning,
                    "[TxReplay] fail=%s pkt=%llu cur=%llu prod=%llu d=%lld ep=%u/%u slot=%llu/%u est=%u",
                    ASFW::Audio::Runtime::RxSequenceReplayReadFailureName(
                        replayDiagnostic.failure),
                    nextPacketToPrepare,
                    replayDiagnostic.readerCursor,
                    replayDiagnostic.producerCursor,
                    replayDistance,
                    replayDiagnostic.readerEpoch,
                    replayDiagnostic.replayEpoch,
                    replayDiagnostic.slotSequence,
                    replayDiagnostic.slotEpoch,
                    replayDiagnostic.replayEstablished ? 1u : 0u);
                directControl->txReplayUnderflows.fetch_add(
                    1, std::memory_order_relaxed);
                timing.replayDataBlocks = 0;
                if (replayDiagnostic.failure ==
                    ASFW::Audio::Runtime::RxSequenceReplayReadFailure::
                        kAheadOfProducer) {
                    if (nextPacketToPrepare >= requiredPacketIndex) {
                        // Optional preparation must wait for real recovered
                        // timing. Filling the entire deep queue with guessed
                        // NO-DATA merely because F has not reached W burns the
                        // replay cushion and creates avoidable content delay.
                        // Hard transport coverage below `required` still uses
                        // explicit NO-DATA without consuming the replay/PCM
                        // cursors.
                        directControl->txContentDeferrals.fetch_add(
                            1, std::memory_order_relaxed);
                        break;
                    }
                    // RX simply has not published this entry yet (a deep
                    // preparation burst outran real-time RX). Hold the reader
                    // where it is and ship one NODATA packet; the same cursor
                    // reads successfully once RX catches up. Resetting the
                    // reader or re-arming alignment here turns a transient,
                    // self-resolving condition into a frame-cursor jump that
                    // abandons host frames.
                } else {
                    // Epoch change, establishment loss, or a seqlock miss: the
                    // RX timing domain itself moved. Drop the reader so the
                    // next packet re-Begins on the live epoch and re-arm the
                    // frame-cursor alignment: while stalled we emit NO-DATA
                    // packets, which do NOT advance the content-frame cursor,
                    // so it freezes at its pre-stall frame while CoreAudio
                    // keeps writing. Re-arming makes the first DATA packet
                    // after replay recovers re-project the cursor to the live
                    // frame, closing the gap.
                    // This branch is what arms the [TxAlign] self-heal, and it
                    // was silent: a recurring re-anchor showed up as a ~112 ms
                    // frame-cursor jump with nothing naming the cause. The
                    // reasons are not equivalent - kHistoryOverwritten means the
                    // reader fell behind, which is NOT the timing-domain move
                    // this branch assumes, and re-anchoring would hide it. Name
                    // the reason so the two cases can be told apart.
                    ASFW_LOG_ERROR(
                        DirectAudio,
                        "[TxReplayRearm] reason=%{public}s cur=%llu prod=%llu ep=%u/%u "
                        "slot=%llu/%u est=%u",
                        ASFW::Audio::Runtime::RxSequenceReplayReadFailureName(
                            replayDiagnostic.failure),
                        replayDiagnostic.readerCursor,
                        replayDiagnostic.producerCursor,
                        replayDiagnostic.readerEpoch,
                        replayDiagnostic.replayEpoch,
                        replayDiagnostic.slotSequence,
                        replayDiagnostic.slotEpoch,
                        replayDiagnostic.replayEstablished ? 1u : 0u);
                    ivars.runtime.txReplayReader.Reset();
                    ivars.runtime.txStreamEngine.ReArmFrameCursorAlignment();
                    if (ivars.runtime.txSecondaryActive) {
                        ivars.runtime.txStreamEngineSecondary
                            .ReArmFrameCursorAlignment();
                    }
                }
            }

            if (replay.dataBlocks != 0) {
                if (replay.sytOffset ==
                        ASFW::Audio::Runtime::
                            RxSequenceReplayState::kNoInfo ||
                    (replay.flags &
                     ASFW::Audio::Runtime::RxSequenceFlags::
                         kValidSyt) == 0) {
                    directControl->txReplayInvalidSyt.fetch_add(
                        1, std::memory_order_relaxed);
                    failProducer(
                        ASFW::Audio::Runtime::TxProducerFaultStage::
                            kReplaySytValidation,
                        ASFW::Audio::Runtime::TxProducerFaultReason::
                                kInvalidReplaySyt,
                        ASFW::Audio::Runtime::FatalStreamReason::
                            TxReplayInvalidSyt,
                        nextPacketToPrepare);
                    break;
                }

                timing.txClockValid = true;
                timing.disposition =
                    ASFW::Protocols::Audio::AMDTP::
                        AmdtpPacketDisposition::Data;
                const uint32_t txDelay =
                    directControl->txTransferDelayTicks.load(
                        std::memory_order_relaxed);
                timing.nextDataSyt =
                    ASFW::Audio::Runtime::
                        ComputeReplaySytFromTicks(
                            replay.sytOffset,
                            packetAnchorTicks,
                            txDelay);

                // Publish the live SYT decision to a lock-free latest-value
                // trace. The watchdog logs it off the hot path (~1 s) so the
                // observed device SYT, the delay-free replay offset, and the
                // re-anchored transmit SYT are visible without logging here.
                // `observedRxSyt` is the device's original SYT, reconstructed
                // from the replayed delay-free offset against its source cycle.
                ASFW::Audio::Runtime::TxSytTraceSample trace{};
                trace.packetIndex = nextPacketToPrepare;
                trace.sourceCycle =
                    ASFW::Timing::decodeCycleTimer(
                        replay.sourceCycleTimer)
                        .cycle;
                trace.outCycle = static_cast<uint32_t>(
                    (ASFW::Timing::normalizeOffsetDomain(
                         packetAnchorTicks) /
                     ASFW::Timing::kTicksPerCycle) %
                    ASFW::Timing::kCyclesPerSecond);
                trace.sytOffsetDelayFree = replay.sytOffset;
                trace.txDelayTicks = txDelay;
                trace.observedRxSyt =
                    ASFW::Audio::Runtime::ComputeReplaySyt(
                        replay.sytOffset,
                        replay.sourceCycleTimer,
                        directControl->rxTransferDelayTicks.load(
                            std::memory_order_relaxed));
                trace.txSyt = timing.nextDataSyt;
                directControl->txSytTrace.Publish(trace);

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
                    // ticks -> frames at the live rate. 44.1k has no integer
                    // ticks/sample (24576000/44100 ~= 557.28), so divide the
                    // tick*rate product instead of dividing by a per-sample
                    // constant (the old /512 overshot ~8.8% at 44.1k).
                    const auto& txConfig =
                        ivars.runtime.txStreamEngine.StreamConfig();
                    const uint32_t kFramesPerPacket =
                        txConfig.framesPerDataPacket;
                    const uint64_t projectedFrame =
                        replay.firstAudioFrame +
                        (static_cast<uint64_t>(presentationDeltaTicks) *
                         txConfig.sampleRate) /
                            ASFW::Timing::kTicksPerSecond;
                    const uint64_t oldestRetainedFrame =
                        ivars.runtime.txPcmStagingRing.OldestValidFrame();
                    const uint64_t writtenEndFrame =
                        ivars.runtime.txPcmStagingRing.WrittenEndFrame();
                    const auto selection =
                        ASFW::Audio::Runtime::SelectCompletePcmPacket(
                            projectedFrame,
                            oldestRetainedFrame,
                            writtenEndFrame,
                            kFramesPerPacket);
                    bool aligned = false;
                    if (selection.available) {
                        aligned = ivars.runtime.txStreamEngine
                                      .AlignFrameCursorOnce(
                                          selection.firstFrame);
                        if (ivars.runtime.txSecondaryActive) {
                            aligned = ivars.runtime.txStreamEngineSecondary
                                          .AlignFrameCursorOnce(
                                              selection.firstFrame) ||
                                      aligned;
                        }
                    }
                    // Fires once at stream start, then again only after a real
                    // timing-domain loss or an overwritten PCM range. The
                    // selected frame is guaranteed to name one complete
                    // retained DATA packet; the producer frontier can contain
                    // only a prefix (the live DICE failure had 4 of 8 frames).
                    if (aligned) {
                        ASFW_LOG(DirectAudio,
                                 "[TxAlign] frame cursor -> %llu (projected=%llu "
                                 "rxFirstFrame=%llu deltaTicks=%lld rate=%u "
                                 "staged=[%llu,%llu))",
                                 selection.firstFrame,
                                 projectedFrame,
                                 replay.firstAudioFrame,
                                 static_cast<long long>(presentationDeltaTicks),
                                 txConfig.sampleRate,
                                 oldestRetainedFrame,
                                 writtenEndFrame);
                    }
                }
            }
        }

        auto prepareResult =
            ivars.runtime.txStreamEngine.PrepareNextTransmitSlot(
                static_cast<uint32_t>(nextPacketToPrepare),
                timing);

        using PrepareResult =
            ASFW::Protocols::Audio::DICE::TxSlotPrepareResult;
        const bool pcmNotReady =
            prepareResult == PrepareResult::kPcmNotYetWritten ||
            prepareResult == PrepareResult::kPcmSnapshotBusy;
        const bool pcmStale =
            prepareResult == PrepareResult::kPcmStaleOverwritten;
        if (pcmNotReady || pcmStale) {
            const uint64_t audioFrame =
                ivars.runtime.txStreamEngine.PacketizerTelemetrySnapshot()
                    .nextAudioFrame;
            const bool mustCoverTransport =
                nextPacketToPrepare < requiredPacketIndex;
            const auto shortageKind =
                pcmStale
                    ? ASFW::Audio::Runtime::TxContentShortageKind::
                          kStaleOverwritten
                    : (prepareResult == PrepareResult::kPcmSnapshotBusy
                           ? ASFW::Audio::Runtime::TxContentShortageKind::
                                 kSnapshotBusy
                           : ASFW::Audio::Runtime::TxContentShortageKind::
                                 kNotYetWritten);
            const auto shortageAction =
                ASFW::Audio::Runtime::DecideTxContentShortageAction(
                    shortageKind, mustCoverTransport);

            if (shortageAction ==
                ASFW::Audio::Runtime::TxContentShortageAction::kDefer) {
                // Future host content does not exist yet. Leave the packet and
                // replay cursor untouched; a later WriteEnd retries the exact
                // same decision with a newly published staging range.
                directControl->txContentDeferrals.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            }

            ASFW::Audio::Runtime::TxContentFaultReason contentReason =
                ASFW::Audio::Runtime::TxContentFaultReason::
                    kStaleOverwritten;
            if (prepareResult == PrepareResult::kPcmNotYetWritten) {
                contentReason = ASFW::Audio::Runtime::TxContentFaultReason::
                    kNotYetWrittenAtDeadline;
            } else if (prepareResult == PrepareResult::kPcmSnapshotBusy) {
                contentReason = ASFW::Audio::Runtime::TxContentFaultReason::
                    kSnapshotBusyAtDeadline;
            }
            recordContentFault(
                contentReason, nextPacketToPrepare, audioFrame);

            if (pcmStale) {
                // Waiting cannot recover an overwritten frame. Commit one
                // explicit NO-DATA cycle without consuming PCM, then let the
                // next valid replay entry project both stream cursors onto the
                // live host range.
                directControl->txContentStaleXruns.fetch_add(
                    1, std::memory_order_relaxed);
            } else {
                // Optional preparation already deferred above. At hard
                // coverage publish explicit NO-DATA, but keep the recoverable
                // PCM cursor. The next DATA opportunity retries this exact
                // range after WriteEnd publishes the missing suffix. Re-arming
                // here fed the live Align -> 4/8 ready -> NO-DATA loop forever.
                directControl->txContentDeadlineNoData.fetch_add(
                    1, std::memory_order_relaxed);
            }
            if (shortageAction ==
                ASFW::Audio::Runtime::TxContentShortageAction::
                    kEmitNoDataRebaseCursor) {
                directControl->txContentRebases.fetch_add(
                    1, std::memory_order_relaxed);
                ivars.runtime.txStreamEngine.ReArmFrameCursorAlignment();
                if (ivars.runtime.txSecondaryActive) {
                    ivars.runtime.txStreamEngineSecondary
                        .ReArmFrameCursorAlignment();
                }
            }
            directControl->counters.txUnderruns.fetch_add(
                1, std::memory_order_relaxed);
            directControl->counters.txSilenceSubstitutions.fetch_add(
                1, std::memory_order_relaxed);

            ASFW_LOG_RING_ONLY_RL(
                DirectAudio,
                pcmStale ? "tx-content-stale" : "tx-content-deadline",
                pcmStale ? 0u : 1000u,
                ::ASFW::Logging::LogLevel::Warning,
                "[TxContent] action=%s reason=%s packet=%llu frame=%llu staged=[%llu,%llu) required=%llu completion=%llu committed=%llu",
                pcmStale ? "stale-xrun-rebase-nodata"
                         : "deadline-xrun-hold-nodata",
                ASFW::Audio::Runtime::TxContentFaultReasonName(contentReason),
                nextPacketToPrepare,
                audioFrame,
                ivars.runtime.txPcmStagingRing.OldestValidFrame(),
                ivars.runtime.txPcmStagingRing.WrittenEndFrame(),
                requiredPacketIndex,
                ivars.runtime.txSlotProvider.queueControl
                    ->completionCursor.load(std::memory_order_relaxed),
                ivars.runtime.txSlotProvider.queueControl
                    ->committedEnd.load(std::memory_order_relaxed));

            timing.disposition =
                ASFW::Protocols::Audio::AMDTP::
                    AmdtpPacketDisposition::NoData;
            timing.replayDataBlocks = 0;
            prepareResult =
                ivars.runtime.txStreamEngine.PrepareNextTransmitSlot(
                    static_cast<uint32_t>(nextPacketToPrepare), timing);
        }

        if (prepareResult !=
            PrepareResult::kPrepared) {
            ASFW::Audio::Runtime::TxProducerFaultStage stage =
                ASFW::Audio::Runtime::TxProducerFaultStage::kSlotAcquire;
            ASFW::Audio::Runtime::TxProducerFaultReason producerReason =
                ASFW::Audio::Runtime::TxProducerFaultReason::
                    kSlotUnavailable;
            ASFW::Audio::Runtime::FatalStreamReason runtimeReason =
                ASFW::Audio::Runtime::FatalStreamReason::
                    TxSlotInvariant;

            switch (prepareResult) {
                case ASFW::Protocols::Audio::DICE::
                    TxSlotPrepareResult::kPacketizerRejected:
                    stage =
                        ASFW::Audio::Runtime::TxProducerFaultStage::
                            kPacketize;
                    producerReason =
                        ASFW::Audio::Runtime::TxProducerFaultReason::
                                kPacketizerRejected;
                    runtimeReason =
                        ASFW::Audio::Runtime::FatalStreamReason::
                            InvalidGeometry;
                    break;
                case ASFW::Protocols::Audio::DICE::
                    TxSlotPrepareResult::kSlotPublishFailed:
                    stage =
                        ASFW::Audio::Runtime::TxProducerFaultStage::
                            kSlotPublish;
                    producerReason =
                        ASFW::Audio::Runtime::TxProducerFaultReason::
                                kSlotPublishFailed;
                    break;
                case ASFW::Protocols::Audio::DICE::
                    TxSlotPrepareResult::kSlotProviderUnavailable:
                case ASFW::Protocols::Audio::DICE::
                    TxSlotPrepareResult::kPcmSourceUnavailable:
                case ASFW::Protocols::Audio::DICE::
                    TxSlotPrepareResult::kPcmNotYetWritten:
                case ASFW::Protocols::Audio::DICE::
                    TxSlotPrepareResult::kPcmStaleOverwritten:
                case ASFW::Protocols::Audio::DICE::
                    TxSlotPrepareResult::kPcmSnapshotBusy:
                case ASFW::Protocols::Audio::DICE::
                    TxSlotPrepareResult::kPcmInvalidRequest:
                case ASFW::Protocols::Audio::DICE::
                    TxSlotPrepareResult::kSlotAcquireFailed:
                    if (prepareResult ==
                            PrepareResult::kPcmSourceUnavailable ||
                        prepareResult ==
                            PrepareResult::kPcmInvalidRequest) {
                        stage =
                            ASFW::Audio::Runtime::TxProducerFaultStage::
                                kPacketize;
                        producerReason =
                            ASFW::Audio::Runtime::TxProducerFaultReason::
                                kPacketizerRejected;
                        runtimeReason =
                            ASFW::Audio::Runtime::FatalStreamReason::
                                InvalidGeometry;
                        recordContentFault(
                            ASFW::Audio::Runtime::TxContentFaultReason::
                                kInvalidSource,
                            nextPacketToPrepare,
                            ivars.runtime.txStreamEngine
                                .PacketizerTelemetrySnapshot()
                                .nextAudioFrame);
                    }
                    break;
                case ASFW::Protocols::Audio::DICE::
                    TxSlotPrepareResult::kPrepared:
                    break;
            }
            failProducer(
                stage,
                producerReason,
                runtimeReason,
                nextPacketToPrepare);
            break;
        }

        // Shadow the master's per-packet timing on the secondary stream so both
        // device RX streams advance in lockstep (same packetIndex/DBC/SYT/
        // disposition), differing only in payload (channels 17–32). A failure
        // is loud: silently letting one DICE stream advance alone creates a
        // second, much harder-to-diagnose cursor divergence.
        if (ivars.runtime.txSecondaryActive) {
            const auto secondaryResult =
                ivars.runtime.txStreamEngineSecondary.PrepareNextTransmitSlot(
                static_cast<uint32_t>(nextPacketToPrepare), timing);
            if (secondaryResult != PrepareResult::kPrepared) {
                recordContentFault(
                    ASFW::Audio::Runtime::TxContentFaultReason::
                        kSecondaryStreamFailure,
                    nextPacketToPrepare,
                    ivars.runtime.txStreamEngineSecondary
                        .PacketizerTelemetrySnapshot()
                        .nextAudioFrame);
                failProducer(
                    ASFW::Audio::Runtime::TxProducerFaultStage::kPacketize,
                    ASFW::Audio::Runtime::TxProducerFaultReason::
                        kPacketizerRejected,
                    ASFW::Audio::Runtime::FatalStreamReason::
                        TxSlotInvariant,
                    nextPacketToPrepare);
                break;
            }
        }

        if (replayEntryPeeked) {
            ivars.runtime.txReplayReader.Advance();
            directControl->txReplayEntries.fetch_add(
                1, std::memory_order_relaxed);
        }

        const uint64_t finalizedFrameEnd =
            ivars.runtime.txStreamEngine.Timeline().FinalizedFrameEnd();
        directControl->txContentFinalizedFrameEnd.store(
            finalizedFrameEnd, std::memory_order_release);
        directControl->playbackRingReadFrame.store(
            finalizedFrameEnd, std::memory_order_release);

        const uint32_t slotIdx =
            static_cast<uint32_t>(
                nextPacketToPrepare % numSlots);
        const auto& meta = metadataRing[slotIdx];
        if (meta.payloadLength > 8) {
            directControl->counters.txDataPackets.fetch_add(
                1, std::memory_order_relaxed);
            directControl->counters.txValidSytPackets.fetch_add(
                1, std::memory_order_relaxed);
        } else if (meta.payloadLength == 0) {
            directControl->counters.txEmptyPackets.fetch_add(
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

    // Commit one complete shared-ring lap before IT RUN. The transport's arm
    // contract validates this exact prefill so that a delayed first producer
    // action cannot expose an uncommitted slot to IT DMA. Steady state still
    // targets completion + kTxPreparationLeadPackets.
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
        if (ivars.runtime.txStreamEngine.PrepareNextTransmitSlot(
                static_cast<uint32_t>(packetIndex), timing) !=
            ASFW::Protocols::Audio::DICE::TxSlotPrepareResult::
                kPrepared) {
            break;
        }
        // Seed the secondary ring in lockstep with the same NO-DATA packets.
        if (ivars.runtime.txSecondaryActive) {
            (void)ivars.runtime.txStreamEngineSecondary.PrepareNextTransmitSlot(
                static_cast<uint32_t>(packetIndex), timing);
        }
        ++prepared;
    }

    ASFW_LOG(DirectAudio,
             "ADK DBG TX prefill seeded %u/%u committed NO-DATA packets before isoch start (steadyLead=%u)",
             prepared,
             numSlots,
             ASFW::Audio::Shared::AudioTimingGeometry::
                 kTxPreparationLeadPackets);
}

} // namespace ASFW::Audio::DriverKit

void IMPL(ASFWAudioDriver, ZtsAnchorReady)
{
    (void)action;
    (void)generation;
    if (!ivars || !ivars->audioDevice) {
        return;
    }

    (void)ASFW::Audio::DriverKit::PublishSharedZeroTimestampToHAL(
        *ivars, "rx-action", false);
}

void IMPL(ASFWAudioDriver, TxPreparationReady)
{
    (void)action;
    (void)generation;
    if (!ivars ||
        !ivars->runtime.txActive.load(
            std::memory_order_acquire)) {
        return;
    }

    auto* txControl = ivars->runtime.txSlotProvider.queueControl;
    const uint32_t numSlots = ivars->runtime.txSlotProvider.numSlots;
    if (!txControl || numSlots == 0) {
        return;
    }

    const uint64_t requested =
        txControl->refillRequestGeneration.load(
            std::memory_order_acquire);
    const uint64_t refillHandled =
        txControl->refillHandledGeneration.load(
            std::memory_order_acquire);
    const bool hardwareWakePending = requested != refillHandled;

    const uint64_t completionCursor =
        txControl->completionCursor.load(std::memory_order_acquire);
    const uint64_t committedEnd =
        txControl->committedEnd.load(std::memory_order_acquire);
    const uint64_t packetCoverageTarget =
        completionCursor +
        ASFW::Audio::Shared::AudioTimingGeometry::
            kTxCoverageLeadPackets;
    const uint64_t packetLimitTarget =
        completionCursor +
        ASFW::Audio::Shared::AudioTimingGeometry::
            kTxPreparationLeadPackets;

    auto* directControl = ivars->runtime.directAudioGraph.control;
    if (directControl) {
        directControl->txTransportCompletionCursor.store(
            completionCursor, std::memory_order_relaxed);
        directControl->txTransportCommittedEnd.store(
            committedEnd, std::memory_order_relaxed);
        directControl->txTransportStatus.store(
            static_cast<uint32_t>(txControl->statusWord.load(
                std::memory_order_acquire)),
            std::memory_order_relaxed);
    }
    const bool replayEstablished =
        directControl && directControl->rxSequenceReplay.IsEstablished();
    const uint64_t audioRequested = directControl
        ? directControl->txPreparationRequests.RequestedGeneration()
        : 0;
    const uint64_t requestedAudioTarget = directControl
        ? directControl->txPreparationRequests.requestedTargetFrameEnd.load(
              std::memory_order_acquire)
        : 0;
    const uint64_t outputWrittenEndFrame =
        directControl ? directControl->client.OutputWrittenEndFrame() : 0;
    const uint32_t dataHorizonFrames =
        ASFW::Audio::Shared::AudioTimingGeometry::TxDataHorizonFrames(
            ivars->runtime.txStreamEngine.StreamConfig().sampleRate);
    // DATA can be finalized only through PCM CoreAudio has actually completed.
    // The 50 ms horizon now sizes retention and diagnostics; it is never a
    // license to commit future zero-filled DATA packets.
    const uint64_t outputTargetFrameEnd = outputWrittenEndFrame;
    const uint64_t targetFrameEnd =
        requestedAudioTarget > outputTargetFrameEnd
            ? requestedAudioTarget
            : outputTargetFrameEnd;
    const uint64_t finalizedFrameEndBefore =
        ivars->runtime.txStreamEngine.Timeline().FinalizedFrameEnd();
    const uint32_t slotsPrepared =
        ASFW::Audio::DriverKit::PrepareTransmitSlots(
            *ivars,
            committedEnd,
            packetCoverageTarget,
            packetLimitTarget,
            ASFW::Audio::Shared::AudioTimingGeometry::
                kTxPreparationLeadPackets,
            targetFrameEnd,
            replayEstablished);
    const uint64_t finalizedFrameEndAfter =
        ivars->runtime.txStreamEngine.Timeline().FinalizedFrameEnd();
    if (directControl) {
        directControl->txTransportCompletionCursor.store(
            txControl->completionCursor.load(std::memory_order_acquire),
            std::memory_order_relaxed);
        directControl->txTransportCommittedEnd.store(
            txControl->committedEnd.load(std::memory_order_acquire),
            std::memory_order_relaxed);
        directControl->txTransportStatus.store(
            static_cast<uint32_t>(txControl->statusWord.load(
                std::memory_order_acquire)),
            std::memory_order_release);
    }

    // [TxPrepRange] Refill-coverage instrumentation. Answers the decisive
    // question: did the producer's range reach `target` this wake, or stop
    // short and leave a hole the IT refill ISR will later trip on? The producer
    // loop is linear in absolute packet index, so `prepareUntil` is exactly
    // `base + slotsPrepared`.
    {
        const uint64_t prepareBaseAbs = committedEnd;
        const uint64_t prepareUntilAbs = committedEnd + slotsPrepared;
        const bool stoppedShort = prepareUntilAbs < packetCoverageTarget;
        const bool frameShort =
            targetFrameEnd != 0 && finalizedFrameEndAfter < targetFrameEnd;
        const uint64_t committedMargin =
            prepareUntilAbs > completionCursor
                ? prepareUntilAbs - completionCursor
                : 0;
        // Basic TX flow is confirmed. Anomaly-only: log a wake that stopped
        // short of the coverage target (hole-producing -- precedes an IT FATAL,
        // proves the underrun is refill-coverage not scheduling margin) or one
        // that could not finalize the requested frame timeline. The steady
        // "nothing to prepare" wake (slotsPrepared == 0, ring already full) is
        // the normal state and no longer logged; the periodic [TxPrep] summary
        // remains the liveness/margin heartbeat.
        if (stoppedShort || frameShort) {
            const uint64_t frameDeficit =
                frameShort ? (targetFrameEnd - finalizedFrameEndAfter) : 0;
            // stoppedShort is rare and precedes an IT FATAL -> always log (interval
            // 0). A frameShort-only wake is a persistent stall (e.g. RX outage
            // NO-DATA) that otherwise floods at ~1 kHz -> rate-limit to ~1/s, with
            // the suppressed-count preserving burst visibility. Keep these hot-path
            // anomalies in the driver ring only; MCP can query them without IO logging.
            ASFW_LOG_RING_ONLY_RL(
                DirectAudio,
                "tx-prep-range",
                stoppedShort ? 0u : 1000u,
                ::ASFW::Logging::LogLevel::Warning,
                "[TxPrepRange] short=%u frame=%u ret=%llu base=%llu until=%llu cov=%llu lim=%llu n=%u margin=%llu",
                stoppedShort ? 1u : 0u,
                frameShort ? 1u : 0u,
                completionCursor,
                prepareBaseAbs,
                prepareUntilAbs,
                packetCoverageTarget,
                packetLimitTarget,
                slotsPrepared,
                committedMargin);
            if (frameShort) {
                ASFW_LOG_RING_ONLY_RL(
                    DirectAudio,
                    "tx-prep-frame",
                    1000u,
                    ::ASFW::Logging::LogLevel::Warning,
                    "[TxPrepFrame] target=%llu before=%llu after=%llu deficit=%llu write=%llu replay=%u",
                    targetFrameEnd,
                    finalizedFrameEndBefore,
                    finalizedFrameEndAfter,
                    frameDeficit,
                    outputWrittenEndFrame,
                    replayEstablished ? 1u : 0u);
            }
        }
    }

    // Keep the legacy W/E snapshot fields populated for older control-plane
    // decoders, but E now means immutable finalized content (F). W-F is normal
    // pending work, not proof that PCM was lost; deadline and stale counters
    // above are the authoritative fault signals.
    if (directControl) {
        directControl->txExposureSampleHostTicks.store(
            mach_absolute_time(), std::memory_order_relaxed);
        directControl->txExposureSampleWriteFrame.store(
            outputWrittenEndFrame, std::memory_order_relaxed);
        directControl->txExposureSampleExposedFrame.store(
            finalizedFrameEndAfter, std::memory_order_relaxed);
        directControl->txExposureReason.store(
            static_cast<uint32_t>(
                ASFW::Audio::Runtime::TxExposureReason::kHealthy),
            std::memory_order_relaxed);
        directControl->txExposurePpm.store(0, std::memory_order_relaxed);
    }

    bool scheduleAudioFollowUp = false;
    if (directControl) {
        const uint64_t now = mach_absolute_time();
        const uint64_t requestedAt =
            hardwareWakePending
                ? txControl->refillRequestHostTicks.load(
                      std::memory_order_relaxed)
                : now;
        const uint64_t latency =
            now >= requestedAt ? now - requestedAt : 0;
        const uint64_t latencyNanos =
            ASFW::Timing::hostTicksToNanos(latency);
        if (hardwareWakePending) {
            directControl->txLastPreparationLatencyTicks.store(
                latency, std::memory_order_relaxed);
            directControl->txPreparationLatencySamples.fetch_add(
                1, std::memory_order_relaxed);
            using Geometry = ASFW::Audio::Shared::AudioTimingGeometry;
            if (latencyNanos <= Geometry::kTxPreparationLatency750Us *
                                    Geometry::kNanosecondsPerMicrosecond) {
                directControl->txPreparationAtMost750Us.fetch_add(
                    1, std::memory_order_relaxed);
            }
            if (latencyNanos >= Geometry::kTxPreparationLatency1500Us *
                                    Geometry::kNanosecondsPerMicrosecond) {
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
            uint64_t previousIntervalMax =
                directControl->txIntervalPreparationLatencyMaxTicks.load(
                    std::memory_order_relaxed);
            while (latency > previousIntervalMax &&
                   !directControl->txIntervalPreparationLatencyMaxTicks
                        .compare_exchange_weak(
                            previousIntervalMax,
                            latency,
                            std::memory_order_relaxed,
                            std::memory_order_relaxed)) {
            }

            const size_t latencyBucket =
                latencyNanos < Geometry::kTxPreparationLatency250Us *
                                   Geometry::kNanosecondsPerMicrosecond
                    ? 0
                    : latencyNanos < Geometry::kTxPreparationLatency500Us *
                                         Geometry::kNanosecondsPerMicrosecond
                          ? 1
                          : latencyNanos < Geometry::kTxPreparationLatency750Us *
                                                Geometry::kNanosecondsPerMicrosecond
                                ? 2
                                : latencyNanos < Geometry::kTxPreparationLatency1000Us *
                                                       Geometry::kNanosecondsPerMicrosecond
                                      ? 3
                                      : latencyNanos < Geometry::kTxPreparationLatency1500Us *
                                                             Geometry::kNanosecondsPerMicrosecond
                                            ? 4
                                            : 5;
            directControl->txIntervalPreparationLatencyHistogram[latencyBucket]
                .fetch_add(1, std::memory_order_relaxed);
        }
        const uint64_t distance =
            packetLimitTarget > committedEnd
                ? packetLimitTarget - committedEnd
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
            committedEnd > completionCursor
                ? committedEnd - completionCursor
                : 0;
        const uint32_t boundedMargin =
            committedMargin > UINT32_MAX
                ? UINT32_MAX
                : static_cast<uint32_t>(committedMargin);
        directControl->txCurrentCommittedMarginPackets.store(
            boundedMargin, std::memory_order_relaxed);
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
        uint32_t previousIntervalMarginMin =
            directControl->txIntervalCommittedMarginMinPackets.load(
                std::memory_order_relaxed);
        while (boundedMargin < previousIntervalMarginMin &&
               !directControl->txIntervalCommittedMarginMinPackets
                    .compare_exchange_weak(
                        previousIntervalMarginMin,
                        boundedMargin,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
        }
        uint32_t previousIntervalMarginMax =
            directControl->txIntervalCommittedMarginMaxPackets.load(
                std::memory_order_relaxed);
        while (boundedMargin > previousIntervalMarginMax &&
               !directControl->txIntervalCommittedMarginMaxPackets
                    .compare_exchange_weak(
                        previousIntervalMarginMax,
                        boundedMargin,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
        }
        using Geometry = ASFW::Audio::Shared::AudioTimingGeometry;
        const size_t marginBucket =
            boundedMargin < Geometry::kTxCommittedMargin2xFloorPackets
                ? 0
                : boundedMargin < Geometry::kTxCommittedMargin4xFloorPackets
                      ? 1
                      : boundedMargin < Geometry::kTxCommittedMargin8xFloorPackets
                            ? 2
                            : boundedMargin < Geometry::kTxCommittedMargin16xFloorPackets
                                  ? 3
                                  : 4;
        directControl->txIntervalCommittedMarginHistogram[marginBucket]
            .fetch_add(1, std::memory_order_relaxed);

        // [TxPrep] Surface the cross-queue preparation health to the log. The
        // refill ISR trips kUnderrunFatal once committedMargin falls to the
        // hardware-owned ring depth, so emit on every new committed-margin low,
        // on every wake beyond the 1.5 ms early-warning threshold, and on a
        // coarse heartbeat. The actual geometry budget is encoded in
        // kTxPreparationSlackPackets. See documentation/ZTS_AND_SYT.md §13.
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
            ASFW::Audio::Shared::AudioTimingGeometry::kTxHardwareRingPackets;
        const bool newCommittedMarginLow =
            boundedMargin < committedMarginFloorBefore;
        const bool slackBudgetExceeded =
            latencyNanos >= Geometry::kTxPreparationLatency1500Us *
                                Geometry::kNanosecondsPerMicrosecond;

        // Wall-clock heartbeat. A wake-count trigger is rate-dependent: the
        // same divisor emits ~1.3 lines/s at 48 kHz and 2-4x that at 96/192 kHz,
        // where ring retention matters most. Both anomaly triggers below are
        // independent of this, so pacing costs no fault coverage.
        constexpr uint64_t kHeartbeatIntervalNanos = 5'000'000'000ULL;
        const uint64_t lastHeartbeatTicks =
            directControl->txHeartbeatLastHostTicks.load(
                std::memory_order_relaxed);
        const bool heartbeatDue =
            lastHeartbeatTicks == 0 || now <= lastHeartbeatTicks ||
            ASFW::Timing::hostTicksToNanos(now - lastHeartbeatTicks) >=
                kHeartbeatIntervalNanos;

        if (newCommittedMarginLow || slackBudgetExceeded || heartbeatDue) {
            // Anomaly emissions intentionally close an interval early. This
            // keeps every retained [TxPrep] line self-contained and leaves the
            // normal healthy interval wall-clock paced at five seconds.
            const uint32_t intervalMarginMin =
                directControl->txIntervalCommittedMarginMinPackets.exchange(
                    UINT32_MAX, std::memory_order_relaxed);
            const uint32_t intervalMarginMax =
                directControl->txIntervalCommittedMarginMaxPackets.exchange(
                    0, std::memory_order_relaxed);
            const uint64_t intervalLatencyMaxNanos =
                ASFW::Timing::hostTicksToNanos(
                    directControl->txIntervalPreparationLatencyMaxTicks.exchange(
                        0, std::memory_order_relaxed));
            const uint64_t latencyBucket0 =
                directControl->txIntervalPreparationLatencyHistogram[0].exchange(
                    0, std::memory_order_relaxed);
            const uint64_t latencyBucket1 =
                directControl->txIntervalPreparationLatencyHistogram[1].exchange(
                    0, std::memory_order_relaxed);
            const uint64_t latencyBucket2 =
                directControl->txIntervalPreparationLatencyHistogram[2].exchange(
                    0, std::memory_order_relaxed);
            const uint64_t latencyBucket3 =
                directControl->txIntervalPreparationLatencyHistogram[3].exchange(
                    0, std::memory_order_relaxed);
            const uint64_t latencyBucket4 =
                directControl->txIntervalPreparationLatencyHistogram[4].exchange(
                    0, std::memory_order_relaxed);
            const uint64_t latencyBucket5 =
                directControl->txIntervalPreparationLatencyHistogram[5].exchange(
                    0, std::memory_order_relaxed);
            const uint64_t marginBucket0 =
                directControl->txIntervalCommittedMarginHistogram[0].exchange(
                    0, std::memory_order_relaxed);
            const uint64_t marginBucket1 =
                directControl->txIntervalCommittedMarginHistogram[1].exchange(
                    0, std::memory_order_relaxed);
            const uint64_t marginBucket2 =
                directControl->txIntervalCommittedMarginHistogram[2].exchange(
                    0, std::memory_order_relaxed);
            const uint64_t marginBucket3 =
                directControl->txIntervalCommittedMarginHistogram[3].exchange(
                    0, std::memory_order_relaxed);
            const uint64_t marginBucket4 =
                directControl->txIntervalCommittedMarginHistogram[4].exchange(
                    0, std::memory_order_relaxed);
            // Publish a stable copy for the read-only user-client snapshot.
            // No control-plane caller receives directControl itself.
            directControl->txCompletedIntervalSequence.fetch_add(
                1, std::memory_order_relaxed);
            directControl->txCompletedIntervalMarginMinPackets.store(
                intervalMarginMin, std::memory_order_relaxed);
            directControl->txCompletedIntervalMarginMaxPackets.store(
                intervalMarginMax, std::memory_order_relaxed);
            directControl->txCompletedIntervalPreparationLatencyMaxTicks.store(
                ASFW::Timing::nanosToHostTicks(intervalLatencyMaxNanos),
                std::memory_order_relaxed);
            const uint64_t latencyBuckets[] = {
                latencyBucket0, latencyBucket1, latencyBucket2,
                latencyBucket3, latencyBucket4, latencyBucket5,
            };
            for (size_t index = 0; index < std::size(latencyBuckets); ++index) {
                directControl->txCompletedIntervalPreparationLatencyHistogram[index].store(
                    latencyBuckets[index], std::memory_order_relaxed);
            }
            const uint64_t marginBuckets[] = {
                marginBucket0, marginBucket1, marginBucket2,
                marginBucket3, marginBucket4,
            };
            for (size_t index = 0; index < std::size(marginBuckets); ++index) {
                directControl->txCompletedIntervalCommittedMarginHistogram[index].store(
                    marginBuckets[index], std::memory_order_relaxed);
            }
            directControl->txCompletedIntervalSequence.fetch_add(
                1, std::memory_order_release);
            directControl->rxCaptureBufferTelemetry.CompleteInterval();
            // Stamped on every emission, so an anomaly burst defers the next
            // heartbeat instead of interleaving with it. Anomalies are never
            // themselves suppressed.
            directControl->txHeartbeatLastHostTicks.store(
                now, std::memory_order_relaxed);
            ASFW_LOG(
                DirectAudio,
                "[TxPrep] margin=%u iMin=%u iMax=%u min=%u lead=%u "
                "lastLatUs=%llu iMaxLatUs=%llu maxLatUs=%llu "
                "latHist=%llu/%llu/%llu/%llu/%llu/%llu "
                "marginHist=%llu/%llu/%llu/%llu/%llu fast750=%llu "
                "late1500=%llu wakes=%llu "
                "retentionHorizonFrames=%u coverageLead=%u%{public}s",
                boundedMargin,
                intervalMarginMin,
                intervalMarginMax,
                minCommittedMargin,
                ASFW::Audio::Shared::AudioTimingGeometry::
                    kTxPreparationLeadPackets,
                latencyNanos / 1000,
                intervalLatencyMaxNanos / 1000,
                maxLatencyNanos / 1000,
                latencyBucket0,
                latencyBucket1,
                latencyBucket2,
                latencyBucket3,
                latencyBucket4,
                latencyBucket5,
                marginBucket0,
                marginBucket1,
                marginBucket2,
                marginBucket3,
                marginBucket4,
                directControl->txPreparationAtMost750Us.load(
                    std::memory_order_relaxed),
                directControl->txPreparationAtLeast1500Us.load(
                    std::memory_order_relaxed),
                wakeSamples,
                dataHorizonFrames,
                ASFW::Audio::Shared::AudioTimingGeometry::
                    kTxCoverageLeadPackets,
                boundedMargin <= kCommittedMarginDangerPackets ? " DANGER"
                                                               : "");
        }

        directControl->counters.txPreparationWakeRequests.store(
            txControl->refillRequestCount.load(
                std::memory_order_relaxed),
            std::memory_order_relaxed);
        directControl->counters.txPreparationWakeDispatches.fetch_add(
            1, std::memory_order_relaxed);
        directControl->counters.txPreparationWakeCoalesced.store(
            txControl->refillCoalescedCount.load(
                std::memory_order_relaxed),
            std::memory_order_relaxed);
        directControl->counters.txPreparationDrainPasses.fetch_add(
            1, std::memory_order_relaxed);
        const bool audioTargetSatisfied =
            targetFrameEnd == 0 || finalizedFrameEndAfter >= targetFrameEnd;
        if (audioTargetSatisfied) {
            directControl->txPreparationRequests.MarkHandled(
                audioRequested, now);
        }
        directControl->txPreparationRequests.FinishWake();
        // A CoreAudio callback can publish while this action is preparing
        // slots. It saw wakeScheduled=true and deliberately did not enqueue a
        // second action; hand it one now after draining the latest target.
        scheduleAudioFollowUp = audioTargetSatisfied &&
            directControl->txPreparationRequests.NeedsHandling() &&
            directControl->txPreparationRequests.TryScheduleWake();
    }

    txControl->MarkRefillHandled(requested);

    if (scheduleAudioFollowUp && ivars->device.audioNub) {
        const kern_return_t requestKr =
            ivars->device.audioNub->RequestTxPreparation(
                directControl->txPreparationRequests.RequestedGeneration());
        if (requestKr != kIOReturnSuccess) {
            directControl->txPreparationRequests.FinishWake();
        }
    }
}
