//
// ASFWAudioDriverZts.cpp
// ASFWDriver
//
// Ordered zero-timestamp queue drain for ASFWAudioDriver.
//

#include "ASFWAudioDriverPrivate.hpp"
#include "../../Common/TimingUtils.hpp"
#include "../../Logging/Logging.hpp"

#include "../Config/TimingCursorPolicy.hpp"

#include <DriverKit/DriverKit.h>

namespace ASFW::Audio::DriverKit {

ASFW::Audio::Runtime::ZtsMirrorPublishResult PublishSharedZeroTimestampToHAL(ASFWAudioDriver_IVars& ivars,
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

    bool published = false;
    bool invalidTimeline = false;
    do {
        ASFW::Audio::Runtime::HostClockAnchorSample anchor{};
        while (control->hostClockAnchor.TryPop(anchor)) {
            const uint64_t lastSampleFrame =
                ivars.runtime.lastHalZeroTimestampSampleFrame.load(
                    std::memory_order_relaxed);
            const uint64_t lastHostTicks =
                ivars.runtime.lastHalZeroTimestampHostTicks.load(
                    std::memory_order_relaxed);
            // Anchors must sit on the declared P grid and advance
            // monotonically. The step may be any positive multiple of P:
            // SYT qualification can complete several periods after the
            // synthetic frame-0 prime, and a mid-stream relock can skip
            // grid points. Requiring exactly last+P here would reject the
            // first post-gap anchor and then every later one (each compares
            // against the stale lastSampleFrame), permanently starving the
            // HAL of real timestamps.
            if (anchor.hostTicks == 0 ||
                anchor.hostNanosPerSampleQ8 == 0 ||
                (anchor.sampleFrame % P) != 0 ||
                (lastHostTicks != 0 &&
                 (anchor.sampleFrame <= lastSampleFrame ||
                  anchor.hostTicks <= lastHostTicks))) {
                ASFW_LOG(
                    DirectAudio,
                    "ADK FATAL ZTS queue invalid sample=%llu host=%llu nanos=%u lastSample=%llu lastHost=%llu period=%u",
                    anchor.sampleFrame,
                    anchor.hostTicks,
                    anchor.hostNanosPerSampleQ8,
                    lastSampleFrame,
                    lastHostTicks,
                    P);
                invalidTimeline = true;
                continue;
            }

            audioDevice->UpdateCurrentZeroTimestamp(
                anchor.sampleFrame, anchor.hostTicks);
            const uint64_t consumed =
                control->hostClockAnchor.consumerCursor.load(
                    std::memory_order_acquire);
            ivars.runtime.lastHalZeroTimestampSampleFrame.store(
                anchor.sampleFrame, std::memory_order_relaxed);
            ivars.runtime.lastHalZeroTimestampHostTicks.store(
                anchor.hostTicks, std::memory_order_relaxed);
            ivars.runtime.lastHalZeroTimestampGeneration.store(
                consumed, std::memory_order_release);

            control->hostClockAnchor.mirrorPublications.fetch_add(
                1, std::memory_order_relaxed);
            control->counters.CountRxAdkZtsPublished();
            published = true;

            if (logSuccess) {
                ASFW_LOG(
                    DirectAudio,
                    "ADK DBG ZTS publish reason=%{public}s sample=%llu host=%llu period=%u queueRead=%llu",
                    reason ? reason : "unknown",
                    anchor.sampleFrame,
                    anchor.hostTicks,
                    P,
                    consumed);
            }
        }
    } while (control->hostClockAnchor.FinishDrainAndNeedsAnotherPass());

    if (published) {
        return ASFW::Audio::Runtime::ZtsMirrorPublishResult::Published;
    }
    return invalidTimeline
        ? ASFW::Audio::Runtime::ZtsMirrorPublishResult::InvalidTimeline
        : ASFW::Audio::Runtime::ZtsMirrorPublishResult::AlreadyPublished;
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

    while (nextPacketToPrepare < targetPacketIndex && preparedCount < maxToPrepare) {
        ASFW::Driver::TxTimingModel::Decision decision{};
        const bool wasTimingSeeded =
            ivars.runtime.txTimingModel.IsSeeded();
        const bool cadenceWouldCarryData =
            ivars.runtime.txStreamEngine.NextPacketWouldCarryData();
        if (allowRecoveredClock && cadenceWouldCarryData) {
            int64_t packetAnchorTicks = 0;
            if (ivars.runtime.txExecutionTimeline.AnchorForPacket(
                    nextPacketToPrepare, packetAnchorTicks)) {
                decision =
                    ivars.runtime.txTimingModel.PeekNextDataSyt(
                        packetAnchorTicks,
                        directControl->rxSytCadence);
                if (decision.seededThisCall && decision.syt != 0xFFFF) {
                    ASFW::Driver::RxSytCadence::Snapshot rx{};
                    if (directControl->rxSytCadence.TrySnapshot(rx) &&
                        rx.established) {
                        const int64_t phaseTicks =
                            ivars.runtime.txTimingModel.OutputPhaseTicks();
                        const int64_t rxRecoveredPhaseTicksDelayFree =
                            ASFW::Timing::normalizeOffsetDomain(
                                rx.recoveredPhaseTicks -
                                ivars.runtime.txTimingModel.GetConfig()
                                    .xmitTransferDelayTicks);
                        const int64_t deltaTicks =
                            ASFW::Timing::extOffsetDiff(
                                phaseTicks, rxRecoveredPhaseTicksDelayFree);
                        // deltaFrames measures how far in the future this
                        // packet hits the wire (preparation pipeline depth +
                        // transfer delay, in frames). Mapping the packet to
                        // lastZtsFrame + deltaFrames would make it carry the
                        // frames CoreAudio produces at the very instant of the
                        // DMA fetch — a zero-margin race the fetch wins, which
                        // ships silence while AmdtpPayloadWriter counts
                        // successful writes (completionCursor's 32-packet IRQ
                        // granularity hides the race from wroteIntoTransmitted).
                        // Subtract the policy's output content lead so the
                        // packet carries frames produced outLead+cursorOffset
                        // frames before its transmit; that margin is also the
                        // device-facing latency the policy reports to HAL.
                        const int64_t deltaFrames = deltaTicks / 512;
                        const uint64_t lastZtsFrame =
                            ivars.runtime.lastHalZeroTimestampSampleFrame.load(
                                std::memory_order_relaxed);
                        constexpr uint32_t kFpp =
                            ASFW::IsochTransport::AudioTimingGeometry::
                                kFramesPerDataPacket;
                        const auto cursorPolicy =
                            ASFW::Audio::TimingCursorPolicy::
                                MakeDice48kBlocking().Snapshot();
                        const int64_t contentLeadFrames =
                            static_cast<int64_t>(
                                cursorPolicy.outputPacketLeadFrames) +
                            cursorPolicy.outputCursorOffsetFrames;
                        int64_t targetFrame =
                            static_cast<int64_t>(lastZtsFrame) +
                            deltaFrames - contentLeadFrames;
                        if (targetFrame < 0) {
                            targetFrame = 0;
                        }
                        const uint64_t alignedFrame =
                            (static_cast<uint64_t>(targetFrame) / kFpp) * kFpp;
                        if (ivars.runtime.txStreamEngine.AlignFrameCursorOnce(
                                alignedFrame)) {
                            ASFW_LOG(
                                DirectAudio,
                                "ADK DBG TX timeline initially aligned: phaseTicks=%lld rxPhase=%lld deltaTicks=%lld deltaFrames=%lld contentLead=%lld lastZtsFrame=%llu alignedFrame=%llu",
                                phaseTicks,
                                rxRecoveredPhaseTicksDelayFree,
                                deltaTicks,
                                deltaFrames,
                                contentLeadFrames,
                                lastZtsFrame,
                                alignedFrame);
                        }
                    }
                }
            }
        }

        if (decision.health !=
            ASFW::Driver::TxTimingModel::LeadHealth::kNotSeeded) {
            directControl->txLastLeadTicks.store(
                decision.leadTicks, std::memory_order_relaxed);
            int64_t previousMin =
                directControl->txMinimumLeadTicks.load(
                    std::memory_order_relaxed);
            while (decision.leadTicks < previousMin &&
                   !directControl->txMinimumLeadTicks
                        .compare_exchange_weak(
                            previousMin,
                            decision.leadTicks,
                            std::memory_order_relaxed,
                            std::memory_order_relaxed)) {
            }
            int64_t previousMax =
                directControl->txMaximumLeadTicks.load(
                    std::memory_order_relaxed);
            while (decision.leadTicks > previousMax &&
                   !directControl->txMaximumLeadTicks
                        .compare_exchange_weak(
                            previousMax,
                            decision.leadTicks,
                            std::memory_order_relaxed,
                            std::memory_order_relaxed)) {
            }
        }

        ASFW::Protocols::Audio::AMDTP::AmdtpTimingState timing{};
        timing.txClockValid = decision.syt != 0xFFFFu;
        timing.disposition =
            timing.txClockValid
                ? ASFW::Protocols::Audio::AMDTP::
                      AmdtpPacketDisposition::Data
                : ASFW::Protocols::Audio::AMDTP::
                      AmdtpPacketDisposition::NoData;
        timing.nextDataSyt = decision.syt;

        if (!ivars.runtime.txStreamEngine.PrepareNextTransmitSlot(
                static_cast<uint32_t>(nextPacketToPrepare), timing)) {
            break;
        }

        const uint32_t slotIdx = nextPacketToPrepare % numSlots;
        auto& meta = metadataRing[slotIdx];
        if (meta.payloadLength > 8) {
            ivars.runtime.txTimingModel.CommitDataPacket();
            directControl->counters.txDataPackets.fetch_add(
                1, std::memory_order_relaxed);
            directControl->counters.txValidSytPackets.fetch_add(
                1, std::memory_order_relaxed);
        } else {
            directControl->counters.txNoDataPackets.fetch_add(
                1, std::memory_order_relaxed);
            directControl->counters.txSytFfffPackets.fetch_add(
                1, std::memory_order_relaxed);
            if (wasTimingSeeded && cadenceWouldCarryData) {
                directControl->counters.txPostLockNoDataPackets.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
        directControl->counters.txPackets.fetch_add(
            1, std::memory_order_relaxed);

        nextPacketToPrepare++;
        preparedCount++;
    }

    return preparedCount;
}

void PrefillTxRingBeforeStart(ASFWAudioDriver_IVars& ivars) noexcept {
    const uint32_t numSlots = ivars.runtime.txSlotProvider.numSlots;
    auto* metadataRing = ivars.runtime.txSlotProvider.metadataRing;
    if (numSlots == 0 || metadataRing == nullptr) {
        return;
    }

    // Seed the ENTIRE shared metadata ring with NO-DATA before the IT DMA
    // context is started, so no refill interrupt during bring-up can meet an
    // uncommitted slot (which fatally stops the context — channel never
    // reaches the wire).
    //
    // The full ring (not just the pump's runtime lead) is required: the pump
    // (TxPreparationReady) is gated on isRunning, which StartDevice only sets
    // after StartStreaming + geometry validation — tens of ms after IT RUN.
    // Seeding kTxSharedSlotPackets (512 pkts = 64 ms) gives bring-up a
    // ~(512−192)·125 µs ≈ 40 ms budget before the refill reaches lap 2;
    // seeding only the 224-packet lead gave 8 ms (2026-06-12 hardware FATAL
    // UNDERRUN at fillAbsIdx=224).
    //
    // Critically, this runs before RX cadence acquisition and before any
    // OUTPUT_LAST completion can provide a packet execution anchor. Saffire's
    // StartStreams/FillFirewireBuffers path likewise pre-fills with NO_INFO
    // until ReadFirewireBuffers establishes the RX cadence history.
    // The packetizer owns DBC continuity, so the prefill-to-live handoff stays
    // gapless.
    ASFW::Protocols::Audio::AMDTP::AmdtpTimingState timing{};
    timing.txClockValid = false;
    timing.disposition =
        ASFW::Protocols::Audio::AMDTP::
            AmdtpPacketDisposition::NoData;

    uint32_t prepared = 0;
    constexpr uint32_t kPreparationLeadPackets =
        ASFW::IsochTransport::AudioTimingGeometry::
            kTxSharedSlotPackets;
    for (uint64_t packetIndex = 0;
         packetIndex < kPreparationLeadPackets;
         ++packetIndex) {
        if (!ivars.runtime.txStreamEngine.PrepareNextTransmitSlot(
                static_cast<uint32_t>(packetIndex), timing)) {
            break;
        }
        ++prepared;
    }

    ASFW_LOG(DirectAudio,
             "ADK DBG TX prefill seeded %u NO_INFO packets before isoch start (lead=%llu, model left unseeded)",
             prepared,
             static_cast<uint64_t>(kPreparationLeadPackets));
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
        !ivars->runtime.isRunning.load(
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

    // Saffire records the TX phase origin at the same instant ReadFirewireBuffers
    // seeds takeTimeStamp. Mirror that ordering: keep shipping NO_INFO until the
    // first real RX anchor has reached HAL, so the alignment step inside
    // PrepareTransmitSlots never reads a zero lastHalZeroTimestampSampleFrame.
    const bool halAnchored =
        ivars->runtime.lastHalZeroTimestampGeneration.load(
            std::memory_order_acquire) != 0;
    (void)ASFW::Audio::DriverKit::PrepareTransmitSlots(
        *ivars,
        exposeCursor,
        targetPacketIndex,
        ASFW::IsochTransport::AudioTimingGeometry::
            kTxPreparationLeadPackets,
        halAnchored);

    if (auto* directControl =
            ivars->runtime.directAudioGraph.control) {
        const uint64_t now = mach_absolute_time();
        const uint64_t requestedAt =
            txControl->preparationRequestHostTicks.load(
                std::memory_order_relaxed);
        const uint64_t latency =
            now >= requestedAt ? now - requestedAt : 0;
        directControl->txLastPreparationLatencyTicks.store(
            latency, std::memory_order_relaxed);
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
