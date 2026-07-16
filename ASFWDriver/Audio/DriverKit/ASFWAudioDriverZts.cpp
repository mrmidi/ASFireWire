//
// ASFWAudioDriverZts.cpp
// ASFWDriver
//
// Latest observed zero-timestamp publication for ASFWAudioDriver.
//

#include <cstdint>
#include <new>

#include "ASFWAudioDevice.h"
#include "ASFWAudioDriverPrivate.hpp"
#include "../../Common/TimingUtils.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/DriverKit.h>

namespace ASFW::Audio::DriverKit {
namespace {

[[nodiscard]] uint64_t SaturatingAdd(uint64_t value,
                                     uint64_t addend) noexcept {
    return (UINT64_MAX - value < addend) ? UINT64_MAX : value + addend;
}

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

    const auto failProducer =
        [&](ASFW::IsochTransport::TxProducerStage stage,
            ASFW::IsochTransport::TxProducerFailureReason producerReason,
            ASFW::Audio::Runtime::FatalStreamReason runtimeReason,
            uint64_t packetIndex) noexcept {
            auto* txControl =
                ivars.runtime.txSlotProvider.controlBlock;
            const uint64_t completionCursor =
                txControl
                    ? txControl->completionCursor.load(
                          std::memory_order_acquire)
                    : 0;
            const uint64_t exposeCursor =
                txControl
                    ? txControl->exposeCursor.load(
                          std::memory_order_acquire)
                    : 0;

            ASFW::IsochTransport::TxProducerFailureRecord failure{
                .stage = stage,
                .reason = producerReason,
                .packetIndex = packetIndex,
                .rangeStart = startPacketIndex,
                .rangeTarget = limitPacketIndex,
                .preparedCount = preparedCount,
                .completionCursor = completionCursor,
                .exposeCursor = exposeCursor,
                .replayProducerCursor =
                    directControl->rxSequenceReplay.ProducerCursor(),
                .replayEpoch =
                    directControl->rxSequenceReplay.Epoch(),
            };
            const uint64_t producerGeneration =
                txControl
                    ? txControl->producerFailure.Publish(failure)
                    : 0;

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
                "completion=%llu expose=%llu replayProducer=%llu "
                "replayEpoch=%u",
                ASFW::IsochTransport::TxProducerStageName(stage),
                ASFW::IsochTransport::TxProducerFailureReasonName(
                    producerReason),
                producerGeneration,
                static_cast<uint32_t>(runtimeReason),
                runtimeGeneration,
                packetIndex,
                startPacketIndex,
                limitPacketIndex,
                preparedCount,
                completionCursor,
                exposeCursor,
                failure.replayProducerCursor,
                failure.replayEpoch);

            if (txControl) {
                txControl->statusWord.store(
                    ASFW::IsochTransport::TxStreamStatus::
                        kUnderrunFatal,
                    std::memory_order_release);
            }
            ivars.runtime.txActive.store(
                false, std::memory_order_release);
        };

    if (numSlots == 0 || metadataRing == nullptr ||
        ivars.runtime.txSlotProvider.controlBlock == nullptr) {
        failProducer(
            ASFW::IsochTransport::TxProducerStage::kPreflight,
            ASFW::IsochTransport::TxProducerFailureReason::
                kInvalidTransport,
            ASFW::Audio::Runtime::FatalStreamReason::
                InvalidGeometry,
            startPacketIndex);
        return 0;
    }

    auto frameTargetSatisfied = [&]() noexcept {
        return targetFrameEnd == 0 ||
               ivars.runtime.txStreamEngine.Timeline().ExposedFrameEnd() >=
                   targetFrameEnd;
    };

