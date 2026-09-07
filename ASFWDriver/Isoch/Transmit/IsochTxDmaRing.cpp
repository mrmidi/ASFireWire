// IsochTxDmaRing.cpp

#include "IsochTxDmaRing.hpp"
#include "TxPacketIndexLift.hpp"

#include "../../Common/TimingUtils.hpp"

#include <algorithm>
#include <DriverKit/IOLib.h>

namespace ASFW::Isoch::Tx {

using namespace ASFW::Async::HW;
using namespace ASFW::Driver;

namespace {

/// Pick the payload image the descriptor will address.
///
/// Image 1 wins only when the producer offered it for exactly this packet's
/// commit generation and no one has claimed it yet. Anything else -- never
/// offered, already claimed, or a state left by an earlier lap through the
/// slot -- falls back to the armed image 0, which is always complete.
///
/// This is a claim, not a peek: it moves the arbitration word, so the producer
/// can no longer observe this packet as undecided. Binding is still not
/// finality -- a packet bound to image 0 here may be repointed later -- which
/// is why it never seals.
[[nodiscard]] uint32_t SelectPayloadImage(IsochTxPacketMeta& meta,
                                          uint64_t expectedGeneration) noexcept {
    return ClaimLateTxPayload(meta, expectedGeneration) ? 1u : 0u;
}

// Replace the channel field [13:8] of a little-endian OHCI isoch transmit header
// quadlet with the channel owned by this ring. Linux queue_iso_transmit() likewise
// takes the channel from the isoch context, never from content-layer metadata
// (references/linux-ohci-firewire-low-level-stack/ohci.c:3373-3381).
[[nodiscard]] inline uint32_t StampHeaderChannel(uint32_t leHeader, uint8_t channel) noexcept {
    // An all-zero header is the "no packet" sentinel (e.g. underrun): leave it
    // untouched so the ring never invents packet state.
    if (leHeader == 0) {
        return 0;
    }
    uint32_t h = OSSwapLittleToHostInt32(leHeader);
    h = (h & ~(static_cast<uint32_t>(0x3F) << 8)) |
        (static_cast<uint32_t>(channel & 0x3F) << 8);
    return OSSwapHostToLittleInt32(h);
}
} // namespace

void IsochTxDmaRing::ResetForStart() noexcept {
    ++captureEpoch_; // Frozen history deliberately survives recovery/restart.
    softwareFillAbsIdx_ = 0;
    lastHwPacketIndex_ = 0;
    lastObservationCycleTimer_ = 0;
    lastObservationCycleTimerValid_ = false;
    ringPacketsAhead_ = 0;
    startLapObserved_ = false;

    nextTransmitCycle_ = 0;
    cycleTrackingValid_ = false;
    lastHwTimestamp_ = 0;

    counters_.lastDmaGapPackets.store(Layout::kNumPackets, std::memory_order_relaxed);
    counters_.minDmaGapPackets.store(Layout::kNumPackets, std::memory_order_relaxed);

    // Every counter that describes *this* stream's completion behaviour has to
    // go with it. A high-water that outlives the geometry it was measured under
    // is worse than no counter: after a runtime tuning change the watchdog line
    // keeps reporting the previous depth's worst case, which is exactly the
    // reading an operator would use to judge whether the new depth is safe.
    // (The gap counters above were already reset; these were not.)
    counters_.maxDeltaConsumed.store(0, std::memory_order_relaxed);
    counters_.lapsRecovered.store(0, std::memory_order_relaxed);
    counters_.lapRecoveryEvents.store(0, std::memory_order_relaxed);
    counters_.lapUnresolvable.store(0, std::memory_order_relaxed);
    counters_.abandonedOnLap.store(0, std::memory_order_relaxed);
    counters_.criticalGapEvents.store(0, std::memory_order_relaxed);
}

void IsochTxDmaRing::SeedCycleTracking(Driver::HardwareInterface& hw) noexcept {
    const uint32_t cycleTime = hw.ReadCycleTime();
    const uint32_t currentCycle = (cycleTime >> 12) & 0x1FFF;
    nextTransmitCycle_ = (currentCycle + 4) % 8000;
    cycleTrackingValid_ = true;
    lastHwTimestamp_ = 0;
    ASFW_LOG(Isoch, "IT: Cycle tracking seeded: currentCycle=%u nextTxCycle=%u",
             currentCycle, nextTransmitCycle_);
}

// The CommandPtr names a descriptor inside the ring, so the difference between
// two readings is a difference of slot indices: it is bounded by
// `kNumPackets - 1` whatever the controller actually did. If the controller
// completed a whole lap or more since the previous observation, that lap is not
// small in this arithmetic -- it is *absent* from it, and because the completion
// cursor is advanced by accumulating these differences, the loss is permanent
// and every later reading is relative to the wrong origin.
//
// The cost is not a counting error alone. The ring branches from its last packet
// back to its first, so an unrefilled context re-transmits stale audio; the
// client's content then reaches the wire one lap -- kNumPackets packets, one
// packet per isochronous cycle, so 48 * 125 us = 6 ms -- later than planned, for
// the remaining life of the stream. Laps accumulate and the offset never
// returns. (What that lap costs in audio frames is the content layer's
// arithmetic, not this one's.)
//
// The cycle timer supplies the lap the pointer cannot. An IT context begins one
// packet per isochronous cycle, so cycles elapsed since the previous observation
// bounds how far the controller advanced, and `LiftRingSlotToAbsolute` picks the
// index congruent to the observed slot nearest that bound. Elapsed cycles is an
// upper bound rather than an equality -- ASFW self-links each packet's skip
// address, so a lost cycle or FIFO overrun skips a cycle without advancing past
// the packet -- and the nearest-congruent rule absorbs that error while
// accumulated skips stay under half a ring. See TxPacketIndexLift.hpp, which
// owns this reasoning and its bound.
uint32_t IsochTxDmaRing::ComputeDeltaConsumed(const uint32_t hwPacketIndex,
                                              const uint32_t nowCycleTimer) noexcept {
    const uint32_t prevHwPacketIndex = lastHwPacketIndex_;
    const uint32_t naiveDelta =
        (hwPacketIndex >= prevHwPacketIndex)
            ? (hwPacketIndex - prevHwPacketIndex)
            : ((Layout::kNumPackets - prevHwPacketIndex) + hwPacketIndex);

    uint32_t deltaConsumed = naiveDelta;

    if (lastObservationCycleTimerValid_ && nowCycleTimer != 0) {
        const uint32_t elapsedCycles =
            Tx::CyclesBetween(lastObservationCycleTimer_, nowCycleTimer);
        // CyclesBetween is only correct across one wrap of the three-bit seconds
        // field. At the wrap the answer is indistinguishable from zero elapsed,
        // so refuse rather than adjudicate on it.
        if (elapsedCycles + 1U < Tx::kCycleTimerWrapCycles) {
            const uint64_t liftedDelta = Tx::RecoverConsumedDelta(
                prevHwPacketIndex, hwPacketIndex, elapsedCycles,
                Layout::kNumPackets);
            // Congruent by construction, so any disagreement is whole laps.
            if (liftedDelta > naiveDelta) {
                const uint64_t lapsLost =
                    (liftedDelta - naiveDelta) / Layout::kNumPackets;
                deltaConsumed = liftedDelta > UINT32_MAX
                    ? UINT32_MAX : static_cast<uint32_t>(liftedDelta);
                counters_.lapsRecovered.fetch_add(lapsLost,
                                                  std::memory_order_relaxed);
                counters_.lapRecoveryEvents.fetch_add(1,
                                                      std::memory_order_relaxed);
                ASFW_LOG_ERROR(Isoch,
                               "[TxLapRecover] prevSlot=%u slot=%u elapsedCycles=%u "
                               "naive=%u lifted=%llu lapsLost=%llu "
                               "packetsRecovered=%llu -- the completion cursor "
                               "would have lost these permanently",
                               prevHwPacketIndex, hwPacketIndex, elapsedCycles,
                               naiveDelta, liftedDelta, lapsLost,
                               lapsLost * Layout::kNumPackets);
            }
        } else {
            counters_.lapUnresolvable.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (lastObservationCycleTimerValid_) {
        counters_.lapUnresolvable.fetch_add(1, std::memory_order_relaxed);
    }

    lastHwPacketIndex_ = hwPacketIndex;
    if (nowCycleTimer != 0) {
        lastObservationCycleTimer_ = nowCycleTimer;
        lastObservationCycleTimerValid_ = true;
    }

    // A recovered delta can exceed the ring, which means the controller lapped
    // the software fill: nothing is ahead of it any more.
    ringPacketsAhead_ = deltaConsumed >= ringPacketsAhead_
        ? 0U : ringPacketsAhead_ - deltaConsumed;

    return deltaConsumed;
}

void IsochTxDmaRing::UpdateGapCounters(const uint32_t gap) noexcept {
    counters_.lastDmaGapPackets.store(gap, std::memory_order_relaxed);
    uint32_t prevMin = counters_.minDmaGapPackets.load(std::memory_order_relaxed);
    while (gap < prevMin &&
           !counters_.minDmaGapPackets.compare_exchange_weak(
               prevMin, gap, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }

    constexpr uint32_t kCriticalGapThreshold = Layout::kNumPackets / 5;
    if (gap < kCriticalGapThreshold) {
        counters_.criticalGapEvents.fetch_add(1, std::memory_order_relaxed);
    }
}

void IsochTxDmaRing::ResyncCycleTracking(Driver::HardwareInterface& hw,
                                         const uint32_t hwPacketIndex,
                                         const uint32_t deltaConsumed,
                                         RefillOutcome& out) noexcept {
    if (deltaConsumed == 0 || !cycleTrackingValid_) {
        return;
    }

    const uint32_t lastProcessedPkt = (hwPacketIndex + Layout::kNumPackets - 1) % Layout::kNumPackets;
    out.completedPacketIndex = lastProcessedPkt;
    out.completedPacketCount = deltaConsumed;
    auto* processedOL = slab_.GetDescriptorPtr(
        lastProcessedPkt * Layout::kBlocksPerPacket +
        Layout::kCompletionBlock);

    if (dmaMemory_) {
        dmaMemory_->FetchFromDevice(reinterpret_cast<const std::byte*>(processedOL), sizeof(*processedOL));
    }

    const uint16_t hwTimestamp = static_cast<uint16_t>(processedOL->statusWord & 0xFFFF);
    out.hwTimestamp = hwTimestamp;

    if (processedOL->statusWord == 0) {
        return;
    }

    const uint32_t hwCycle = (hwTimestamp & 0x1FFFu) % 8000u;
    out.hwTimestamp = static_cast<uint16_t>(0x8000u | hwCycle);
    lastHwTimestamp_ = hwTimestamp;

    const uint32_t softwareFillIndex = static_cast<uint32_t>(softwareFillAbsIdx_ % Layout::kNumPackets);
    const uint32_t aheadCount =
        (softwareFillIndex + Layout::kNumPackets - lastProcessedPkt) % Layout::kNumPackets;
    nextTransmitCycle_ = (hwCycle + aheadCount) % 8000;
}

void IsochTxDmaRing::CommitRefill(const uint32_t toFill) noexcept {
    softwareFillAbsIdx_ += toFill;
    ringPacketsAhead_ += toFill;

    std::atomic_thread_fence(std::memory_order_release);
    ASFW::Driver::WriteBarrier();

    counters_.packetsRefilled.fetch_add(toFill, std::memory_order_relaxed);
}

bool IsochTxDmaRing::ReadLiveHardwareAbsIndex(
    Driver::HardwareInterface& hw,
    const uint8_t contextIndex,
    const uint64_t referenceAbsIdx,
    uint64_t& outAbsIdx) noexcept {
    uint32_t cmdPtr = 0;
    {
        auto access = hw.TryBeginAccess();
        if (!access) return false;
        cmdPtr = access.Read(static_cast<Register32>(
            DMAContextHelpers::IsoXmitCommandPtr(contextIndex)));
    }
    uint32_t hwPacketIndex = 0;
    if (!DecodeHardwarePacketIndex(cmdPtr, hwPacketIndex)) return false;

    // The controller only moves forward, and a pass that let it advance a whole
    // ring lap has already holed the ring and is detected elsewhere. Lift the
    // modulo index onto the absolute timeline at or after the reference.
    const uint64_t lapBase =
        referenceAbsIdx - (referenceAbsIdx % Layout::kNumPackets);
    uint64_t live = lapBase + hwPacketIndex;
    if (live < referenceAbsIdx) live += Layout::kNumPackets;
    outAbsIdx = live;
    return true;
}

void IsochTxDmaRing::SealOnArmedImage(
    IsochTxPacketMeta& meta,
    const uint64_t generation,
    IsochTxQueueControl* controlBlock,
    RefillOutcome& out) noexcept {
    if (!FinalizeTxPayloadOnArmedImage(meta, generation)) {
        return;
    }
    // The producer published an image the wire will not carry. Counting it
    // here, at the only place that can know, is what lets the engine correct
    // an optimistic fill count into a truthful one.
    ++out.latePayloadLostPublications;
    controlBlock->latePayloadLostPublicationCount.fetch_add(
        1, std::memory_order_relaxed);
}

void IsochTxDmaRing::TryBindLatePayload(
    Driver::HardwareInterface& hw,
    const uint8_t contextIndex,
    const uint64_t packetAbs,
    const uint64_t hardwareAbsIdx,
    IsochTxPacketMeta* metadataRing,
    IsochTxQueueControl* controlBlock,
    const uint32_t numSlots,
    uint8_t* payloadBase,
    const TxPayloadDmaMap& payloadDmaMap,
    RefillOutcome& out) noexcept {
    const uint32_t producerSlot = static_cast<uint32_t>(packetAbs % numSlots);
    auto& meta = metadataRing[producerSlot];
    const uint64_t expectedGeneration =
        ExpectedTxCommitGeneration(packetAbs, numSlots);
    if (meta.packetIndex != packetAbs ||
        meta.commitGeneration.load(std::memory_order_acquire) !=
            expectedGeneration ||
        meta.selectedPayloadImage == 1) {
        return;
    }
    // A peek, only to avoid validating a packet that has nothing on offer. It
    // is not the decision -- the decision is the claim below.
    if (meta.payloadArbitration.load(std::memory_order_acquire) !=
        MakeTxPayloadArbitration(expectedGeneration,
                                 TxPayloadArbitration::kLateImageReady)) {
        return;
    }

    const uint32_t payloadLength = meta.payloadLength;
    const uint64_t payloadOffset = TxPayloadImageOffset(
        producerSlot, 1, controlBlock->slotStrideBytes);
    std::array<TxPayloadDmaFragment, 2> fragments{};

    // A late rebind is legal only when image 1 preserves every descriptor field
    // except the mutable tail address. Every rejection below is a property of
    // this packet's geometry, not a transient condition, so a rejected packet
    // is sealed immediately: retrying it on later passes could only reject it
    // again, once per pass, which is how a single bad packet used to inflate
    // the rejection count.
    const bool mappable =
        payloadLength != 0 && meta.payloadPrefixBytes != 0 &&
        meta.payloadPrefixBytes < payloadLength &&
        payloadDmaMap.ResolveTwoFragments(
            payloadOffset, payloadLength, fragments, meta.payloadPrefixBytes);

    const uint32_t hardwareSlot =
        static_cast<uint32_t>(packetAbs % Layout::kNumPackets);
    auto* prefixDescriptor = slab_.GetDescriptorPtr(
        hardwareSlot * Layout::kBlocksPerPacket + Layout::kFirstPayloadBlock);
    auto* tailDescriptor = slab_.GetDescriptorPtr(
        hardwareSlot * Layout::kBlocksPerPacket + Layout::kCompletionBlock);
    const uint32_t prefixLength = prefixDescriptor->control & 0xffffu;
    const uint32_t tailLength = tailDescriptor->control & 0xffffu;
    const uint64_t armedPayloadOffset = TxPayloadImageOffset(
        producerSlot, 0, controlBlock->slotStrideBytes);

    const bool shapePreserved = mappable &&
        fragments[0].length == prefixLength &&
        fragments[1].length == tailLength &&
        std::memcmp(payloadBase + armedPayloadOffset,
                    payloadBase + payloadOffset,
                    meta.payloadPrefixBytes) == 0;

    if (!shapePreserved) {
        ++out.latePayloadRebindRejected;
        controlBlock->latePayloadRebindRejectedCount.fetch_add(
            1, std::memory_order_relaxed);
        SealOnArmedImage(meta, expectedGeneration, controlBlock, out);
        return;
    }

    // Make image 1 visible to the device first. Writing that memory is
    // harmless whatever the controller is doing -- it is not the live image --
    // so it belongs before the deadline check rather than inside it.
    const auto* payload = reinterpret_cast<const std::byte*>(
        payloadBase + payloadOffset);
    if (dmaMemory_) {
        dmaMemory_->PublishToDevice(payload, payloadLength);
        dmaMemory_->PublishBarrier();
    }

    // The position that authorised this pass was sampled before completion
    // processing and before every packet examined ahead of this one, so it can
    // be several packets old by the time we get here. The guard has to hold at
    // the store, not at the top of the pass: re-read the controller now and
    // abandon the rebind if it has reached the packet. The armed image is
    // complete and already bound, so abandoning costs content, never a holed
    // ring.
    //
    // This closes snapshot age only. Controller prefetch, and controller
    // progress between this read and the store below, remain open questions
    // that need reference or hardware evidence to settle.
    uint64_t liveAbsIdx = hardwareAbsIdx;
    if (!ReadLiveHardwareAbsIndex(hw, contextIndex, hardwareAbsIdx,
                                  liveAbsIdx)) {
        // No authority to decide, so decide nothing. The packet stays open for
        // a later pass, and the finality seal accounts it if it runs out of
        // time first. Sealing here would turn a transient loss of MMIO access
        // into permanently discarded content.
        return;
    }
    if (packetAbs < liveAbsIdx +
                        ASFW::Shared::Isoch::IsochQueueGeometry::
                            kPayloadRepointGuardPackets) {
        // Permanent for this packet: the controller only moves forward, so a
        // later pass would find it further past, fail again, and count again.
        // Seal it now, which also books the producer's image as the lost
        // publication it is.
        ++out.latePayloadRebindMissedDeadline;
        controlBlock->latePayloadRebindMissedDeadlineCount.fetch_add(
            1, std::memory_order_relaxed);
        SealOnArmedImage(meta, expectedGeneration, controlBlock, out);
        return;
    }

    // Single linearization point for "transport chose image 1". After it
    // succeeds no producer can still be told it owns this packet.
    if (!ClaimLateTxPayload(meta, expectedGeneration)) {
        return;
    }

    meta.selectedPayloadImage = 1;
    meta.payloadSeal = ASFW::Shared::Isoch::SealTxPayload(
        payloadBase + payloadOffset, payloadLength);
    std::atomic_thread_fence(std::memory_order_release);
    tailDescriptor->dataAddress = fragments[1].deviceAddress;
    if (dmaMemory_) {
        dmaMemory_->PublishToDevice(
            reinterpret_cast<const std::byte*>(&tailDescriptor->dataAddress),
            sizeof(tailDescriptor->dataAddress));
        dmaMemory_->PublishBarrier();
    } else {
        ASFW::Driver::WriteBarrier();
    }

    // Against the position actually checked, so the reported minimum is a
    // margin that existed rather than one the snapshot implied.
    const uint32_t distance =
        static_cast<uint32_t>(packetAbs - liveAbsIdx);
    uint32_t previous = controlBlock->minimumLatePayloadRebindDistance.load(
        std::memory_order_relaxed);
    while (distance < previous &&
           !controlBlock->minimumLatePayloadRebindDistance
                .compare_exchange_weak(previous, distance,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {
    }
    ++out.latePayloadRebinds;
    controlBlock->latePayloadRebindCount.fetch_add(
        1, std::memory_order_relaxed);
}

void IsochTxDmaRing::RefreshLatePayloadBindings(
    Driver::HardwareInterface& hw,
    const uint8_t contextIndex,
    const uint64_t hardwareAbsIdx,
    const uint32_t sealCycleTimer,
    IsochTxPacketMeta* metadataRing,
    IsochTxQueueControl* controlBlock,
    const uint32_t numSlots,
    uint8_t* payloadBase,
    const TxPayloadDmaMap& payloadDmaMap,
    RefillOutcome& out) noexcept {
    using Geometry = ASFW::Shared::Isoch::IsochQueueGeometry;

    const uint64_t mappedEnd =
        controlBlock->mappedEnd.load(std::memory_order_acquire);
    const uint64_t firstRepointable = hardwareAbsIdx +
        Geometry::kPayloadRepointGuardPackets;

    // Only transport mutates live descriptor addresses. The producer writes a
    // complete second image and offers it; this pass claims what it can, makes
    // the image DMA-visible, then performs the single aligned store enabled by
    // the invariant-prefix split. This is the ASFW equivalent of keeping
    // content position separate from descriptor/DMA position; it does not
    // depend on Apple's high-level DCL representation.
    for (uint64_t packetAbs = firstRepointable;
         packetAbs < mappedEnd;
         ++packetAbs) {
        TryBindLatePayload(hw, contextIndex, packetAbs, hardwareAbsIdx,
                           metadataRing, controlBlock, numSlots, payloadBase,
                           payloadDmaMap, out);
    }

    // Between completion callbacks the command pointer can advance by one
    // completion group. Keep that interval plus the live-descriptor repoint
    // guard final even if a producer reads this frontier at the worst instant.
    const uint64_t nextFinalizedEnd = std::min<uint64_t>(
        mappedEnd,
        hardwareAbsIdx + Geometry::kPayloadFinalityLeadPackets);
    const uint64_t previousFinalizedEnd =
        controlBlock->finalizedEnd.load(std::memory_order_relaxed);
    const uint64_t monotonicFinalizedEnd =
        std::max(previousFinalizedEnd, nextFinalizedEnd);

    // Seal per packet, before publishing the frontier. The frontier is
    // advisory telemetry; the arbitration word is the decision, and a packet
    // that crosses into finality without one is exactly the state a producer
    // could still win. Iterating the frontier delta -- not the repointable
    // window -- keeps coverage gap-free when the command pointer jumps more
    // than one completion group.
    for (uint64_t packetAbs = previousFinalizedEnd;
         packetAbs < monotonicFinalizedEnd;
         ++packetAbs) {
        // One last claim immediately before the seal. An offer that landed
        // while this pass was busy with later packets is still legitimate
        // content, and binding it costs nothing; without this, the window
        // between passing a packet and sealing it spans the whole scan.
        if (packetAbs >= firstRepointable) {
            TryBindLatePayload(hw, contextIndex, packetAbs, hardwareAbsIdx,
                               metadataRing, controlBlock, numSlots,
                               payloadBase, payloadDmaMap, out);
        }
        auto& meta = metadataRing[static_cast<uint32_t>(packetAbs % numSlots)];
        if (meta.packetIndex != packetAbs) continue;
        SealOnArmedImage(meta,
                         ExpectedTxCommitGeneration(packetAbs, numSlots),
                         controlBlock, out);
    }

    controlBlock->finalizedEnd.store(
        monotonicFinalizedEnd, std::memory_order_release);
    // Date the decision here, where it is taken. A consumer reading this later
    // cannot reconstruct when the frontier moved, only when it noticed.
    if (monotonicFinalizedEnd > previousFinalizedEnd && sealCycleTimer != 0) {
        controlBlock->PublishFinalitySeal(monotonicFinalizedEnd,
                                          sealCycleTimer);
    }
    out.finalizedEnd = monotonicFinalizedEnd;
}

IsochTxDmaRing::PrimeStats IsochTxDmaRing::Prime(
    const TxPayloadDmaMap& payloadDmaMap,
    const uint32_t numSlots,
    const uint32_t slotStrideBytes,
    IsochTxPacketMeta* metadataRing,
    IsochTxQueueControl* controlBlock,
    uint8_t* payloadBase,
    const uint64_t preFillCount) noexcept {
    PrimeStats stats{};
    if (!slab_.IsValid()) {
        ASFW_LOG(Isoch, "IT: Prime failed - descriptor slab is invalid");
        return stats;
    }
    if (!payloadDmaMap.IsValid() ||
        numSlots == 0 || slotStrideBytes == 0 || metadataRing == nullptr ||
        controlBlock == nullptr || payloadBase == nullptr) {
        ASFW_LOG(Isoch,
                 "IT: Prime failed - invalid shared payload contract segments=%zu slots=%u stride=%u meta=%p",
                 payloadDmaMap.SegmentCount(), numSlots, slotStrideBytes, metadataRing);
        return stats;
    }

    const uint32_t numPackets = Layout::kNumPackets;
    if (preFillCount < numPackets || preFillCount > numSlots) {
        ASFW_LOG(
            Isoch,
            "IT: Prime failed - committed prefill=%llu must cover %u descriptors within %u slots",
            preFillCount,
            numPackets,
            numSlots);
        return stats;
    }

    for (uint32_t pktIdx = 0; pktIdx < numPackets; ++pktIdx) {
        const uint32_t descBase = pktIdx * Layout::kBlocksPerPacket;
        auto* desc0 = slab_.GetDescriptorPtr(descBase);
        auto* immDesc = reinterpret_cast<OHCIDescriptorImmediate*>(desc0);

        // Fetch pre-filled metadata for this slot.
        const uint32_t producerSlot = pktIdx % numSlots;
        auto& meta = metadataRing[producerSlot];
        const uint64_t expectedGen =
            ExpectedTxCommitGeneration(pktIdx, numSlots);
        if (meta.commitGeneration.load(std::memory_order_acquire) != expectedGen) {
            ASFW_LOG(
                Isoch,
                "IT: Prime failed - slot %u is not committed for packet %u",
                producerSlot,
                pktIdx);
            return stats;
        }
        if (meta.payloadLength != 0 && (meta.payloadLength < 2 || meta.payloadLength > slotStrideBytes)) {
            ASFW_LOG(
                Isoch,
                "IT: Prime failed - invalid payload length packet=%u slot=%u len=%u stride=%u",
                pktIdx,
                producerSlot,
                meta.payloadLength,
                slotStrideBytes);
            return stats;
        }

        std::array<TxPayloadDmaFragment, 2> payloadFragments{};
        const uint32_t selectedImage =
            SelectPayloadImage(meta, expectedGen);
        meta.selectedPayloadImage = static_cast<uint16_t>(selectedImage);
        const uint64_t payloadOffset = TxPayloadImageOffset(
            producerSlot, selectedImage, slotStrideBytes);
        meta.payloadSeal = ASFW::Shared::Isoch::SealTxPayload(
            payloadBase + payloadOffset, meta.payloadLength);
        if (!payloadDmaMap.ResolveTwoFragments(
                payloadOffset, meta.payloadLength, payloadFragments,
                meta.payloadPrefixBytes)) {
            ASFW_LOG(
                Isoch,
                "IT: Prime payload mapping failed packet=%u slot=%u offset=%llu len=%u segments=%zu",
                pktIdx,
                producerSlot,
                payloadOffset,
                meta.payloadLength,
                payloadDmaMap.SegmentCount());
            return stats;
        }

        // Prime the OMI descriptor structure (Descriptor 0 and 1)
        std::memset(immDesc, 0, sizeof(OHCIDescriptorImmediate));
        immDesc->common.control = OHCIDescriptor::BuildControl({
            .reqCount = 8, // isoch packet header = 2 immediate quadlets (Q0 + data_length)
            .command = OHCIDescriptor::kCmdOutputMore,
            .key = OHCIDescriptor::kKeyImmediate,
            .interruptBits = OHCIDescriptor::kIntNever,
            .branchBits = OHCIDescriptor::kBranchNever,
        });

        // The transport context owns channel selection. Always override the
        // producer placeholder so master and secondary streams obey Configure().
        immDesc->immediateData[0] =
            StampHeaderChannel(meta.immediateHeader[0], channel_);
        immDesc->immediateData[1] = meta.immediateHeader[1];

        // Linux queue_iso_transmit() self-links the skip address so a lost
        // cycle/FIFO overrun skips one cycle without dropping this packet.
        immDesc->common.branchWord =
            MakeBranchWordAT(slab_.GetDescriptorIOVA(descBase), Layout::kBlocksPerPacket);

        // Standard OUTPUT_MORE for the first payload fragment.
        auto* desc2 =
            slab_.GetDescriptorPtr(descBase + Layout::kFirstPayloadBlock);
        std::memset(desc2, 0, sizeof(OHCIDescriptor));
        desc2->control = OHCIDescriptor::BuildControl({
            .reqCount = static_cast<uint16_t>(payloadFragments[0].length),
            .command = OHCIDescriptor::kCmdOutputMore,
            .key = OHCIDescriptor::kKeyStandard,
            .interruptBits = OHCIDescriptor::kIntNever,
            .branchBits = OHCIDescriptor::kBranchNever,
        });
        desc2->dataAddress = payloadFragments[0].deviceAddress;

        // OUTPUT_LAST owns status, interrupt, and the branch to the next packet.
        auto* desc3 =
            slab_.GetDescriptorPtr(descBase + Layout::kCompletionBlock);
        std::memset(desc3, 0, sizeof(OHCIDescriptor));
        desc3->control = OHCIDescriptor::BuildControl({
            .reqCount = static_cast<uint16_t>(payloadFragments[1].length),
            .command = OHCIDescriptor::kCmdOutputLast,
            .key = OHCIDescriptor::kKeyStandard,
            .interruptBits =
                Core::IsTimingGroupBoundary(pktIdx)
                    ? OHCIDescriptor::kIntAlways
                    : OHCIDescriptor::kIntNever,
            .branchBits = OHCIDescriptor::kBranchAlways,
        });
        desc3->control |=
            (1u << (OHCIDescriptor::kStatusShift +
                    OHCIDescriptor::kControlHighShift));
        desc3->dataAddress = payloadFragments[1].deviceAddress;

        const uint32_t nextPktIdx = (pktIdx + 1) % numPackets;
        const uint32_t nextDescIOVA =
            slab_.GetDescriptorIOVA(nextPktIdx * Layout::kBlocksPerPacket);
        desc3->branchWord =
            MakeBranchWordAT(nextDescIOVA, Layout::kBlocksPerPacket);
        AR_init_status(*desc3, 0);
    }

    if (dmaMemory_) {
        dmaMemory_->PublishToDevice(slab_.DescriptorRegion().virtualBase, slab_.DescriptorRegion().size);
    }

    // Since we've primed the ring with the pre-filled data, advance the
    // software tracking state to match the primed count (the hardware capacity).
    // This stops the first refill ISR from immediately trying to "refill" what
    // we just primed, ensuring it fetches the next packet in the sequence.
    softwareFillAbsIdx_ = numPackets;
    ringPacketsAhead_ = numPackets;
    controlBlock->mappedEnd.store(softwareFillAbsIdx_,
                                  std::memory_order_release);
    const uint64_t primedFinalizedEnd = std::min<uint64_t>(
        softwareFillAbsIdx_,
        ASFW::Shared::Isoch::IsochQueueGeometry::kPayloadFinalityLeadPackets);
    // Prime makes these packets final exactly as a completion pass would, so it
    // owes them the same per-packet seal. Without it the first packets of a
    // stream are the one window where a producer could still win a race against
    // a decision already taken.
    RefillOutcome primeSeal{};
    for (uint64_t packetAbs = 0; packetAbs < primedFinalizedEnd; ++packetAbs) {
        auto& meta = metadataRing[static_cast<uint32_t>(packetAbs % numSlots)];
        if (meta.packetIndex != packetAbs) continue;
        SealOnArmedImage(meta,
                         ExpectedTxCommitGeneration(packetAbs, numSlots),
                         controlBlock, primeSeal);
    }
    controlBlock->finalizedEnd.store(primedFinalizedEnd,
                                     std::memory_order_release);

    stats.packetsAssembled = numPackets;
    ASFW_LOG(Isoch, "IT: Dynamic descriptor ring primed. numPackets=%u softwareFillIdx=%llu",
             numPackets, softwareFillAbsIdx_);
    return stats;
}

bool IsochTxDmaRing::DecodeHardwarePacketIndex(const uint32_t cmdPtr,
                                               uint32_t& outPacketIndex) noexcept {
    const uint32_t cmdAddr = cmdPtr & 0xFFFFFFF0u;

    uint32_t hwLogicalIndex = 0;
    if (!slab_.DecodeCmdAddrToLogicalIndex(cmdAddr, hwLogicalIndex)) {
        return false;
    }

    outPacketIndex = hwLogicalIndex / Layout::kBlocksPerPacket;
    return outPacketIndex < Layout::kNumPackets;
}

const char* IsochTxDmaRing::RefillFailureReasonName(
    RefillFailureReason reason) noexcept {
    switch (reason) {
        case RefillFailureReason::None:
            return "none";
        case RefillFailureReason::InvalidSharedContract:
            return "invalid-shared-contract";
        case RefillFailureReason::DeadContext:
            return "dead-context";
        case RefillFailureReason::ProducerFaultStatus:
            return "producer-fault-status";
        case RefillFailureReason::CommandPointerDecode:
            return "command-pointer-decode";
        case RefillFailureReason::UncommittedSlot:
            return "uncommitted-slot";
        case RefillFailureReason::InvalidPacketSize:
            return "invalid-packet-size";
        case RefillFailureReason::PayloadMapping:
            return "payload-mapping";
        case RefillFailureReason::PayloadSealMismatch:
            return "payload-seal-mismatch";
    }
    return "unknown";
}

IsochTxDmaRing::RefillOutcome IsochTxDmaRing::Refill(
    Driver::HardwareInterface& hw,
    uint8_t contextIndex,
    IsochTxPacketMeta* metadataRing,
    IsochTxQueueControl* controlBlock,
    uint32_t numSlots,
    uint8_t* payloadBase,
    const TxPayloadDmaMap& payloadDmaMap,
    uint64_t eventTicks, uint32_t source) noexcept
{
    counters_.calls.fetch_add(1, std::memory_order_relaxed);
    RefillOutcome out{};

    if (!metadataRing || !controlBlock || !payloadBase ||
        !payloadDmaMap.IsValid() ||
        numSlots == 0 || numSlots != controlBlock->numSlots ||
        controlBlock->slotStrideBytes == 0 ||
        controlBlock->maxPacketBytes == 0 ||
        controlBlock->maxPacketBytes > controlBlock->slotStrideBytes) {
        out.failureReason = RefillFailureReason::InvalidSharedContract;
        return out;
    }

    // 1. Snapshot controller state as one short MMIO batch. Descriptor
    // preparation must not retain MMIO permission or trigger nested scopes.
    const Register32 ctrlReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(contextIndex));
    const Register32 cmdPtrReg = static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(contextIndex));
    uint32_t refillCycleTimer = 0;
    uint32_t ctrl = 0;
    uint32_t cmdPtr = 0;
    uint64_t cycleReadBeforeHostTicks = 0;
    uint64_t cycleReadAfterHostTicks = 0;
    {
        auto access = hw.TryBeginAccess();
        if (!access) {
            out.failureReason = RefillFailureReason::InvalidSharedContract;
            return out;
        }
        cycleReadBeforeHostTicks = mach_absolute_time();
        refillCycleTimer = access.Read(Register32::kCycleTimer);
        cycleReadAfterHostTicks = mach_absolute_time();
        ctrl = access.Read(ctrlReg);
        cmdPtr = access.Read(cmdPtrReg);
    }

#if ASFW_TX_FLIGHT_RECORDER
    TxRefillRecord capture{};
    capture.epoch = captureEpoch_;
    capture.eventTicks = eventTicks;
    capture.source = source;
    capture.readBeforeTicks = cycleReadBeforeHostTicks;
    capture.readAfterTicks = cycleReadAfterHostTicks;
    capture.cycle = refillCycleTimer;
    capture.previousCycle = lastObservationCycleTimer_;
    capture.previousSlot = lastHwPacketIndex_;
    capture.command = cmdPtr;
    capture.control = ctrl;
    capture.completionBefore = controlBlock->completionCursor.load(std::memory_order_relaxed);
    capture.committedBefore = controlBlock->committedEnd.load(std::memory_order_acquire);
    capture.mappedBefore = softwareFillAbsIdx_;
    const bool previousValid = lastObservationCycleTimerValid_;
    // Runs on every exit after the MMIO snapshot, including a failed seal.
    // Freeze only after the outcome is filled, before the caller can recover.
    struct CaptureOnExit {
        TxRefillFlightRecorder& recorder;
        TxRefillRecord& record;
        const RefillOutcome& outcome;
        bool previousValid;
        ~CaptureOnExit() {
            record.slot = outcome.hwPacketIndex;
            record.inferredDelta = outcome.completedPacketCount;
            record.filled = static_cast<uint32_t>(outcome.packetsFilled);
            record.failure = static_cast<uint32_t>(outcome.failureReason);
            record.failedPacket = outcome.failurePacketAbs;
            record.expectedSeal = outcome.failureExpectedPayloadSeal;
            record.observedSeal = outcome.failureObservedPayloadSeal;
            if (previousValid) record.flags |= 1;
            if (record.inferredDelta >= Layout::kNumPackets) record.flags |= 2;
            // A gap of a ring or more admits ambiguous modulo progress. Freeze
            // even if the current estimator happens to return a small delta.
            if (previousValid && Tx::CyclesBetween(record.previousCycle, record.cycle)
                    >= Layout::kNumPackets) record.flags |= 4;
            recorder.Record(record, record.failure != 0 || (record.flags & 6) != 0);
        }
    } captureOnExit{flightRecorder_, capture, out, previousValid};
#endif

    // Publish the raw controller/host pair. Bracketing only the cycle-timer
    // load provides a true midpoint without widening the MMIO batch. Any
    // smoothing or projection belongs to the content producer's clock domain.
    {
        const uint64_t hostTime = cycleReadAfterHostTicks >=
                cycleReadBeforeHostTicks
            ? cycleReadBeforeHostTicks +
                (cycleReadAfterHostTicks - cycleReadBeforeHostTicks) / 2
            : cycleReadAfterHostTicks;

        IsochTxClockPairSample sample{};
        sample.hostTimeMid = hostTime;
        sample.cycleTimer32 = refillCycleTimer;
        controlBlock->clockPair.Publish(sample);
    }

    // 2. Check context status
    out.contextControl = ctrl;
    const bool dead = (ctrl & Driver::ContextControl::kDead) != 0;
    if (dead) {
        counters_.exitDead.fetch_add(1, std::memory_order_relaxed);
        controlBlock->statusWord.store(IsochTxQueueStatus::kDeadContext, std::memory_order_release);
        controlBlock->streamGeneration.fetch_add(1, std::memory_order_release);
        out.dead = true;
        out.failureReason = RefillFailureReason::DeadContext;
        out.streamStatus = static_cast<uint32_t>(
            IsochTxQueueStatus::kDeadContext);
        return out;
    }
    const auto streamStatus =
        controlBlock->statusWord.load(std::memory_order_acquire);
    out.streamStatus = static_cast<uint32_t>(streamStatus);
    if (streamStatus ==
        IsochTxQueueStatus::kProducerFault) {
        out.failureReason = RefillFailureReason::ProducerFaultStatus;
        return out;
    }

    // 3. Decode hardware pointer and advance completed cursor
    uint32_t hwPacketIndex = 0;
    if (!DecodeHardwarePacketIndex(cmdPtr, hwPacketIndex)) {
        counters_.exitDecodeFail.fetch_add(1, std::memory_order_relaxed);
        out.decodeFailed = true;
        out.failureReason = RefillFailureReason::CommandPointerDecode;
        out.cmdPtr = cmdPtr;
        out.cmdAddr = cmdPtr & 0xFFFFFFF0u;
        return out;
    }

    out.hwPacketIndex = hwPacketIndex;
    out.cmdPtr = cmdPtr;
    out.cmdAddr = cmdPtr & 0xFFFFFFF0u;

    // One-shot: how far the controller had already gone when software first
    // looked. Accumulating deltas assumes this is under one lap, and nothing in
    // the descriptor program enforces that -- the ring branches from its last
    // packet back to its first, so an unrefilled context re-transmits the same
    // 48 packets indefinitely. Laps missed here are not a counting error alone:
    // that stale audio went on the wire, and the client's content followed it
    // 288 frames per lap later than planned.
    if (!startLapObserved_) {
        startLapObserved_ = true;
        const uint32_t startCycleTimer =
            controlBlock->startCycleMatch.load(std::memory_order_acquire);
        if (startCycleTimer != 0) {
            const uint32_t elapsedCycles =
                Tx::CyclesBetween(startCycleTimer, refillCycleTimer);
            const uint64_t lifted = Tx::LiftRingSlotToAbsolute(
                hwPacketIndex, elapsedCycles, Layout::kNumPackets);
            const uint64_t lapsLost = lifted / Layout::kNumPackets;
            // The lift infers descriptor progress from elapsed cycles, which
            // holds only while the context advanced one packet per cycle. The
            // self-linked skip address means it may not have: report the raw
            // observations, mark the inference, and state the bound under which
            // it is exact. See TxPacketIndexLift.hpp.
            ASFW_LOG(Isoch,
                     "[TxLapSeed] slot=%u elapsedCycles=%u naive=%u liftedEst=%llu lapsLostEst=%llu elapsedUs=%u exactIfSkippedCyclesUnder=%u",
                     hwPacketIndex, elapsedCycles, hwPacketIndex, lifted,
                     lapsLost, elapsedCycles * 125u,
                     Layout::kNumPackets / 2u);
        } else {
            ASFW_LOG(Isoch,
                     "[TxLapSeed] slot=%u no start anchor; lap unresolvable",
                     hwPacketIndex);
        }
    }

    const uint32_t deltaConsumed =
        ComputeDeltaConsumed(hwPacketIndex, refillCycleTimer);
    out.completedPacketCount = deltaConsumed;
    const uint32_t gap = ringPacketsAhead_;
    UpdateGapCounters(gap);
    ResyncCycleTracking(hw, hwPacketIndex, deltaConsumed, out);

    // Publish a neutral completion-delta high-water. Content consumers decide
    // whether their own frame/lead policy can tolerate the observed cadence.
    {
        uint32_t prevMax =
            counters_.maxDeltaConsumed.load(std::memory_order_relaxed);
        while (deltaConsumed > prevMax &&
               !counters_.maxDeltaConsumed.compare_exchange_weak(
                   prevMax, deltaConsumed, std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
        if (deltaConsumed > prevMax) {
            controlBlock->maxCompletionDelta.store(
                deltaConsumed, std::memory_order_release);
            controlBlock->maxCompletionDeltaEvents.fetch_add(
                1, std::memory_order_relaxed);
            ASFW_LOG_RING_ONLY(
                Isoch,
                ::ASFW::Logging::LogLevel::Notice,
                "IT completion delta high-water=%u",
                deltaConsumed);
        }
    }

    // Fetch and publish completed stamps
    const uint64_t completedAbsIdx = controlBlock->completionCursor.load(std::memory_order_relaxed);

    // The walk returns shared-slot ownership for packets the controller has
    // finished with, so it may only inspect slots whose metadata still
    // describes the packet being retired. A slot can be retired once per lap:
    // if `deltaConsumed` exceeds the descriptor ring, the controller lapped and
    // everything older than the last `kNumPackets` had its slot recycled a lap
    // ago. Verifying those compares a current payload against stale metadata and
    // reports a seal mismatch that is an artefact of the walk, not a producer
    // fault -- observed on hardware 2026-09-06 as `[TxPayloadSeal] FATAL
    // packet=237852` two records after a `lapsLost=1` recovery, which then
    // fatal-stopped a healthy stream.
    //
    // Walk the most recent `kNumPackets` instead and account for the rest as
    // abandoned. The cursor still advances by the *full* recovered delta below:
    // the lap really did happen, and hiding it is what put 288 frames of latency
    // into every later packet. Only the inspection is bounded, never the truth.
    const auto walkSpan =
        Tx::SplitCompletionWalk(deltaConsumed, Layout::kNumPackets);
    const uint32_t abandonedPackets = walkSpan.abandoned;
    if (abandonedPackets != 0) {
        counters_.abandonedOnLap.fetch_add(abandonedPackets,
                                           std::memory_order_relaxed);
        // Loud: this is real content the controller transmitted from recycled
        // descriptors, i.e. stale audio that went on the wire. Recoverable, but
        // never silent.
        ASFW_LOG_ERROR(
            Isoch,
            "[TxLapAbandon] delta=%u ring=%u abandoned=%u firstAbs=%llu "
            "resumeAbs=%llu -- the controller lapped the software fill; these "
            "packets transmitted stale descriptors and their slots are gone",
            deltaConsumed, Layout::kNumPackets, abandonedPackets,
            completedAbsIdx, completedAbsIdx + abandonedPackets);
    }
    for (uint32_t i = abandonedPackets; i < deltaConsumed; ++i) {
        const uint64_t currentAbsIdx = completedAbsIdx + i;
        const uint32_t completedPktSlot = static_cast<uint32_t>(currentAbsIdx % Layout::kNumPackets);

        // The producer cannot reuse this shared slot until completionCursor is
        // published below. Verify the opaque payload still matches its release-
        // commit seal before returning ownership. This catches the exact class
        // of any producer that mutates a payload after release commit.
        const uint32_t producerSlot =
            static_cast<uint32_t>(currentAbsIdx % numSlots);
        const auto& completedMeta = metadataRing[producerSlot];
        const uint32_t completedLength = completedMeta.payloadLength;
        if (completedLength > controlBlock->maxPacketBytes) {
            counters_.fatalPacketSize.fetch_add(
                1, std::memory_order_relaxed);
            controlBlock->statusWord.store(
                IsochTxQueueStatus::kProducerFault,
                std::memory_order_release);
            out.failureReason = RefillFailureReason::InvalidPacketSize;
            out.failurePacketAbs = currentAbsIdx;
            out.failureSlot = producerSlot;
            out.failurePayloadLength = completedLength;
            return out;
        }
        const uint8_t* completedPayload =
            payloadBase + TxPayloadImageOffset(
                              producerSlot,
                              completedMeta.selectedPayloadImage,
                              controlBlock->slotStrideBytes);
        const uint64_t observedSeal =
            ASFW::Shared::Isoch::SealTxPayload(
                completedPayload, completedLength);
        if (observedSeal != completedMeta.payloadSeal) {
            counters_.fatalPayloadSealMismatch.fetch_add(
                1, std::memory_order_relaxed);
            controlBlock->statusWord.store(
                IsochTxQueueStatus::kProducerFault,
                std::memory_order_release);
            controlBlock->streamGeneration.fetch_add(
                1, std::memory_order_release);
            out.failureReason =
                RefillFailureReason::PayloadSealMismatch;
            out.failurePacketAbs = currentAbsIdx;
            out.failureSlot = producerSlot;
            out.failurePayloadLength = completedLength;
            out.failureExpectedPayloadSeal = completedMeta.payloadSeal;
            out.failureObservedPayloadSeal = observedSeal;
            ASFW_LOG_ERROR(
                Isoch,
                "[TxPayloadSeal] FATAL packet=%llu slot=%u len=%u expected=0x%016llx observed=0x%016llx committed=%llu completion=%llu",
                currentAbsIdx,
                producerSlot,
                completedLength,
                completedMeta.payloadSeal,
                observedSeal,
                controlBlock->committedEnd.load(std::memory_order_acquire),
                completedAbsIdx);
            return out;
        }

        auto* desc2 = slab_.GetDescriptorPtr(
            completedPktSlot * Layout::kBlocksPerPacket +
            Layout::kCompletionBlock);
        if (dmaMemory_) {
            dmaMemory_->FetchFromDevice(reinterpret_cast<const std::byte*>(desc2), sizeof(*desc2));
        }

        const uint16_t hwTimestamp =
            static_cast<uint16_t>(desc2->statusWord & 0xFFFF);

        // OHCI OUTPUT_LAST reports sec[2:0]:cycle[12:0] and omits the
        // intra-cycle offset. Reconstruct the packet cycle at offset zero.
        const uint32_t completionCycleTimer =
            (static_cast<uint32_t>((hwTimestamp >> 13) & 0x7u) << 25) |
            (static_cast<uint32_t>(hwTimestamp & 0x1FFFu) << 12);
        controlBlock->PushCompletionStamp(currentAbsIdx,
                                          completionCycleTimer);
    }
    if (deltaConsumed > 0) {
        controlBlock->completionCursor.store(completedAbsIdx + deltaConsumed, std::memory_order_release);

        const uint64_t requested =
            controlBlock->refillRequestGeneration.load(
                std::memory_order_relaxed);
        const uint64_t handled =
            controlBlock->refillHandledGeneration.load(
                std::memory_order_acquire);
        if (requested == handled) {
            const uint64_t generation = requested + 1;
            controlBlock->refillRequestHostTicks.store(
                mach_absolute_time(), std::memory_order_relaxed);
            controlBlock->refillRequestGeneration.store(
                generation, std::memory_order_release);
            controlBlock->refillRequestCount.fetch_add(
                1, std::memory_order_relaxed);
            out.refillRequestGeneration = generation;
        } else {
            controlBlock->refillCoalescedCount.fetch_add(
                1, std::memory_order_relaxed);
        }
    }

    // Refresh already-bound payload choices before recycling completed
    // descriptors. The absolute command position is the completed cursor after
    // this batch; modulo descriptor indices alone cannot distinguish laps.
    RefreshLatePayloadBindings(
        hw,
        contextIndex,
        completedAbsIdx + deltaConsumed,
        refillCycleTimer,
        metadataRing,
        controlBlock,
        numSlots,
        payloadBase,
        payloadDmaMap,
        out);

    // 4. Refill batch: try to fill deltaConsumed slots
    // If softwareFillAbsIdx_ is 0, initialize it from the completedAbsIdx + ringPacketsAhead_
    if (softwareFillAbsIdx_ == 0) {
        softwareFillAbsIdx_ = completedAbsIdx + ringPacketsAhead_;
    }

    uint32_t packetsFilled = 0;
    for (uint32_t i = 0; i < deltaConsumed; ++i) {
        const uint64_t fillAbsIdx = softwareFillAbsIdx_ + i;
        const uint32_t pktSlot = static_cast<uint32_t>(fillAbsIdx % numSlots);

        auto& meta = metadataRing[pktSlot];
        const uint64_t expectedGen = ExpectedTxCommitGeneration(fillAbsIdx, numSlots);
        const uint64_t commitGen =
            meta.commitGeneration.load(std::memory_order_acquire);

        if (commitGen != expectedGen) {
            const uint64_t requestGeneration =
                controlBlock->refillRequestGeneration.load(
                    std::memory_order_acquire);
            const uint64_t handledGeneration =
                controlBlock->refillHandledGeneration.load(
                    std::memory_order_acquire);
            const uint64_t requestHostTicks =
                controlBlock->refillRequestHostTicks.load(
                    std::memory_order_relaxed);
            const uint64_t nowHostTicks = mach_absolute_time();
            const uint64_t requestAgeUs =
                requestHostTicks != 0 && nowHostTicks >= requestHostTicks
                    ? ASFW::Timing::hostTicksToNanos(
                          nowHostTicks - requestHostTicks) /
                          1000
                    : 0;
            counters_.txUnderruns.fetch_add(1, std::memory_order_relaxed);
            controlBlock->statusWord.store(
                IsochTxQueueStatus::kProducerFault,
                std::memory_order_release);
            controlBlock->streamGeneration.fetch_add(
                1, std::memory_order_release);
            ASFW_LOG(
                Isoch,
                "IT FATAL: slot %u not committed packet=%llu commitGen=%llu expectedGen=%llu",
                pktSlot,
                fillAbsIdx,
                commitGen,
                expectedGen);
            // Refill-coverage post-mortem: was this absolute packet ever
            // prepared into its slot, or does the slot still hold a previous
            // lap's packet? meta.packetIndex == fillAbsIdx with a stale
            // commitGen => commit/writeback ordering bug; meta.packetIndex one
            // lap behind => the producer's committed cursor never reached this
            // packet (coverage/margin). The producer cursors localize it.
            ASFW_LOG(
                Isoch,
                "IT FATAL dump: fatalAbs=%llu slot=%u expectedGen=%llu commitGen=%llu "
                "slotLastPacketAbs=%llu committedEnd=%llu completionCursor=%llu "
                "softwareFillAbs=%llu ringPacketsAhead=%u deltaConsumed=%u i=%u numSlots=%u "
                "prepReq=%llu prepHandled=%llu prepAgeUs=%llu prepCoalesced=%llu",
                fillAbsIdx,
                pktSlot,
                expectedGen,
                commitGen,
                meta.packetIndex,
                controlBlock->committedEnd.load(std::memory_order_acquire),
                controlBlock->completionCursor.load(std::memory_order_acquire),
                softwareFillAbsIdx_,
                ringPacketsAhead_,
                deltaConsumed,
                i,
                numSlots,
                requestGeneration,
                handledGeneration,
                requestAgeUs,
                controlBlock->refillCoalescedCount.load(
                    std::memory_order_relaxed));
            out.failureReason = RefillFailureReason::UncommittedSlot;
            out.failurePacketAbs = fillAbsIdx;
            out.failureSlot = pktSlot;
            return out;
        }

        const uint32_t hwSlot = static_cast<uint32_t>(fillAbsIdx % Layout::kNumPackets);
        uint64_t payloadOffset = 0;  // assigned once the image is selected

        const uint32_t payloadLength = meta.payloadLength;

        if (payloadLength != 0 && (payloadLength < 2 ||
            payloadLength > controlBlock->maxPacketBytes)) {
            counters_.fatalPacketSize.fetch_add(1, std::memory_order_relaxed);
            out.failureReason = RefillFailureReason::InvalidPacketSize;
            out.failurePacketAbs = fillAbsIdx;
            out.failureSlot = pktSlot;
            out.failurePayloadLength = payloadLength;
            ASFW_LOG(
                Isoch,
                "IT FATAL: invalid payload size packet=%llu slot=%u len=%u max=%u stride=%u",
                fillAbsIdx,
                pktSlot,
                payloadLength,
                controlBlock->maxPacketBytes,
                controlBlock->slotStrideBytes);
            return out;
        }

        std::array<TxPayloadDmaFragment, 2> payloadFragments{};
        // Initial binding chooses the best complete image currently published.
        // Binding is descriptor ownership, not payload finality: a later
        // completion pass may still repoint the mutable tail while the command
        // remains outside the live-command guard.
        const uint32_t selectedImage = SelectPayloadImage(meta, expectedGen);
        meta.selectedPayloadImage = static_cast<uint16_t>(selectedImage);
        payloadOffset = TxPayloadImageOffset(
            pktSlot, selectedImage, controlBlock->slotStrideBytes);
        meta.payloadSeal = ASFW::Shared::Isoch::SealTxPayload(
            payloadBase + payloadOffset, payloadLength);
        if (!payloadDmaMap.ResolveTwoFragments(
                payloadOffset, payloadLength, payloadFragments,
                meta.payloadPrefixBytes)) {
            counters_.fatalPayloadMapping.fetch_add(1, std::memory_order_relaxed);
            ASFW_LOG(
                Isoch,
                "IT: Refill payload mapping failed packet=%u slot=%u offset=%llu len=%u segments=%zu",
                hwSlot,
                pktSlot,
                payloadOffset,
                meta.payloadLength,
                payloadDmaMap.SegmentCount());
            out.failureReason = RefillFailureReason::PayloadMapping;
            out.failurePacketAbs = fillAbsIdx;
            out.failureSlot = pktSlot;
            out.failurePayloadLength = payloadLength;
            return out;
        }

        // Linux queue_iso_transmit() writes this pair through (__le32 *)&d[1]:
        // offsets 0x10/0x14 after the OMI command descriptor. Offsets 0x08/0x0c
        // are the cycle-loss skip address and command status, not transmitted
        // data. Cross-validated with Linux: firewire/ohci.c:3373-3383.
        const uint32_t descBase = hwSlot * Layout::kBlocksPerPacket;
        auto* immDesc = reinterpret_cast<OHCIDescriptorImmediate*>(
            slab_.GetDescriptorPtr(descBase));
        immDesc->immediateData[0] =
            StampHeaderChannel(meta.immediateHeader[0], channel_);
        immDesc->immediateData[1] = meta.immediateHeader[1];
        immDesc->common.branchWord = MakeBranchWordAT(
            slab_.GetDescriptorIOVA(descBase),
            Layout::kBlocksPerPacket);

        auto* desc2 =
            slab_.GetDescriptorPtr(descBase + Layout::kFirstPayloadBlock);
        desc2->control = OHCIDescriptor::BuildControl({
            .reqCount = static_cast<uint16_t>(payloadFragments[0].length),
            .command = OHCIDescriptor::kCmdOutputMore,
            .key = OHCIDescriptor::kKeyStandard,
            .interruptBits = OHCIDescriptor::kIntNever,
            .branchBits = OHCIDescriptor::kBranchNever,
        });
        desc2->dataAddress = payloadFragments[0].deviceAddress;
        desc2->branchWord = 0;
        desc2->statusWord = 0;

        auto* desc3 =
            slab_.GetDescriptorPtr(descBase + Layout::kCompletionBlock);
        desc3->control = OHCIDescriptor::BuildControl({
            .reqCount = static_cast<uint16_t>(payloadFragments[1].length),
            .command = OHCIDescriptor::kCmdOutputLast,
            .key = OHCIDescriptor::kKeyStandard,
            .interruptBits =
                Core::IsTimingGroupBoundary(hwSlot)
                    ? OHCIDescriptor::kIntAlways
                    : OHCIDescriptor::kIntNever,
            .branchBits = OHCIDescriptor::kBranchAlways,
        });
        desc3->control |=
            (1u << (OHCIDescriptor::kStatusShift +
                    OHCIDescriptor::kControlHighShift));
        desc3->dataAddress = payloadFragments[1].deviceAddress;
        const uint32_t nextHwSlot = (hwSlot + 1) % Layout::kNumPackets;
        desc3->branchWord = MakeBranchWordAT(
            slab_.GetDescriptorIOVA(nextHwSlot * Layout::kBlocksPerPacket),
            Layout::kBlocksPerPacket);
        AR_init_status(
            *desc3, static_cast<uint16_t>(payloadFragments[1].length));

        // Publish the shared producer slot before exposing descriptor changes.
        if (dmaMemory_) {
            const auto* payloadSlot =
                reinterpret_cast<const std::byte*>(payloadBase + payloadOffset);
            dmaMemory_->PublishToDevice(payloadSlot, payloadLength);
            dmaMemory_->PublishBarrier();
            dmaMemory_->PublishToDevice(reinterpret_cast<const std::byte*>(immDesc), sizeof(OHCIDescriptorImmediate));
            dmaMemory_->PublishToDevice(reinterpret_cast<const std::byte*>(desc2), sizeof(OHCIDescriptor));
            dmaMemory_->PublishToDevice(reinterpret_cast<const std::byte*>(desc3), sizeof(OHCIDescriptor));
        }

        packetsFilled++;
    }

    if (packetsFilled > 0) {
        CommitRefill(packetsFilled);
        // Publish the freeze frontier after the descriptors are visible, so a
        // producer that sees the new value can never still be racing a bind it
        // was told had not happened.
        controlBlock->mappedEnd.store(softwareFillAbsIdx_,
                                      std::memory_order_release);
    }

    out.ok = true;
    out.packetsFilled = packetsFilled;
    return out;
}

bool IsochTxDmaRing::WakeHardwareIfIdle(Driver::HardwareInterface& hw,
                                       uint8_t contextIndex) noexcept {
    Register32 ctrlReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(contextIndex));
    auto access = hw.TryBeginAccess();
    if (!access) return false;
    const uint32_t ctrl = access.Read(ctrlReg);

    const bool run = (ctrl & Driver::ContextControl::kRun) != 0;
    const bool dead = (ctrl & Driver::ContextControl::kDead) != 0;
    const bool active = (ctrl & Driver::ContextControl::kActive) != 0;

    if (run && !dead && !active) {
        Register32 ctrlSetReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControlSet(contextIndex));
        // Cross-validated with Linux context queue/flush behavior:
        // firewire/ohci.c:1520-1525,3586-3592. Flush this anomaly-path write
        // so the single bounded recovery attempt is observable immediately.
        access.WriteAndFlush(ctrlSetReg, Driver::ContextControl::kWake);
        return true;
    }
    return false;
}

uint32_t IsochTxDmaRing::CompletionStatusBefore(
    const uint32_t hwPacketIndex) noexcept {
    if (!slab_.IsValid() || hwPacketIndex >= Layout::kNumPackets) {
        return 0;
    }
    const uint32_t completedPacket =
        (hwPacketIndex + Layout::kNumPackets - 1) % Layout::kNumPackets;
    auto* descriptor = slab_.GetDescriptorPtr(
        completedPacket * Layout::kBlocksPerPacket +
        Layout::kCompletionBlock);
    if (!descriptor) {
        return 0;
    }
    if (dmaMemory_) {
        dmaMemory_->FetchFromDevice(
            reinterpret_cast<const std::byte*>(descriptor),
            sizeof(*descriptor));
    }
    return descriptor->statusWord;
}

void IsochTxDmaRing::DumpAtCmdPtr(Driver::HardwareInterface& hw, uint8_t contextIndex) const noexcept {
#ifndef ASFW_HOST_TEST
    Register32 cmdPtrReg = static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(contextIndex));
    auto access = hw.TryBeginAccess();
    if (!access) return;
    const uint32_t cmdPtr = access.Read(cmdPtrReg);
    const uint32_t addr = cmdPtr & 0xFFFFFFF0u;
    const uint32_t z = cmdPtr & 0xF;

    const uint32_t base = static_cast<uint32_t>(slab_.DescriptorRegion().deviceBase);

    ASFW_LOG(Isoch, "IT: DumpAtCmdPtr: cmdPtr=0x%08x addr=0x%08x Z=%u (base=0x%08x)",
             cmdPtr, addr, z, base);

    uint32_t logicalIdx = 0;
    if (!slab_.DecodeCmdAddrToLogicalIndex(addr, logicalIdx)) {
        ASFW_LOG(Isoch, "IT: CmdPtr decode FAILED - addr=0x%08x outside ring or in padding", addr);
        return;
    }

    ASFW_LOG(Isoch, "IT: CmdPtr decoded to logicalIdx=%u (packet=%u, block=%u)",
             logicalIdx, logicalIdx / Layout::kBlocksPerPacket, logicalIdx % Layout::kBlocksPerPacket);

    for (uint32_t k = 0; k < 4 && (logicalIdx + k) < Layout::kRingBlocks; ++k) {
        const auto* b = slab_.GetDescriptorPtr(logicalIdx + k);
        ASFW_LOG(Isoch, "IT: @%u ctl=0x%08x dat=0x%08x br=0x%08x st=0x%08x",
                 logicalIdx + k, b->control, b->dataAddress, b->branchWord, b->statusWord);
    }
#endif
}

void IsochTxDmaRing::DumpDescriptorRing(uint32_t startPacket, uint32_t numPackets) const noexcept {
    const auto desc = slab_.DescriptorRegion();
    if (!desc.virtualBase) {
        ASFW_LOG(Isoch, "IT: DumpDescriptorRing - no descriptor ring allocated");
        return;
    }

    constexpr uint32_t totalPackets = Layout::kNumPackets;
    if (startPacket >= totalPackets) {
        ASFW_LOG(Isoch, "IT: DumpDescriptorRing - startPacket %u out of range (max=%u)",
                 startPacket, totalPackets - 1);
        return;
    }
    if (startPacket + numPackets > totalPackets) {
        numPackets = totalPackets - startPacket;
    }

    const uint32_t descBaseIOVA = static_cast<uint32_t>(desc.deviceBase);
    ASFW_LOG(Isoch, "IT: DescRing Dump pkts %u-%u (total=%u pages=%u) DescBase=0x%08x Z=%u",
             startPacket, startPacket + numPackets - 1, totalPackets, Layout::kTotalPages,
             descBaseIOVA, Layout::kBlocksPerPacket);

    for (uint32_t pktIdx = startPacket; pktIdx < startPacket + numPackets; ++pktIdx) {
        const uint32_t descBase = pktIdx * Layout::kBlocksPerPacket;

        auto* desc0 = slab_.GetDescriptorPtr(descBase);
        auto* immDesc = reinterpret_cast<const OHCIDescriptorImmediate*>(desc0);
        const uint32_t ctl0 = desc0->control;
        const uint32_t i0 = (ctl0 >> 18) & 0x3;
        const uint32_t b0 = (ctl0 >> 16) & 0x3;

        const uint32_t skipAddr = immDesc->common.branchWord & 0xFFFFFFF0u;
        const uint32_t skipZ = immDesc->common.branchWord & 0xF;
        const uint32_t itQ0 = immDesc->immediateData[0];
        const uint32_t itQ1 = immDesc->immediateData[1];

        const uint32_t spd = (itQ0 >> 16) & 0x7;
        const uint32_t tag = (itQ0 >> 14) & 0x3;
        const uint32_t chan = (itQ0 >> 8) & 0x3F;
        const uint32_t tcode = (itQ0 >> 4) & 0xF;
        const uint32_t sy = itQ0 & 0xF;
        const uint32_t dataLen = (itQ1 >> 16) & 0xFFFF;

        auto* desc2 =
            slab_.GetDescriptorPtr(descBase + Layout::kFirstPayloadBlock);
        const uint32_t ctl1 = desc2->control;
        const uint32_t reqCount1 = ctl1 & 0xFFFF;

        auto* desc3 =
            slab_.GetDescriptorPtr(descBase + Layout::kCompletionBlock);
        const uint32_t ctl2 = desc3->control;
        const uint32_t i2 = (ctl2 >> 18) & 0x3;
        const uint32_t b2 = (ctl2 >> 16) & 0x3;
        const uint32_t reqCount2 = ctl2 & 0xFFFF;
        const uint32_t branchAddr = desc3->branchWord & 0xFFFFFFF0u;
        const uint32_t branchZ = desc3->branchWord & 0xF;
        const uint16_t xferStatus =
            static_cast<uint16_t>(desc3->statusWord >> 16);

        const uint32_t computedIOVA = slab_.GetDescriptorIOVA(descBase);

        ASFW_LOG(Isoch, "  Pkt[%u] @desc%u IOVA=0x%08x OMI: ctl=0x%08x i=%u b=%u skip=0x%08x|%u Q0=0x%08x(spd=%u tag=%u ch=%u tcode=0x%x sy=%u) Q1=0x%08x(len=%u)",
                 pktIdx, descBase, computedIOVA, ctl0, i0, b0, skipAddr, skipZ,
                 itQ0, spd, tag, chan, tcode, sy,
                 itQ1, dataLen);
        ASFW_LOG(Isoch,
                 "         OM:  ctl=0x%08x req=%u data=0x%08x",
                 ctl1,
                 reqCount1,
                 desc2->dataAddress);
        ASFW_LOG(Isoch,
                 "         OL:  ctl=0x%08x i=%u b=%u req=%u data=0x%08x br=0x%08x|%u st=0x%04x",
                 ctl2,
                 i2,
                 b2,
                 reqCount2,
                 desc3->dataAddress,
                 branchAddr,
                 branchZ,
                 xferStatus);
    }
}

} // namespace ASFW::Isoch::Tx

namespace ASFW::Isoch::Tx {
void IsochTxDmaRing::ExportFrozenRefills(uint8_t context) const noexcept {
    // Watchdog diagnostic phase only, never refill/IRQ. Split lines stay below
    // LogRing's 232-byte payload cap, including maximum-width numeric values.
    (void)flightRecorder_.ExportOnce([&](uint32_t i, uint32_t count, const TxRefillRecord& r) {
        ASFW_LOG(Isoch, "[TxFlightA] ctx=%u i=%u/%u ep=%llu src=%u event=%llu before=%llu after=%llu",
                 context, i, count, r.epoch, r.source, r.eventTicks, r.readBeforeTicks, r.readAfterTicks);
        ASFW_LOG(Isoch, "[TxFlightB] ctx=%u i=%u cycle=%08x prev=%08x cmd=%08x ctrl=%08x slots=%u/%u delta=%u fill=%u flags=%u fail=%u",
                 context, i, r.cycle, r.previousCycle, r.command, r.control,
                 r.previousSlot, r.slot, r.inferredDelta, r.filled, r.flags, r.failure);
        ASFW_LOG(Isoch, "[TxFlightC] ctx=%u i=%u completed=%llu mapped=%llu committed=%llu failed=%llu seals=%016llx/%016llx",
                 context, i, r.completionBefore, r.mappedBefore, r.committedBefore,
                 r.failedPacket, r.expectedSeal, r.observedSeal);
    });
}
}
