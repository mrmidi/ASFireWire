//
// ASFWAudioDriverTxProducer.cpp
// ASFWDriver
//
// The TX producer: packet preparation (PrepareTransmitSlots), the start
// prefill, arming the primary producer, and the TX preparation wake. Split
// from ASFWAudioDriverZts.cpp (Epic 4, T6); convergence milestone 6 (FW-209)
// redesigns this side.
//

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <new>
#include <array>
#include <span>

#include "ASFWAudioDevice.h"
#include "ASFWAudioDriverPrivate.hpp"
#include "../../Common/TimingUtils.hpp"
#include "../../Logging/Logging.hpp"
#include "../Wire/IEC61883/Syt.hpp"
#include "../Families/BeBoB/MAudio/MAudioClockSourcePolicy.hpp"

#include <DriverKit/DriverKit.h>

// A producer stall up to the preparation slack is survivable by the ring; the
// RX replay history must outlast it too, or the reader re-anchors and the lead
// cannot recover (TX_OWNERSHIP.md, T5b).
static_assert(ASFW::Audio::Runtime::RxSequenceReplayState::kCapacity -
                      ASFW::Audio::Runtime::RxSequenceReplayState::kReadDelay >=
                  ASFW::IsochTransport::AudioTimingGeometry::kTxPreparationSlackPackets,
              "RX replay history must outlast a producer stall the IT ring survives");