    while (nextPacketToPrepare < limitPacketIndex &&
           preparedCount < maxToPrepare) {
        if (nextPacketToPrepare >= requiredPacketIndex &&
            frameTargetSatisfied()) {
            break;
        }

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
                failProducer(
                    ASFW::IsochTransport::TxProducerStage::
                        kExecutionAnchor,
                    ASFW::IsochTransport::TxProducerFailureReason::
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
            // a coordinator restart cannot re-prime TX. Instead, re-sync the reader
            // to RX's current epoch and ship a NO-DATA (silence) packet for this
            // slot; DATA replay resumes as soon as RX republishes kReadDelay
            // entries. Persistent unavailability degrades to silence, which is the
            // correct "nothing to send yet" state, not a stream death.
            if (!ivars.runtime.txReplayReader.IsActive()) {
                (void)ivars.runtime.txReplayReader.Begin(
                    directControl->rxSequenceReplay);
            }

            ASFW::Audio::Runtime::RxSequenceEntry replay{};
            if (ivars.runtime.txReplayReader.IsActive() &&
                ivars.runtime.txReplayReader.TryRead(
                    directControl->rxSequenceReplay, replay)) {
                directControl->txReplayEntries.fetch_add(
                    1, std::memory_order_relaxed);
                timing.replayDataBlocks = replay.dataBlocks;
            } else {
                // Reader stale (epoch moved) or ahead of the producer: count it,
                // drop the reader so the next packet re-Begins on the live epoch,
                // and fall through with the NO-DATA disposition set above. Also
                // re-arm the frame-cursor alignment: while stalled we emit NO-DATA
                // packets, which do NOT advance the content-frame cursor, so it
                // freezes at its pre-stall frame while CoreAudio keeps writing. If
                // the stall outlasts one playback ring the cursor is stranded on
                // overwritten (silent) frames forever. Re-arming makes the first
                // DATA packet after replay recovers re-project the cursor to the
                // live frame, closing the gap. Only reached on a genuine replay
                // stall (churn), never during normal cadence NO-DATA.
                directControl->txReplayUnderflows.fetch_add(
                    1, std::memory_order_relaxed);
                ivars.runtime.txReplayReader.Reset();
                ivars.runtime.txStreamEngine.ReArmFrameCursorAlignment();
                if (ivars.runtime.txSecondaryActive) {
                    ivars.runtime.txStreamEngineSecondary
                        .ReArmFrameCursorAlignment();
                }
                timing.replayDataBlocks = 0;
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
                        ASFW::IsochTransport::TxProducerStage::
                            kReplaySytValidation,
                        ASFW::IsochTransport::
                            TxProducerFailureReason::
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
                    const uint64_t alignedFrame =
                        (projectedFrame / kFramesPerPacket) *
                        kFramesPerPacket;
                    const bool aligned =
                        ivars.runtime.txStreamEngine
                            .AlignFrameCursorOnce(alignedFrame);
                    if (ivars.runtime.txSecondaryActive) {
                        (void)ivars.runtime.txStreamEngineSecondary
                            .AlignFrameCursorOnce(alignedFrame);
                    }
                    // Fires once at stream start, then again each time replay
                    // recovers after a stall re-armed the cursor. A 2nd+ line is
                    // the self-heal closing a deficit that would otherwise be
                    // permanent silence; anomaly-only, so a clean run prints one.
                    if (aligned) {
                        ASFW_LOG(DirectAudio,
                                 "[TxAlign] frame cursor -> %llu (projected=%llu "
                                 "rxFirstFrame=%llu deltaTicks=%lld rate=%u)",
                                 alignedFrame,
                                 projectedFrame,
                                 replay.firstAudioFrame,
                                 static_cast<long long>(presentationDeltaTicks),
                                 txConfig.sampleRate);
                    }
                }
            }
        }

