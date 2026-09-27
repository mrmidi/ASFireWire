// IsochTxDmaRing.cpp

#include "IsochTxDmaRing.hpp"

#include "../../Common/TimingUtils.hpp"

#include <algorithm>
#include <DriverKit/IOLib.h>

namespace ASFW::Isoch::Tx {

using namespace ASFW::Async::HW;
using namespace ASFW::Driver;

namespace {
// Replace the channel field [13:8] and the speed field [18:16] of a little-endian
// OHCI isoch transmit header quadlet with the values owned by this ring. Linux
// queue_iso_transmit() likewise takes both from the isoch context, never from
// content-layer metadata: the channel at ohci.c:3373-3381 and the speed from
// the context's own speed at ohci.c:3377. Speed is a property of the link to
// the device, which only the transport knows; the audio producer writes a
// placeholder it cannot resolve.
[[nodiscard]] inline uint32_t StampHeaderChannelAndSpeed(uint32_t leHeader, uint8_t channel,
                                                         FW::FwSpeed speed) noexcept {
    // An all-zero header is the "no packet" sentinel (e.g. underrun): leave it
    // untouched so the ring never invents packet state.
    if (leHeader == 0) {
        return 0;
    }
    uint32_t h = OSSwapLittleToHostInt32(leHeader);
    h = (h & ~(static_cast<uint32_t>(0x3F) << 8)) |
        (static_cast<uint32_t>(channel & 0x3F) << 8);
    h = (h & ~(static_cast<uint32_t>(0x7) << 16)) |
        ((static_cast<uint32_t>(speed) & 0x7U) << 16);
    return OSSwapHostToLittleInt32(h);
}
} // namespace

void IsochTxDmaRing::ResetForStart() noexcept {
    softwareFillAbsIdx_ = 0;

    nextTransmitCycle_ = 0;
    cycleTrackingValid_ = false;
    lastHwTimestamp_ = 0;

    counters_.lastDmaGapPackets.store(Layout::kNumPackets, std::memory_order_relaxed);
    counters_.minDmaGapPackets.store(Layout::kNumPackets, std::memory_order_relaxed);
    counters_.maxDeltaConsumed.store(0, std::memory_order_relaxed);
    counters_.criticalGapEvents.store(0, std::memory_order_relaxed);
    // Per stream, like the gap counters: the IT context outlives devices, so
    // a stale value would report an earlier stream's fault.
    counters_.mappedRegionExhausted.store(0, std::memory_order_relaxed);
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

// Completion is read from the descriptors the controller wrote, not derived
// from where its CommandPtr points. The CommandPtr names a slot inside the
// ring, so the difference of two readings cannot carry a lap: every scheme
// that restores one from elapsed cycles is an estimate (the lap detector this
// replaces, and midi's c4dd1700 before it).
//
// OHCI writes xferStatus into an OUTPUT_LAST as it retires that packet, and
// every (re)program zeroes it (AR_init_status), so a non-zero status means
// "finished since software last mapped it". Behaviour cross-checked with
// Linux, which walks transfer_status the same way
// (references/linux-ohci-firewire-low-level-stack/ohci.c:2918-2922) and sets
// the status-update bit on every IT OUTPUT_LAST (ohci.c:3302-3306); the
// completion block here sets the same bit.
//
// The walk stops at `mappedLimit`: past the mapped end a slot still holds
// the status of the packet it carried one ring earlier.
uint32_t IsochTxDmaRing::CountCompletedPackets(const uint64_t completedAbsIdx,
                                               const uint64_t mappedLimit) noexcept {
    uint32_t completed = 0;
    while (completed < mappedLimit) {
        const uint32_t slot = static_cast<uint32_t>(
            (completedAbsIdx + completed) % Layout::kNumPackets);
        auto* completionDesc = slab_.GetDescriptorPtr(
            slot * Layout::kBlocksPerPacket + Layout::kCompletionBlock);
        if (dmaMemory_) {
            dmaMemory_->FetchFromDevice(
                reinterpret_cast<const std::byte*>(completionDesc),
                sizeof(*completionDesc));
        }
        // xferStatus is the high half; the low half is the transmit
        // timestamp, meaningless until status is written.
        if ((completionDesc->statusWord >> 16) == 0) {
            break;
        }
        ++completed;
    }
    return completed;
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

void IsochTxDmaRing::ResyncCycleTracking(const uint32_t newestCompletedSlot,
                                         const uint32_t completedCount,
                                         RefillOutcome& out) noexcept {
    if (completedCount == 0 || !cycleTrackingValid_) {
        return;
    }

    out.completedPacketIndex = newestCompletedSlot;
    out.completedPacketCount = completedCount;
    auto* processedOL = slab_.GetDescriptorPtr(
        newestCompletedSlot * Layout::kBlocksPerPacket +
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
        (softwareFillIndex + Layout::kNumPackets - newestCompletedSlot) % Layout::kNumPackets;
    nextTransmitCycle_ = (hwCycle + aheadCount) % 8000;
}

void IsochTxDmaRing::CommitRefill(const uint32_t toFill) noexcept {
    softwareFillAbsIdx_ += toFill;

    std::atomic_thread_fence(std::memory_order_release);
    ASFW::Driver::WriteBarrier();

    counters_.packetsRefilled.fetch_add(toFill, std::memory_order_relaxed);
}

IsochTxDmaRing::PrimeStats IsochTxDmaRing::Prime(
    const TxPayloadDmaMap& payloadDmaMap,
    const uint32_t numSlots,
    const uint32_t slotStrideBytes,
    const IsochTxPacketMeta* metadataRing,
    const uint64_t preFillCount) noexcept {
    PrimeStats stats{};
    if (!slab_.IsValid()) {
        ASFW_LOG(Isoch, "IT: Prime failed - descriptor slab is invalid");
        return stats;
    }
    if (!payloadDmaMap.IsValid() ||
        numSlots == 0 || slotStrideBytes == 0 || metadataRing == nullptr) {
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
        const auto& meta = metadataRing[producerSlot];
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
        const uint64_t payloadOffset =
            static_cast<uint64_t>(producerSlot) * slotStrideBytes;
        if (!payloadDmaMap.ResolveTwoFragments(
                payloadOffset, meta.payloadLength, payloadFragments)) {
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
            StampHeaderChannelAndSpeed(meta.immediateHeader[0], channel_, speed_);
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

        // Finite queue: the last primed packet ends the chain. Refill links
        // each new batch to it, so the hardware stops at the mapped end
        // instead of replaying the head (Linux context_append leaves the new
        // tail zero and links it after publication, ohci.c:1105-1107,
        // 1137-1141).
        desc3->branchWord = pktIdx + 1 < numPackets
            ? MakeBranchWordAT(
                  slab_.GetDescriptorIOVA((pktIdx + 1) * Layout::kBlocksPerPacket),
                  Layout::kBlocksPerPacket)
            : 0;
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
        case RefillFailureReason::MappedRegionExhausted:
            return "mapped-region-exhausted";
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
    const TxPayloadDmaMap& payloadDmaMap) noexcept
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
    {
        auto access = hw.TryBeginAccess();
        if (!access) {
            out.failureReason = RefillFailureReason::InvalidSharedContract;
            return out;
        }
        refillCycleTimer = access.Read(Register32::kCycleTimer);
        ctrl = access.Read(ctrlReg);
        cmdPtr = access.Read(cmdPtrReg);
    }

    // Clock-smoothing / filtering is handled natively by AudioDriverKit's clock algorithms.
    {
        const uint64_t hostTime = mach_absolute_time();

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

    // Completion comes from the descriptors, not the CommandPtr (see
    // CountCompletedPackets): a late refill is not ambiguous, it just finds
    // more finished descriptors.
    const uint64_t completedAbsIdx =
        controlBlock->completionCursor.load(std::memory_order_relaxed);
    if (completedAbsIdx > softwareFillAbsIdx_ ||
        softwareFillAbsIdx_ - completedAbsIdx > Layout::kNumPackets) {
        // The completion cursor cannot lead the mapping or trail it by more
        // than a ring: nothing may be retired against a frontier that does
        // not exist.
        counters_.mappedRegionExhausted.fetch_add(1, std::memory_order_relaxed);
        out.failureReason = RefillFailureReason::MappedRegionExhausted;
        out.failurePacketAbs = completedAbsIdx;
        return out;
    }
    const uint64_t mappedLimit = softwareFillAbsIdx_ - completedAbsIdx;
    const uint32_t observedCompleted = CountCompletedPackets(completedAbsIdx, mappedLimit);
    UpdateGapCounters(static_cast<uint32_t>(mappedLimit - observedCompleted));
    if (mappedLimit == 0 || observedCompleted == mappedLimit) {
        // The finite queue ran dry: the hardware stopped at the mapped end,
        // and packets mapped from here would go out later than their cycles.
        // Leave the descriptors alone; the caller stops the stream and a
        // restart recovers. This does not tell a late refill from a late
        // producer.
        counters_.mappedRegionExhausted.fetch_add(1, std::memory_order_relaxed);
        out.failureReason = RefillFailureReason::MappedRegionExhausted;
        out.failurePacketAbs = softwareFillAbsIdx_;
        ASFW_LOG(Isoch,
                 "IT FATAL: mapped region exhausted completion=%llu mappedEnd=%llu hwPacket=%u",
                 completedAbsIdx, softwareFillAbsIdx_, hwPacketIndex);
        return out;
    }
    const uint64_t newCompletedAbsIdx = completedAbsIdx + observedCompleted;
    ResyncCycleTracking(
        static_cast<uint32_t>((newCompletedAbsIdx + Layout::kNumPackets - 1) % Layout::kNumPackets),
        observedCompleted, out);

    // Publish a neutral completion-delta high-water. Content consumers decide
    // whether their own frame/lead policy can tolerate the observed cadence.
    {
        uint32_t prevMax =
            counters_.maxDeltaConsumed.load(std::memory_order_relaxed);
        while (observedCompleted > prevMax &&
               !counters_.maxDeltaConsumed.compare_exchange_weak(
                   prevMax, observedCompleted, std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
        if (observedCompleted > prevMax) {
            controlBlock->maxCompletionDelta.store(
                observedCompleted, std::memory_order_release);
            controlBlock->maxCompletionDeltaEvents.fetch_add(
                1, std::memory_order_relaxed);
            ASFW_LOG_RING_ONLY(
                Isoch,
                ::ASFW::Logging::LogLevel::Notice,
                "IT completion delta high-water=%u",
                observedCompleted);
        }
    }

    // Publish the completion stamps of the packets that finished.
    for (uint32_t i = 0; i < observedCompleted; ++i) {
        const uint64_t currentAbsIdx = completedAbsIdx + i;
        const uint32_t completedPktSlot = static_cast<uint32_t>(currentAbsIdx % Layout::kNumPackets);

        auto* desc2 = slab_.GetDescriptorPtr(
            completedPktSlot * Layout::kBlocksPerPacket +
            Layout::kCompletionBlock);
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
    if (observedCompleted > 0) {
        controlBlock->completionCursor.store(newCompletedAbsIdx, std::memory_order_release);

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

    // 4. Map the next batch. The newest completed descriptor stays as the
    // resume anchor until its successor completes: the controller may still
    // follow its branch (Linux keeps ctx->last and frees earlier descriptor
    // storage only after moving past it, ohci.c:954-982). Every descriptor
    // older than it is free, which keeps the mapped window one ring deep.
    const uint64_t recyclableEnd = newCompletedAbsIdx - 1;
    const uint32_t toFill =
        static_cast<uint32_t>(recyclableEnd + Layout::kNumPackets - softwareFillAbsIdx_);

    // Build a detached, zero-terminated batch in those slots. The exhaustion
    // check above guarantees the old tail is not among them, and until the
    // tail's branch is published below the controller can reach only the old
    // queue, whose zero branch stops it even if this loop stalls.
    uint32_t packetsFilled = 0;
    for (uint32_t i = 0; i < toFill; ++i) {
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
                "softwareFillAbs=%llu toFill=%u i=%u numSlots=%u "
                "prepReq=%llu prepHandled=%llu prepAgeUs=%llu prepCoalesced=%llu",
                fillAbsIdx,
                pktSlot,
                expectedGen,
                commitGen,
                meta.packetIndex,
                controlBlock->committedEnd.load(std::memory_order_acquire),
                controlBlock->completionCursor.load(std::memory_order_acquire),
                softwareFillAbsIdx_,
                toFill,
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
        const uint64_t payloadOffset =
            static_cast<uint64_t>(pktSlot) * controlBlock->slotStrideBytes;

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
        if (!payloadDmaMap.ResolveTwoFragments(
                payloadOffset, payloadLength, payloadFragments)) {
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
            StampHeaderChannelAndSpeed(meta.immediateHeader[0], channel_, speed_);
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
        desc3->branchWord = i + 1 < toFill
            ? MakeBranchWordAT(
                  slab_.GetDescriptorIOVA(nextHwSlot * Layout::kBlocksPerPacket),
                  Layout::kBlocksPerPacket)
            : 0;
        AR_init_status(
            *desc3, static_cast<uint16_t>(payloadFragments[1].length));

        // Publish the shared producer slot before exposing descriptor changes.
        if (dmaMemory_) {
            const auto* payloadSlot =
                reinterpret_cast<const std::byte*>(
                    payloadBase + static_cast<size_t>(pktSlot) * controlBlock->slotStrideBytes);
            dmaMemory_->PublishToDevice(payloadSlot, payloadLength);
            dmaMemory_->PublishBarrier();
            dmaMemory_->PublishToDevice(reinterpret_cast<const std::byte*>(immDesc), sizeof(OHCIDescriptorImmediate));
            dmaMemory_->PublishToDevice(reinterpret_cast<const std::byte*>(desc2), sizeof(OHCIDescriptor));
            dmaMemory_->PublishToDevice(reinterpret_cast<const std::byte*>(desc3), sizeof(OHCIDescriptor));
        }

        packetsFilled++;
    }

    if (packetsFilled > 0) {
        // Make the whole batch visible before it becomes reachable, then link
        // it from the old tail with one aligned branch-word store. Only that
        // word is published: the tail's status may still be hardware-owned.
        // Linux context_append orders it the same way (ohci.c:1137-1141).
        if (dmaMemory_) {
            dmaMemory_->PublishBarrier();
        } else {
            ASFW::Driver::WriteBarrier();
        }
        const uint32_t oldTailSlot =
            static_cast<uint32_t>((softwareFillAbsIdx_ - 1) % Layout::kNumPackets);
        auto* oldTail = slab_.GetDescriptorPtr(
            oldTailSlot * Layout::kBlocksPerPacket + Layout::kCompletionBlock);
        oldTail->branchWord = MakeBranchWordAT(
            slab_.GetDescriptorIOVA(
                static_cast<uint32_t>(softwareFillAbsIdx_ % Layout::kNumPackets) *
                Layout::kBlocksPerPacket),
            Layout::kBlocksPerPacket);
        if (dmaMemory_) {
            dmaMemory_->PublishToDevice(
                reinterpret_cast<const std::byte*>(&oldTail->branchWord),
                sizeof(oldTail->branchWord));
            dmaMemory_->PublishBarrier();
        } else {
            ASFW::Driver::WriteBarrier();
        }
        CommitRefill(packetsFilled);
    }

    out.ok = true;
    out.packetsFilled = packetsFilled;
    return out;
}

bool IsochTxDmaRing::WakeHardware(Driver::HardwareInterface& hw, uint8_t contextIndex,
                                  const bool queueAppended) noexcept {
    Register32 ctrlReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(contextIndex));
    auto access = hw.TryBeginAccess();
    if (!access) return false;
    const uint32_t ctrl = access.Read(ctrlReg);

    const bool run = (ctrl & Driver::ContextControl::kRun) != 0;
    const bool dead = (ctrl & Driver::ContextControl::kDead) != 0;
    const bool active = (ctrl & Driver::ContextControl::kActive) != 0;

    if (run && !dead && (queueAppended || !active)) {
        Register32 ctrlSetReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControlSet(contextIndex));
        // Posted writes must reach the controller before the refill returns.
        access.WriteAndFlush(ctrlSetReg, Driver::ContextControl::kWake);
        return true;
    }
    return false;
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