namespace ASFW::Audio::DriverKit {

// The TX frame cursor's start: the RX packet's first frame plus the
// presentation distance to the TX packet, in frames at the live rate. 44.1k
// has no integer ticks per sample, so the tick*rate product is divided.
// It is NOT rounded to a packet boundary: rounding down made every output
// frame play 0-7 frames late, chosen by each start's phase (measured on the
// Pro 24 DSP: RTL_ts = 105.03 + the rounding, TX_OWNERSHIP.md §1g). Nothing
// downstream needs packet-aligned frames.
uint64_t ProjectTxFrameCursor(uint64_t rxFirstFrame,
                              uint64_t presentationDeltaTicks,
                              uint32_t sampleRate) noexcept {
    return rxFirstFrame +
           (presentationDeltaTicks * sampleRate) / ASFW::Timing::kTicksPerSecond;
}

uint32_t PrepareTransmitSlots(ASFWAudioDriver_IVars& ivars,
                             uint64_t startPacketIndex,
                             uint64_t requiredPacketIndex,
                             uint64_t limitPacketIndex,
                             uint32_t maxToPrepare,
                             bool allowRecoveredClock) noexcept {
    const uint32_t numSlots = ivars.runtime.txSlotProvider.numSlots;
    auto* metadataRing = ivars.runtime.txSlotProvider.metadataRing;
    auto* directControl = ivars.runtime.directAudioGraph.control;
    if (directControl == nullptr) {
        ivars.runtime.motuTxTimingStamper.BindCache(nullptr);
        return 0;
    }
    // MOTU's per-block SPH offsets are captured on the transport side and replayed here.
    // They live in the shared control block -- the seam both services map -- rather than
    // in the capture consumer, which IsochDuplexHostTransport owns and destroys on stop
    // while this service may still be preparing packets (the FW-60 cross-service class).
    // Rebind on every pass so the stamper's pointer is never older than directControl.
    ivars.runtime.motuTxTimingStamper.BindCache(&directControl->motuEventOffsets);

    uint64_t nextPacketToPrepare = startPacketIndex;
    uint32_t preparedCount = 0;

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
            const uint64_t exposeCursor =
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
                .committedEnd = exposeCursor,
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
                exposeCursor,
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

    // Below this the transport's next refill loads descriptors: a packet here
    // must be committed now, even if its content is not ready.
    const uint64_t mustCommitBefore =
        ivars.runtime.txSlotProvider.queueControl->completionCursor.load(
            std::memory_order_acquire) +
        ASFW::IsochTransport::AudioTimingGeometry::kTxHardwareRingPackets +
        ASFW::IsochTransport::AudioTimingGeometry::kTxPacketsPerGroup;
    const auto requestHeaderlessRecovery = [&]() noexcept {
        const uint32_t lossRun = ivars.runtime.rxReplayLossRun.fetch_add(
            1, std::memory_order_relaxed) + 1;
        auto* current = ivars.runtime.directAudioGraph.control;
        if (lossRun >= 32 && current && ivars.device.audioNub &&
            ivars.runtime.txActive.load(std::memory_order_acquire) &&
            !ivars.runtime.rxReplayRecoveryRequested.exchange(
                true, std::memory_order_acq_rel)) {
            ivars.device.audioNub->RequestTimingRecovery(
                current->rxReplayEpochResets.load(std::memory_order_acquire));
        }
    };

    while (nextPacketToPrepare < limitPacketIndex &&
           preparedCount < maxToPrepare) {
        if (nextPacketToPrepare >= requiredPacketIndex) {
            break;
        }
        // A pass can prepare a whole coverage lead (1008 packets); stop as
        // soon as StopIO has cleared txActive instead of finishing it.
        if (!ivars.runtime.txActive.load(std::memory_order_acquire)) {
            break;
        }

        ASFW::Protocols::Audio::AMDTP::AmdtpTimingState timing{};
        timing.disposition =
            ASFW::Protocols::Audio::AMDTP::AmdtpPacketDisposition::NoData;
        const bool bootstrapCadence =
            ivars.runtime.rxReplayAfterBootstrap.load(std::memory_order_acquire) &&
            !allowRecoveredClock;
        // Start with configured cadence; after RX history establishes, the
        // next writable index is the immutable transition boundary.
        timing.replayValid = !bootstrapCadence;
        if (bootstrapCadence) {
            timing.disposition =
                ASFW::Protocols::Audio::AMDTP::AmdtpPacketDisposition::Data;
        }

        ASFW::Audio::BeBoB::MAudioInternalTxTiming::PacketPlan mAudioPlan{};
        if (ivars.runtime.mAudioInternalTxActive.load(std::memory_order_acquire)) {
            if (!ivars.runtime.mAudioInternalTxTiming.PreviewNextPacket(mAudioPlan) ||
                mAudioPlan.sequence != nextPacketToPrepare) {
                failProducer(
                    ASFW::Audio::Runtime::TxProducerFaultStage::kInternalCadence,
                    ASFW::Audio::Runtime::TxProducerFaultReason::kCadencePlanMismatch,
                    ASFW::Audio::Runtime::FatalStreamReason::TxReplayInvalidSyt,
                    nextPacketToPrepare);
                break;
            }
            int64_t packetAnchorTicks = 0;
            if (mAudioPlan.isData &&
                ivars.runtime.txExecutionTimeline.AnchorForPacket(
                    nextPacketToPrepare, packetAnchorTicks)) {
                const uint32_t transmitCycle = static_cast<uint32_t>(
                    (ASFW::Timing::normalizeOffsetDomain(packetAnchorTicks) /
                     ASFW::Timing::kTicksPerCycle) %
                    ASFW::Timing::kCyclesPerSecond);
                timing.replayDataBlocks = mAudioPlan.dataBlocks;
                timing.disposition =
                    ASFW::Protocols::Audio::AMDTP::AmdtpPacketDisposition::Data;
                timing.txClockValid = true;
                timing.nextDataSyt = ASFW::Audio::BeBoB::MAudioInternalTxSyt(
                    mAudioPlan.sytOffsetTicks, transmitCycle,
                    ivars.runtime.mAudioInternalTxTiming.TransferDelayTicks());
            }
        } else if (allowRecoveredClock) {
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
            // The cycle this packet goes out in, derived exactly as the SYT trace below
            // derives outCycle. MOTU stamps each block's SPH relative to it.
            timing.transmitCycle = static_cast<uint32_t>(
                (ASFW::Timing::normalizeOffsetDomain(packetAnchorTicks) /
                 ASFW::Timing::kTicksPerCycle) %
                ASFW::Timing::kCyclesPerSecond);
            timing.transmitCycleValid = true;

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
            const bool headerlessReplayMode =
                ivars.runtime.rxReplayAfterBootstrap.load(std::memory_order_acquire) &&
                ivars.runtime.txStreamEngine.StreamConfig().packetFraming ==
                    ASFW::Protocols::Audio::AMDTP::AmdtpStreamConfig::
                        PacketFraming::Headerless;

            ASFW::Audio::Runtime::RxSequenceEntry replay{};
            ASFW::Audio::Runtime::RxSequenceReplayReadDiagnostic replayDiagnostic{};
            bool replayReadable = ivars.runtime.txReplayReader.TryRead(
                directControl->rxSequenceReplay, replay,
                &replayDiagnostic);
            // A reader that fell out of the bounded RX history (kCapacity) is
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
                        kHistoryOverwritten && !headerlessReplayMode) {
                if (ivars.runtime.txReplayReader.Begin(
                        directControl->rxSequenceReplay)) {
                    replayReadable = ivars.runtime.txReplayReader.TryRead(
                        directControl->rxSequenceReplay, replay,
                        &replayDiagnostic);
                }
                ASFW_LOG_RING_ONLY_RL(
                    DirectAudio,
                    "tx-replay-reclamp",
                    1000u,
                    ::ASFW::Logging::LogLevel::Warning,
                    "[TxReplay] reclamped pkt=%llu cur=%llu prod=%llu ok=%u",
                    nextPacketToPrepare,
                    replayDiagnostic.readerCursor,
                    replayDiagnostic.producerCursor,
                    replayReadable ? 1u : 0u);
            }
            if (replayReadable) {
                ivars.runtime.rxReplayLossRun.store(0, std::memory_order_relaxed);
                directControl->txReplayEntries.fetch_add(
                    1, std::memory_order_relaxed);
                timing.replayDataBlocks = replay.dataBlocks;
            } else if (replayDiagnostic.failure ==
                           ASFW::Audio::Runtime::RxSequenceReplayReadFailure::
                               kAheadOfProducer &&
                       nextPacketToPrepare >= mustCommitBefore) {
                if (headerlessReplayMode) {
                    requestHeaderlessRecovery();
                }
                // Existing CIP replay must stop here: NO-DATA would move its
                // SYT/frame mapping. Headerless RME has no presentation field
                // to corrupt, so bounded skip cycles keep OHCI committed while
                // recovery is queued.
                if (!headerlessReplayMode) {
                    break;
                }
            } else {
                if (headerlessReplayMode) {
                    requestHeaderlessRecovery();
                }
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
                    // Ahead of RX inside the descriptor floor: the hardware
                    // would reach an uncommitted slot, so this one NO-DATA is
                    // forced. It costs one cycle of lag; counted and logged.
                    directControl->txReplayForcedNoData.fetch_add(
                        1, std::memory_order_relaxed);
                    ASFW_LOG_RING_ONLY_RL(
                        DirectAudio,
                        "tx-replay-forced",
                        1000u,
                        ::ASFW::Logging::LogLevel::Warning,
                        "[TxReplay] forced NO-DATA pkt=%llu mustCommitBefore=%llu cur=%llu prod=%llu",
                        nextPacketToPrepare,
                        mustCommitBefore,
                        replayDiagnostic.readerCursor,
                        replayDiagnostic.producerCursor);
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
                    if (!headerlessReplayMode) {
                        ivars.runtime.txReplayReader.Reset();
                        ivars.runtime.txStreamEngine.ReArmFrameCursorAlignment();
                        if (ivars.runtime.txSecondaryActive) {
                            ivars.runtime.txStreamEngineSecondary
                                .ReArmFrameCursorAlignment();
                        }
                    }
                }
            }

            if (replay.dataBlocks != 0) {
                const bool headerlessReplay =
                    ivars.runtime.rxReplayAfterBootstrap.load(std::memory_order_acquire) &&
                    ivars.runtime.txStreamEngine.StreamConfig().packetFraming ==
                        ASFW::Protocols::Audio::AMDTP::AmdtpStreamConfig::
                            PacketFraming::Headerless;
                // A replayed entry with data blocks but no SYT offset is corruption for
                // an SYT-aware family -- but it is the normal case for MOTU, whose capture
                // side correctly never sets kValidSyt. Linux treats it as an ordinary DATA
                // packet: the SYT goes out as CIP_SYT_NO_INFO and data_blocks is replayed
                // regardless (amdtp-stream.c:1033-1037; the capture cache stores
                // CIP_SYT_NO_INFO for these at :518-521). Faulting here killed transmit
                // 22 ms after StartIO succeeded, so the device started but stayed silent.
                const bool hasReplaySyt =
                    replay.sytOffset !=
                        ASFW::Audio::Runtime::RxSequenceReplayState::kNoInfo &&
                    (replay.flags &
                     ASFW::Audio::Runtime::RxSequenceFlags::kValidSyt) != 0;
                const bool sytUnaware = headerlessReplay ||
                    ivars.runtime.txStreamEngine.IsSytUnaware();
                // kNoInfo is UINT32_MAX; the presentation arithmetic below must never see
                // it. For an SYT-unaware family the packet's cycle timer is the best
                // anchor available, so contribute no sub-cycle offset -- at most one cycle
                // of error, which the framesPerDataPacket alignment absorbs, while the
                // per-block SPH carries the exact timing.
                const uint32_t replaySytOffset = hasReplaySyt ? replay.sytOffset : 0U;
                if (!hasReplaySyt && !sytUnaware) {
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
                    hasReplaySyt
                        ? ASFW::Audio::Runtime::ComputeReplaySytFromTicks(
                              replay.sytOffset, packetAnchorTicks, txDelay)
                        : ASFW::Protocols::Audio::IEC61883::SytFormatter::kNoInfo;

                // [TxSyt]: the stream's first replayed SYT decision, once per
                // stream: the device's SYT (reconstructed from the replayed
                // delay-free offset against its source cycle), that offset,
                // and the transmit SYT re-anchored to our cycle.
                if (hasReplaySyt && !ivars.runtime.txSytLogged) {
                    ivars.runtime.txSytLogged = true;
                    const uint32_t sourceCycle =
                        ASFW::Timing::decodeCycleTimer(replay.sourceCycleTimer).cycle;
                    const auto outCycle = static_cast<uint32_t>(
                        (ASFW::Timing::normalizeOffsetDomain(packetAnchorTicks) /
                         ASFW::Timing::kTicksPerCycle) %
                        ASFW::Timing::kCyclesPerSecond);
                    const auto observedRxSyt = static_cast<uint16_t>(
                        ASFW::Audio::Runtime::ComputeReplaySyt(
                            replay.sytOffset, replay.sourceCycleTimer,
                            directControl->rxTransferDelayTicks.load(
                                std::memory_order_relaxed)));
                    const auto txSyt = static_cast<uint16_t>(timing.nextDataSyt);
                    ASFW_LOG(TxSyt,
                             "obsCyc=%u rxSyt=0x%04x sytOffDelayFree=%u +txDelay=%u outCyc=%u "
                             "=> txSyt=0x%04x (cyc=%u off=0x%03x) pkt=%llu rate=%u rxDbc=%u rxBlocks=%u",
                             sourceCycle, observedRxSyt, replay.sytOffset, txDelay, outCycle,
                             txSyt, (static_cast<uint32_t>(txSyt) >> 12) & 0x0fu,
                             static_cast<uint32_t>(txSyt) & 0x0fffu, nextPacketToPrepare,
                             ivars.runtime.txStreamEngine.StreamConfig().sampleRate,
                             replay.dbc, replay.dataBlocks);
                }

                if (!headerlessReplay) {
                const int64_t sourcePresentationTicks =
                    ASFW::Timing::normalizeOffsetDomain(
                        ASFW::Timing::encodedTstampToOffsets(
                            replay.sourceCycleTimer) +
                        replaySytOffset +
                        directControl
                            ->rxTransferDelayTicks.load(
                                std::memory_order_relaxed));
                const int64_t outputPresentationTicks =
                    ASFW::Timing::normalizeOffsetDomain(
                        packetAnchorTicks +
                        replaySytOffset +
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
                    const uint64_t projectedFrame =
                        ASFW::Audio::DriverKit::ProjectTxFrameCursor(
                            replay.firstAudioFrame,
                            static_cast<uint64_t>(presentationDeltaTicks),
                            txConfig.sampleRate);
                    const uint64_t alignedFrame = projectedFrame;
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
        }

        const auto prepareResult =
            ivars.runtime.txStreamEngine.PrepareNextTransmitSlot(
                nextPacketToPrepare,
                timing);
        if (prepareResult !=
            ASFW::Protocols::Audio::DICE::TxSlotPrepareResult::
                kPrepared) {
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
                nextPacketToPrepare, timing);
        }

        const uint32_t slotIdx =
            static_cast<uint32_t>(
                nextPacketToPrepare % numSlots);
        const auto& meta = metadataRing[slotIdx];
        // Ask the packetizer what it emitted; packet size is not a DATA test.
        // M-Audio cadence NO-DATA packets are full-size (CF-labelled blocks).
        const auto* preparedSlot =
            ivars.runtime.txStreamEngine.Timeline().SlotByIndex(
                nextPacketToPrepare);
        const bool emittedData = preparedSlot != nullptr && preparedSlot->isData;
        if (ivars.runtime.mAudioInternalTxActive.load(std::memory_order_acquire) &&
            !ivars.runtime.mAudioInternalTxTiming.CommitPacket(
                mAudioPlan, emittedData)) {
            failProducer(
                ASFW::Audio::Runtime::TxProducerFaultStage::kInternalCadence,
                ASFW::Audio::Runtime::TxProducerFaultReason::kCadenceCommitRejected,
                ASFW::Audio::Runtime::FatalStreamReason::TxReplayInvalidSyt,
                nextPacketToPrepare);
            break;
        }
        const bool headerless = ivars.runtime.txStreamEngine.StreamConfig().packetFraming ==
            ASFW::Protocols::Audio::AMDTP::AmdtpStreamConfig::PacketFraming::Headerless;
        if (emittedData) {
            directControl->counters.txDataPackets.fetch_add(
                1, std::memory_order_relaxed);
            if (!headerless) {
                directControl->counters.txValidSytPackets.fetch_add(
                    1, std::memory_order_relaxed);
            }
        } else if (!headerless && meta.payloadLength == 0) {
            directControl->counters.txEmptyPackets.fetch_add(
                1, std::memory_order_relaxed);
        } else {
            directControl->counters.txNoDataPackets.fetch_add(
                1, std::memory_order_relaxed);
            if (!headerless) {
                directControl->counters.txSytFfffPackets.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
        directControl->counters.txPackets.fetch_add(
            1, std::memory_order_relaxed);

        ++nextPacketToPrepare;
        ++preparedCount;
        if (ivars.runtime.rxReplayAfterBootstrap.load(std::memory_order_acquire) &&
            ivars.runtime.txStreamEngine.StreamConfig().packetFraming ==
                ASFW::Protocols::Audio::AMDTP::AmdtpStreamConfig::
                    PacketFraming::Headerless &&
            ivars.runtime.rxReplayRecoveryRequested.load(std::memory_order_acquire) &&
            preparedCount >= 32) {
            break;
        }
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
    timing.disposition =
        ASFW::Protocols::Audio::AMDTP::AmdtpPacketDisposition::NoData;
    timing.replayValid = !ivars.runtime.rxReplayAfterBootstrap.load(
        std::memory_order_acquire);
    if (!timing.replayValid) {
        timing.disposition =
            ASFW::Protocols::Audio::AMDTP::AmdtpPacketDisposition::Data;
    }
    timing.txClockValid = false;

    uint32_t prepared = 0;
    for (uint64_t packetIndex = 0;
         packetIndex < numSlots;
         ++packetIndex) {
        ASFW::Audio::BeBoB::MAudioInternalTxTiming::PacketPlan mAudioPlan{};
        if (ivars.runtime.mAudioInternalTxActive.load(std::memory_order_acquire) &&
            (!ivars.runtime.mAudioInternalTxTiming.PreviewNextPacket(mAudioPlan) ||
             mAudioPlan.sequence != packetIndex)) {
            break;
        }
        if (ivars.runtime.txStreamEngine.PrepareNextTransmitSlot(
                packetIndex, timing) !=
            ASFW::Protocols::Audio::DICE::TxSlotPrepareResult::
                kPrepared) {
            break;
        }
        // Seed the secondary ring in lockstep with the same NO-DATA packets.
        if (ivars.runtime.txSecondaryActive) {
            (void)ivars.runtime.txStreamEngineSecondary.PrepareNextTransmitSlot(
                packetIndex, timing);
        }
        if (ivars.runtime.mAudioInternalTxActive.load(std::memory_order_acquire) &&
            !ivars.runtime.mAudioInternalTxTiming.CommitPacket(mAudioPlan, false)) {
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

PrimaryTxArmResult ArmPrimaryTxProducer(
    ASFWAudioDriver_IVars& ivars,
    const ASFW::Isoch::Audio::IAudioStreamProfile& profile,
    const ASFW::Isoch::Audio::AudioStreamConfig& txConfig,
    const PrimaryTxQueueMemory& memory) noexcept {
    auto* control = ivars.runtime.directAudioGraph.control;
    if (control == nullptr || memory.queueControl == nullptr) {
        return {kIOReturnNotReady, "BindPrimaryTxQueue"};
    }

    // Clear stale runtime cursors before prefill: the shared slab can be
    // reused across StartIO/StopIO probes (CoreAudio re-probes on a
    // sample-rate change), and a carried-over committed cursor fails the IT
    // prime ("committed prefill > slots").
    memory.queueControl->ResetProducerForStart();

    ivars.runtime.txSlotProvider.payloadBase = memory.payloadBase;
    ivars.runtime.txSlotProvider.metadataRing = memory.metadataRing;
    ivars.runtime.txSlotProvider.queueControl = memory.queueControl;
    ivars.runtime.txSlotProvider.numSlots = memory.numSlots;
    ivars.runtime.txSlotProvider.slotStrideBytes = memory.slotStrideBytes;

    ivars.runtime.txExecutionTimeline.queueControl = memory.queueControl;

    // A device's own slot order (the Phase 88 is planar) arrives in txConfig
    // from its discovered graph; Configure prefers it over the profile's map.
    if (!ivars.runtime.txStreamEngine.Configure(profile, txConfig)) {
        ASFW_LOG(Audio, "ASFWAudioDevice: txStreamEngine Configure failed");
        return {kIOReturnError, "ConfigureTxStreamEngine"};
    }
    ivars.runtime.txStreamEngine.SetTimingLossCallback({});
    const auto txPolicy = profile.TxStreamPolicy();
    if (txPolicy.hostToDevicePcmEncoding == ASFW::Encoding::AudioWireFormat::kMotuPacked) {
        ivars.runtime.motuPayloadWriter.Configure(
            ::ASFW::Encoding::Motu::MotuPayloadStreamConfig{
                .pcmChunks = txConfig.pcmChannels,
                .sourceChannelOffset = txConfig.sourceChannelOffset,
                .ports = txPolicy.motuPlaybackPorts,
                .pcmByteOffset = txConfig.motuPcmByteOffset});
        ivars.runtime.motuPayloadWriter.BindTimeline(&ivars.runtime.txStreamEngine.Timeline());
        ivars.runtime.txStreamEngine.SetPayloadWriter(&ivars.runtime.motuPayloadWriter);

        ivars.runtime.txStreamEngine.SetTimingLossCallback([state = &ivars] {
            if (!state->runtime.isRunning.load(std::memory_order_acquire)) return false;
            auto* currentControl = state->runtime.directAudioGraph.control;
            auto* nub = state->device.audioNub;
            if (!nub || !currentControl) return false;
            nub->RequestTimingRecovery(
                currentControl->rxReplayEpochResets.load(std::memory_order_acquire));
            return true;
        });
        ivars.runtime.motuTxTimingStamper.Configure(txConfig.dbs, txConfig.sampleRate);
        ivars.runtime.txStreamEngine.BindTimingStamper(&ivars.runtime.motuTxTimingStamper);
    }
    ivars.runtime.txStreamEngine.BindSlotProvider(&ivars.runtime.txSlotProvider);
    ivars.runtime.txStreamEngine.ResetForStart(0, 0);
    ivars.runtime.txPlacementLogged = false;
    ivars.runtime.txSytLogged = false;
    ivars.runtime.txFilledFrameEnd = 0;
    // The interval minimum belongs to one stream: drop the previous one's.
    (void)ivars.runtime.txStreamEngine.TakeMinFinalityMarginPackets();
    ivars.runtime.txMissedFinalityAtStart =
        ivars.runtime.txStreamEngine.PayloadWriterCounters().framesMissedFinality.load(
            std::memory_order_relaxed) +
        ivars.runtime.motuPayloadWriter.Counters().framesMissedFinality.load(
            std::memory_order_relaxed);
    ivars.runtime.txReplayReader.Reset();
    ivars.runtime.rxReplayLossRun.store(0, std::memory_order_relaxed);
    ivars.runtime.rxReplayRecoveryRequested.store(false, std::memory_order_relaxed);

    // The resolved geometry is the only rate/timing source here; there is no
    // 48 kHz fallback (a graph that failed to resolve never reaches StartIO).
    const auto& timing = ivars.device.timing;
    const uint32_t timingRateHz = timing.sampleRateHz;
    if (ivars.runtime.mAudioInternalTxActive.load(
            std::memory_order_acquire)) {
        ++ivars.runtime.mAudioTxClockStartEpoch;
        if (ivars.runtime.mAudioTxClockStartEpoch == 0) {
            ++ivars.runtime.mAudioTxClockStartEpoch;
        }
        ivars.runtime.txCompletionStampCursor = 0;
        ivars.runtime.mAudioTxCorrelationUnwrap = {};
        if (!ivars.runtime.mAudioTxClockBridge.Arm(
                control->hardwareTimeline,
                ivars.runtime.mAudioTxClockStartEpoch,
                timingRateHz,
                timing.zeroTimestampPeriodFrames,
                ivars.runtime.mAudioInternalTxTiming.
                    TransferDelayTicks())) {
            return {kIOReturnUnsupported, "MAudioTxClockBridge"};
        }
        // Once per start, the counterpart of the Receive line in
        // SelectTxClockDomain: which clock owns this start's timeline.
        ASFW_LOG(DirectAudio, "[Zts] epoch=%llu source=transmit reason=start-io rate=%u",
                 ivars.runtime.mAudioTxClockBridge.Epoch(), timingRateHz);
    }
    control->rxTransferDelayTicks.store(timing.rxTransferDelayTicks,
                                        std::memory_order_relaxed);
    control->txTransferDelayTicks.store(timing.txTransferDelayTicks,
                                        std::memory_order_relaxed);
    return {};
}

bool MeasureTxPlacement(ASFWAudioDriver_IVars& ivars, TxPlacementSample& out) noexcept {
    const auto* queue = ivars.runtime.txSlotProvider.queueControl;
    const auto* control = ivars.runtime.directAudioGraph.control;
    if (queue == nullptr || control == nullptr) {
        return false;
    }
    const uint64_t stamps = queue->completionStampCount.load(std::memory_order_acquire);
    uint64_t packet = 0;
    uint32_t stamp = 0;
    if (stamps == 0 || !queue->ReadCompletionStamp(stamps - 1, packet, stamp)) {
        return false;
    }

    // This runs on the preparation queue, which also writes the timeline, so
    // the slot is read without the RT-side seqlock.
    const auto* slot =
        ivars.runtime.txStreamEngine.Timeline().SlotByIndex(static_cast<uint32_t>(packet));
    if (slot == nullptr || slot->packetIndex != packet ||
        !slot->isData || slot->framesInPacket == 0) {
        return false;
    }

    // The packet's transmit cycle, as host time, through the transport's
    // cycle-timer/host-time pair from the same refill.
    ASFW::Isoch::IsochTxClockPairSample pair{};
    if (!queue->clockPair.TryRead(pair) || pair.hostTimeMid == 0) {
        return false;
    }
    const int64_t busDelta = ASFW::Timing::extOffsetDiff(
        ASFW::Timing::encodedTstampToOffsets(stamp),
        ASFW::Timing::encodedTstampToOffsets(pair.cycleTimer32));
    const uint64_t busMagnitude =
        static_cast<uint64_t>(busDelta < 0 ? -busDelta : busDelta);
    const uint64_t busHost = ASFW::Timing::nanosToHostTicks(
        busMagnitude * 1'000'000'000ULL / ASFW::Timing::kTicksPerSecond);
    const uint64_t txHost =
        busDelta >= 0 ? pair.hostTimeMid + busHost : pair.hostTimeMid - busHost;

    // The HAL's sample time at that host time, from the one anchor CoreAudio
    // is fed (Epic 4: HostClockAnchor is the ZTS projection).
    ASFW::Audio::Runtime::HostClockAnchorSample anchor{};
    const uint32_t rate = ivars.runtime.txStreamEngine.StreamConfig().sampleRate;
    if (!control->hostClockAnchor.TryReadLatest(0, anchor) || anchor.hostTicks == 0 ||
        rate == 0) {
        return false;
    }
    const bool after = txHost >= anchor.hostTicks;
    const uint64_t hostNanos = ASFW::Timing::hostTicksToNanos(
        after ? txHost - anchor.hostTicks : anchor.hostTicks - txHost);
    const auto frames = static_cast<int64_t>(
        static_cast<__uint128_t>(hostNanos) * rate / 1'000'000'000ULL);

    out.packetIndex = packet;
    out.firstAudioFrame = slot->firstAudioFrame;
    out.halSampleTime = static_cast<int64_t>(anchor.sampleFrame) + (after ? frames : -frames);
    out.offsetFrames = static_cast<int64_t>(slot->firstAudioFrame) - out.halSampleTime;
    return true;
}

} // namespace ASFW::Audio::DriverKit

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

    if (hardwareWakePending && ivars->runtime.mAudioInternalTxActive.load(
                                   std::memory_order_acquire)) {
        ASFW::Audio::DriverKit::ObserveMAudioTxClock(*ivars, requested);
    }

    const uint64_t completionCursor =
        txControl->completionCursor.load(std::memory_order_acquire);
    const uint64_t exposeCursor =
        txControl->committedEnd.load(std::memory_order_acquire);
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
    const uint64_t audioRequested = directControl
        ? directControl->txPreparationRequests.RequestedGeneration()
        : 0;
    const uint64_t preparationBegin = mach_absolute_time();
    const uint64_t hardwareRequestedAt = txControl->refillRequestHostTicks.load(std::memory_order_relaxed);
    const uint64_t queueDelayUs = hardwareWakePending && preparationBegin >= hardwareRequestedAt
        ? ASFW::Timing::hostTicksToNanos(preparationBegin - hardwareRequestedAt) / 1000 : 0;
    if (queueDelayUs >= 5000) {
        ASFW_LOG(DirectAudio, "[TxPrepStall] phase=begin queue=TxPreparation generation=%llu queueDelayUs=%llu completion=%llu committed=%llu clientFrame=%llu clientHost=%llu",
            requested, queueDelayUs, completionCursor, exposeCursor,
            directControl ? directControl->client.outputClientWriteEndFrame.load(std::memory_order_acquire) : 0,
            directControl ? directControl->client.outputWriteEndHostTicks.load(std::memory_order_relaxed) : 0);
    }
    const uint32_t slotsPrepared =
        ASFW::Audio::DriverKit::PrepareTransmitSlots(
            *ivars,
            exposeCursor,
            packetCoverageTarget,
            packetLimitTarget,
            ASFW::IsochTransport::AudioTimingGeometry::
                kTxPreparationLeadPackets,
            replayEstablished);

    const uint64_t preparationUs = ASFW::Timing::hostTicksToNanos(mach_absolute_time() - preparationBegin) / 1000;
    if (queueDelayUs >= 5000 || preparationUs >= 5000) {
        ASFW_LOG(DirectAudio, "[TxPrepStall] phase=end queue=TxPreparation generation=%llu queueDelayUs=%llu preparationUs=%llu prepared=%u clientFrame=%llu clientHost=%llu",
            requested, queueDelayUs, preparationUs, slotsPrepared,
            directControl ? directControl->client.outputClientWriteEndFrame.load(std::memory_order_acquire) : 0,
            directControl ? directControl->client.outputWriteEndHostTicks.load(std::memory_order_relaxed) : 0);
    }

    // [TxPrepRange] Refill-coverage instrumentation. Answers the decisive
    // question: did the producer's range reach `target` this wake, or stop
    // short and leave a hole the IT refill ISR will later trip on? The producer
    // loop is linear in absolute packet index, so `prepareUntil` is exactly
    // `base + slotsPrepared`.
    {
        const uint64_t prepareBaseAbs = exposeCursor;
        const uint64_t prepareUntilAbs = exposeCursor + slotsPrepared;
        // A pass may end short of the coverage target on purpose: it stops
        // when RX replay is not there yet (T4). Only a pass that ends below
        // the descriptor floor leaves a hole the refill will trip on.
        const uint64_t descriptorFloor =
            completionCursor +
            ASFW::IsochTransport::AudioTimingGeometry::kTxHardwareRingPackets +
            ASFW::IsochTransport::AudioTimingGeometry::kTxPacketsPerGroup;
        const bool stoppedShort = prepareUntilAbs < descriptorFloor;
        const uint64_t committedMargin =
            prepareUntilAbs > completionCursor
                ? prepareUntilAbs - completionCursor
                : 0;
        // Anomaly-only: log a wake that left a hole below the descriptor
        // floor (it precedes an IT FATAL). The periodic [TxPrep] summary
        // remains the liveness/margin heartbeat.
        if (stoppedShort) {
            ASFW_LOG_RING_ONLY_RL(
                DirectAudio,
                "tx-prep-range",
                0u,
                ::ASFW::Logging::LogLevel::Warning,
                "[TxPrepRange] short=1 ret=%llu base=%llu until=%llu cov=%llu lim=%llu n=%u margin=%llu",
                completionCursor,
                prepareBaseAbs,
                prepareUntilAbs,
                packetCoverageTarget,
                packetLimitTarget,
                slotsPrepared,
                committedMargin);
        }
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
            using Geometry = ASFW::IsochTransport::AudioTimingGeometry;
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
        using Geometry = ASFW::IsochTransport::AudioTimingGeometry;
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
            ASFW::IsochTransport::AudioTimingGeometry::kTxHardwareRingPackets;
        const bool newCommittedMarginLow =
            boundedMargin < committedMarginFloorBefore;

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

        // A slow wake is counted (late1500), not logged on its own: since T4
        // the transport margin is the coverage target (~16 ms), far above the
        // 1.5 ms wake budget, and the fill's exposure to slow wakes shows up
        // as missedFinality in the heartbeat.
        // A new margin low is an anomaly only near the descriptor floor; the
        // drain from the prefill down to the coverage target at start-up is
        // a sequence of new lows by design.
        const bool nearFloor =
            boundedMargin <= ASFW::IsochTransport::AudioTimingGeometry::kTxHardwareRingPackets +
                                 ASFW::IsochTransport::AudioTimingGeometry::kTxPacketsPerGroup;
        if ((newCommittedMarginLow && nearFloor) || heartbeatDue) {
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
            // The interval opened at the previous emission (0 = unknown:
            // first emission after a reset).
            const uint64_t intervalDurationTicks =
                lastHeartbeatTicks != 0 && now > lastHeartbeatTicks
                    ? now - lastHeartbeatTicks
                    : 0;
            ASFW::Audio::Runtime::SeqlockWriteBegin(
                directControl->txCompletedIntervalSequence);
            directControl->txCompletedIntervalDurationTicks.store(
                intervalDurationTicks, std::memory_order_relaxed);
            directControl->txCompletedIntervalEndHostTicks.store(
                now, std::memory_order_relaxed);
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
            ASFW::Audio::Runtime::SeqlockWriteEnd(
                directControl->txCompletedIntervalSequence);
            // Close the RX interval on the same boundary. If the receive path
            // is closing it concurrently this is skipped; it is retried at the
            // next emission.
            (void)directControl->rxCaptureBufferTelemetry.CompleteInterval(now);
            // Stamped on every emission, so an anomaly burst defers the next
            // heartbeat instead of interleaving with it. Anomalies are never
            // themselves suppressed.
            directControl->txHeartbeatLastHostTicks.store(
                now, std::memory_order_relaxed);
            // Kept under the driver ring's 232-byte message. Fill health and
            // the CoreAudio budget first (TX_OWNERSHIP.md, experiment E1):
            // sOutMinPk = smallest S_out headroom this interval, in packets
            //   ahead of the finality frontier (-1: nothing was filled);
            // sInMinFr = smallest S_in headroom, capture frames already
            //   written past the HAL read end (-1: no reads);
            // sInStarve = capture starvations this interval after start-up;
            // sInStart = reads that starved before the stream's first complete
            //   read (the capture ring still filling), for the whole stream.
            const uint64_t missedNow =
                ivars->runtime.txStreamEngine.PayloadWriterCounters()
                    .framesMissedFinality.load(std::memory_order_relaxed) +
                ivars->runtime.motuPayloadWriter.Counters()
                    .framesMissedFinality.load(std::memory_order_relaxed);
            const int64_t sOutMin =
                ivars->runtime.txStreamEngine.TakeMinFinalityMarginPackets();
            // The receive side also observes the capture ring, so without a
            // CoreAudio read in the interval the minimum is just the ring
            // capacity. Headroom only exists relative to a read.
            const bool inputRead =
                directControl->rxCaptureBufferTelemetry.completedReaderBeginReadCalls.load(
                    std::memory_order_relaxed) != 0;
            const uint64_t sInMin = inputRead
                ? directControl->rxCaptureBufferTelemetry.completedMinimumAvailableFrames.load(
                      std::memory_order_relaxed)
                : UINT64_MAX;
            ASFW_LOG(
                DirectAudio,
                "[TxPrep] forcedNoData=%llu missedFinality=%llu sOutMinPk=%lld "
                "sInMinFr=%lld sInStarve=%llu sInStart=%llu margin=%u min=%u latUs=%llu/%llu/%llu "
                "late1500=%llu wakes=%llu fillUs=%llu%{public}s",
                directControl->txReplayForcedNoData.load(std::memory_order_relaxed),
                missedNow >= ivars->runtime.txMissedFinalityAtStart
                    ? missedNow - ivars->runtime.txMissedFinalityAtStart
                    : 0,
                sOutMin == INT64_MAX ? -1LL : static_cast<long long>(sOutMin),
                sInMin == UINT64_MAX ? -1LL : static_cast<long long>(sInMin),
                directControl->rxCaptureBufferTelemetry.completedStarvationEvents.load(
                    std::memory_order_relaxed),
                directControl->captureRingStartupStarvations.load(std::memory_order_relaxed),
                boundedMargin,
                minCommittedMargin,
                latencyNanos / 1000,
                intervalLatencyMaxNanos / 1000,
                maxLatencyNanos / 1000,
                directControl->txPreparationAtLeast1500Us.load(
                    std::memory_order_relaxed),
                wakeSamples,
                ASFW::Timing::hostTicksToNanos(
                    ivars->runtime.txFillMaxDurationTicks.exchange(
                        0, std::memory_order_relaxed)) / 1000,
                boundedMargin <= kCommittedMarginDangerPackets ? " DANGER" : "");
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
        // Every pass fills what CoreAudio has written, so every pass handles
        // the request it saw.
        directControl->txPreparationRequests.MarkHandled(audioRequested, now);
        directControl->txPreparationRequests.FinishWake();
        // A CoreAudio callback can publish while this action is preparing
        // slots. It saw wakeScheduled=true and deliberately did not enqueue a
        // second action; hand it one now after draining the latest target.
        scheduleAudioFollowUp =
            directControl->txPreparationRequests.NeedsHandling() &&
            directControl->txPreparationRequests.TryScheduleWake();
    }

    txControl->MarkRefillHandled(requested);

    // [TxPlace]: where a transmitted frame sits against the HAL clock
    // (milestone 6, documentation/TX_OWNERSHIP.md). The first measurement of
    // each stream only (T8); the start-to-start difference is what matters.
    if (!ivars->runtime.txPlacementLogged) {
        ASFW::Audio::DriverKit::TxPlacementSample placement{};
        if (ASFW::Audio::DriverKit::MeasureTxPlacement(*ivars, placement)) {
            ivars->runtime.txPlacementLogged = true;
            ASFW_LOG(DirectAudio,
                     "[TxPlace] pkt=%llu frame=%llu halSample=%lld offset=%lld rate=%u",
                     placement.packetIndex, placement.firstAudioFrame,
                     placement.halSampleTime, placement.offsetFrames,
                     ivars->runtime.txStreamEngine.StreamConfig().sampleRate);
        }
    }

    if (scheduleAudioFollowUp && ivars->device.audioNub && ivars->txPreparationAction) {
        // Same direct, one-way OSAction path as the hardware refill callback.
        ivars->device.audioNub->TxPreparationReady(ivars->txPreparationAction.get(),
            directControl->txPreparationRequests.RequestedGeneration());
    }
}