        const auto prepareResult =
            ivars.runtime.txStreamEngine.PrepareNextTransmitSlot(
                static_cast<uint32_t>(nextPacketToPrepare),
                timing);
        if (prepareResult !=
            ASFW::Protocols::Audio::DICE::TxSlotPrepareResult::
                kPrepared) {
            ASFW::IsochTransport::TxProducerStage stage =
                ASFW::IsochTransport::TxProducerStage::kSlotAcquire;
            ASFW::IsochTransport::TxProducerFailureReason producerReason =
                ASFW::IsochTransport::TxProducerFailureReason::
                    kSlotUnavailable;
            ASFW::Audio::Runtime::FatalStreamReason runtimeReason =
                ASFW::Audio::Runtime::FatalStreamReason::
                    TxSlotInvariant;

            switch (prepareResult) {
                case ASFW::Protocols::Audio::DICE::
                    TxSlotPrepareResult::kPacketizerRejected:
                    stage =
                        ASFW::IsochTransport::TxProducerStage::
                            kPacketize;
                    producerReason =
                        ASFW::IsochTransport::
                            TxProducerFailureReason::
                                kPacketizerRejected;
                    runtimeReason =
                        ASFW::Audio::Runtime::FatalStreamReason::
                            InvalidGeometry;
                    break;
                case ASFW::Protocols::Audio::DICE::
                    TxSlotPrepareResult::kSlotPublishFailed:
                    stage =
                        ASFW::IsochTransport::TxProducerStage::
                            kSlotPublish;
                    producerReason =
                        ASFW::IsochTransport::
                            TxProducerFailureReason::
                                kSlotPublishFailed;
                    break;
                case ASFW::Protocols::Audio::DICE::
                    TxSlotPrepareResult::kSlotProviderUnavailable:
                case ASFW::Protocols::Audio::DICE::
                    TxSlotPrepareResult::kSlotAcquireFailed:
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
        // disposition), differing only in payload (channels 17–32). Best-effort:
        // a secondary hiccup must never stall the master (channels 1–16).
        if (ivars.runtime.txSecondaryActive) {
            (void)ivars.runtime.txStreamEngineSecondary.PrepareNextTransmitSlot(
                static_cast<uint32_t>(nextPacketToPrepare), timing);
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
             ASFW::IsochTransport::AudioTimingGeometry::
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
    const uint64_t packetCoverageTarget =
        completionCursor +
        ASFW::IsochTransport::AudioTimingGeometry::
            kTxCoverageLeadPackets;
    const uint64_t packetLimitTarget =
        completionCursor +
        ASFW::IsochTransport::AudioTimingGeometry::
            kTxPreparationLeadPackets;

    auto* directControl = ivars->runtime.directAudioGraph.control;
    const bool replayEstablished =
        directControl && directControl->rxSequenceReplay.IsEstablished();
    const uint64_t outputWrittenEndFrame =
        directControl ? directControl->client.OutputWrittenEndFrame() : 0;
    const uint64_t targetFrameEnd =
        outputWrittenEndFrame != 0
            ? ASFW::Audio::DriverKit::SaturatingAdd(
                  outputWrittenEndFrame,
                  ASFW::IsochTransport::AudioTimingGeometry::
                      kTxExposureLeadFrames)
            : 0;
    const uint64_t exposedFrameEndBefore =
        ivars->runtime.txStreamEngine.Timeline().ExposedFrameEnd();
    const uint32_t slotsPrepared =
        ASFW::Audio::DriverKit::PrepareTransmitSlots(
            *ivars,
            exposeCursor,
            packetCoverageTarget,
            packetLimitTarget,
            ASFW::IsochTransport::AudioTimingGeometry::
                kTxPreparationLeadPackets,
            targetFrameEnd,
            replayEstablished);
    const uint64_t exposedFrameEndAfter =
        ivars->runtime.txStreamEngine.Timeline().ExposedFrameEnd();

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
            packetLimitTarget > prepareBaseAbs
                ? packetLimitTarget - prepareBaseAbs
                : 0;
        const bool stoppedShort = prepareUntilAbs < packetCoverageTarget;
        const bool frameShort =
            targetFrameEnd != 0 && exposedFrameEndAfter < targetFrameEnd;
        const uint64_t firstMissingAbs =
            stoppedShort ? prepareUntilAbs : UINT64_MAX;
        const uint64_t committedMargin =
            prepareUntilAbs > completionCursor
                ? prepareUntilAbs - completionCursor
                : 0;
        // Basic TX flow is confirmed (Defect B closed, tag
        // tx-frame-exposure-lead). Anomaly-only: log only a wake that stopped
        // short of the coverage target (hole-producing -- precedes an IT FATAL,
        // proves the underrun is refill-coverage not scheduling margin) or one
        // that under-exposed the frame timeline (frameShort, W > E). The steady
        // "nothing to prepare" wake (slotsPrepared == 0, ring already full) is
        // the normal state and no longer logged; the periodic [TxPrep] summary
        // remains the liveness/margin heartbeat.
        if (stoppedShort || frameShort) {
            const uint64_t frameDeficit =
                frameShort ? (targetFrameEnd - exposedFrameEndAfter) : 0;
            ASFW_LOG(
                DirectAudio,
                "[TxPrepRange] retiredAbs=%llu prepareBaseAbs=%llu "
                "prepareUntilAbs=%llu coverageTargetAbs=%llu limitTargetAbs=%llu reqSpan=%llu "
                "prepared=%u short=%d firstMissingAbs=%lld marginAfter=%llu "
                "frameTarget=%llu exposedBefore=%llu exposedAfter=%llu "
                "frameShort=%d frameDeficit=%llu writeEnd=%llu replay=%d",
                completionCursor,
                prepareBaseAbs,
                prepareUntilAbs,
                packetCoverageTarget,
                packetLimitTarget,
                requestedSpan,
                slotsPrepared,
                stoppedShort ? 1 : 0,
                stoppedShort ? static_cast<int64_t>(firstMissingAbs)
                             : static_cast<int64_t>(-1),
                committedMargin,
                targetFrameEnd,
                exposedFrameEndBefore,
                exposedFrameEndAfter,
                frameShort ? 1 : 0,
                frameDeficit,
                outputWrittenEndFrame,
                replayEstablished ? 1 : 0);
        }
    }

    if (directControl) {
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
            packetLimitTarget > exposeCursor
                ? packetLimitTarget - exposeCursor
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
            ASFW::IsochTransport::AudioTimingGeometry::kTxHardwareRingPackets;
        const bool newCommittedMarginLow =
            boundedMargin < committedMarginFloorBefore;
        const bool slackBudgetExceeded = latencyNanos >= 1500000;
        if (newCommittedMarginLow || slackBudgetExceeded ||
            (wakeSamples % 1024) == 0) {
            ASFW_LOG(
                DirectAudio,
                "[TxPrep] margin=%u min=%u lead=%u distMin=%u lastLatUs=%llu "
                "maxLatUs=%llu late1500=%llu wakes=%llu exposureLead=%u "
                "coverageLead=%u%{public}s",
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
                ASFW::IsochTransport::AudioTimingGeometry::
                    kTxExposureLeadFrames,
                ASFW::IsochTransport::AudioTimingGeometry::
                    kTxCoverageLeadPackets,
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
