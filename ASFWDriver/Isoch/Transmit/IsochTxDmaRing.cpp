// IsochTxDmaRing.cpp

#include "IsochTxDmaRing.hpp"

#include <algorithm>
#include <DriverKit/IOLib.h>

namespace ASFW::Isoch::Tx {

using namespace ASFW::Async::HW;
using namespace ASFW::Driver;

void IsochTxDmaRing::ResetForStart() noexcept {
    softwareFillAbsIdx_ = 0;
    lastHwPacketIndex_ = 0;
    ringPacketsAhead_ = 0;

    nextTransmitCycle_ = 0;
    cycleTrackingValid_ = false;
    lastHwTimestamp_ = 0;

    counters_.lastDmaGapPackets.store(Layout::kNumPackets, std::memory_order_relaxed);
    counters_.minDmaGapPackets.store(Layout::kNumPackets, std::memory_order_relaxed);
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

uint32_t IsochTxDmaRing::BuildIsochHeaderQ0(uint8_t channel) noexcept {
    return ((2u & 0x7) << 16) |   // spd
           ((1u & 0x3) << 14) |   // tag
           ((channel & 0x3F) << 8) |
           ((0xAu & 0xF) << 4) |  // tcode = STREAM_DATA
           (0u & 0xF);
}

uint32_t IsochTxDmaRing::ComputeDeltaConsumed(const uint32_t hwPacketIndex) noexcept {
    const uint32_t prevHwPacketIndex = lastHwPacketIndex_;
    const uint32_t deltaConsumed =
        (hwPacketIndex >= prevHwPacketIndex)
            ? (hwPacketIndex - prevHwPacketIndex)
            : ((Layout::kNumPackets - prevHwPacketIndex) + hwPacketIndex);
    lastHwPacketIndex_ = hwPacketIndex;

    ringPacketsAhead_ -= deltaConsumed;
    if (ringPacketsAhead_ > Layout::kNumPackets) {
        ringPacketsAhead_ = 0;
    }

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
    auto* processedOL = slab_.GetDescriptorPtr(lastProcessedPkt * Layout::kBlocksPerPacket + 2);

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

IsochTxDmaRing::PrimeStats IsochTxDmaRing::Prime() noexcept {
    PrimeStats stats{};
    if (!slab_.IsValid()) {
        ASFW_LOG(Isoch, "IT: Prime failed - descriptor slab is invalid");
        return stats;
    }

    const uint32_t numPackets = Layout::kNumPackets;
    const uint32_t bufBaseIOVA = static_cast<uint32_t>(slab_.PayloadRegion().deviceBase);

    for (uint32_t pktIdx = 0; pktIdx < numPackets; ++pktIdx) {
        const uint32_t descBase = pktIdx * Layout::kBlocksPerPacket;
        auto* desc0 = slab_.GetDescriptorPtr(descBase);
        auto* immDesc = reinterpret_cast<OHCIDescriptorImmediate*>(desc0);

        // Prime the OMI descriptor structure (Descriptor 0 and 1)
        std::memset(immDesc, 0, sizeof(OHCIDescriptorImmediate));
        immDesc->common.control = OHCIDescriptor::BuildControl({
            .reqCount = 4, // CIP Q0 only
            .command = OHCIDescriptor::kCmdOutputMore,
            .key = OHCIDescriptor::kKeyImmediate,
            .interruptBits = OHCIDescriptor::kIntNever,
            .branchBits = OHCIDescriptor::kBranchNever,
        });

        // Prime the OL descriptor (Descriptor 2)
        auto* desc2 = slab_.GetDescriptorPtr(descBase + 2);
        std::memset(desc2, 0, sizeof(OHCIDescriptor));

        const uint32_t nextPktIdx = (pktIdx + 1) % numPackets;
        const uint32_t nextDescIOVA = slab_.GetDescriptorIOVA(nextPktIdx * Layout::kBlocksPerPacket);

        desc2->control = OHCIDescriptor::BuildControl({
            .reqCount = 0, // Set during refill
            .command = OHCIDescriptor::kCmdOutputLast,
            .key = OHCIDescriptor::kKeyStandard,
            .interruptBits = ((pktIdx + 1) % 8 == 0) ? OHCIDescriptor::kIntAlways : OHCIDescriptor::kIntNever,
            .branchBits = OHCIDescriptor::kBranchAlways,
        });

        // Set status update bit (s=1)
        desc2->control |= (1u << (OHCIDescriptor::kStatusShift + OHCIDescriptor::kControlHighShift));

        desc2->dataAddress = bufBaseIOVA + pktIdx * Layout::kMaxPacketSize;
        desc2->branchWord = MakeBranchWordAT(nextDescIOVA, Layout::kBlocksPerPacket);
        AR_init_status(*desc2, 0);
    }

    if (dmaMemory_) {
        dmaMemory_->PublishToDevice(slab_.DescriptorRegion().virtualBase, slab_.DescriptorRegion().size);
    }

    stats.packetsAssembled = numPackets;
    ASFW_LOG(Isoch, "IT: Static descriptor ring primed. numPackets=%u", numPackets);
    return stats;
}

bool IsochTxDmaRing::DecodeHardwarePacketIndex(Driver::HardwareInterface& hw,
                                               const uint8_t contextIndex,
                                               uint32_t& outPacketIndex,
                                               uint32_t& outCmdPtr) noexcept {
    const Register32 cmdPtrReg =
        static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(contextIndex));
    outCmdPtr = hw.Read(cmdPtrReg);
    const uint32_t cmdAddr = outCmdPtr & 0xFFFFFFF0u;

    uint32_t hwLogicalIndex = 0;
    if (!slab_.DecodeCmdAddrToLogicalIndex(cmdAddr, hwLogicalIndex)) {
        return false;
    }

    outPacketIndex = hwLogicalIndex / Layout::kBlocksPerPacket;
    return outPacketIndex < Layout::kNumPackets;
}

IsochTxDmaRing::RefillOutcome IsochTxDmaRing::Refill(
    Driver::HardwareInterface& hw,
    uint8_t contextIndex,
    ASFW::IsochTransport::TxPacketMeta* metadataRing,
    ASFW::IsochTransport::TxStreamControl* controlBlock,
    uint32_t numSlots,
    uint8_t* payloadBase,
    uint64_t payloadIOVA) noexcept
{
    counters_.calls.fetch_add(1, std::memory_order_relaxed);
    RefillOutcome out{};

    if (!metadataRing || !controlBlock || numSlots == 0) {
        out.ok = false;
        return out;
    }

    // 1. Capture CYCLE_TIMER and host time, and publish under seqlock.
    // Clock-smoothing / filtering is handled natively by AudioDriverKit's clock algorithms.
    {
        const uint32_t cycleTimer = hw.Read(static_cast<Register32>(Register32::kCycleTimer));
        const uint64_t hostTime = mach_absolute_time();

        ASFW::IsochTransport::ClockPairSample sample{};
        sample.hostTimeMid = hostTime;
        sample.cycleTimer32 = cycleTimer;
        controlBlock->clockPair.Publish(sample);
    }

    // 2. Check context status
    const Register32 ctrlReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(contextIndex));
    const uint32_t ctrl = hw.Read(ctrlReg);
    const bool dead = (ctrl & Driver::ContextControl::kDead) != 0;
    if (dead) {
        counters_.exitDead.fetch_add(1, std::memory_order_relaxed);
        controlBlock->statusWord.store(ASFW::IsochTransport::TxStreamStatus::kDeadContext, std::memory_order_release);
        controlBlock->streamGeneration.fetch_add(1, std::memory_order_release);
        out.dead = true;
        return out;
    }

    // 3. Decode hardware pointer and advance completed cursor
    uint32_t hwPacketIndex = 0;
    uint32_t cmdPtr = 0;
    if (!DecodeHardwarePacketIndex(hw, contextIndex, hwPacketIndex, cmdPtr)) {
        counters_.exitDecodeFail.fetch_add(1, std::memory_order_relaxed);
        out.decodeFailed = true;
        return out;
    }

    out.hwPacketIndex = hwPacketIndex;
    out.cmdPtr = cmdPtr;
    out.cmdAddr = cmdPtr & 0xFFFFFFF0u;

    const uint32_t deltaConsumed = ComputeDeltaConsumed(hwPacketIndex);
    const uint32_t gap = ringPacketsAhead_;
    UpdateGapCounters(gap);
    ResyncCycleTracking(hw, hwPacketIndex, deltaConsumed, out);

    // Fetch and publish completed stamps
    const uint64_t completedAbsIdx = controlBlock->completionCursor.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < deltaConsumed; ++i) {
        const uint64_t currentAbsIdx = completedAbsIdx + i;
        const uint32_t completedPktSlot = static_cast<uint32_t>(currentAbsIdx % Layout::kNumPackets);

        auto* desc2 = slab_.GetDescriptorPtr(completedPktSlot * Layout::kBlocksPerPacket + 2);
        if (dmaMemory_) {
            dmaMemory_->FetchFromDevice(reinterpret_cast<const std::byte*>(desc2), sizeof(*desc2));
        }

        const uint16_t hwTimestamp = static_cast<uint16_t>(desc2->statusWord & 0xFFFF);
        controlBlock->PushCompletionStamp(currentAbsIdx, hwTimestamp);
    }
    if (deltaConsumed > 0) {
        controlBlock->completionCursor.store(completedAbsIdx + deltaConsumed, std::memory_order_release);
    }

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
        const uint64_t expectedGen = ASFW::IsochTransport::ExpectedCommitGen(fillAbsIdx, numSlots);
        const uint64_t commitGen = meta.commitGen.load(std::memory_order_acquire);

        // Check if committed
        if (commitGen != expectedGen) {
            // Fatal Underrun!
            ASFW_LOG(Isoch, "IT FATAL UNDERRUN: fillAbsIdx=%llu expectedGen=%llu commitGen=%llu",
                     fillAbsIdx, expectedGen, commitGen);
            controlBlock->statusWord.store(ASFW::IsochTransport::TxStreamStatus::kUnderrunFatal, std::memory_order_release);
            controlBlock->streamGeneration.fetch_add(1, std::memory_order_release);

            // Stop context immediately
            const Register32 ctrlClrReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControlClear(contextIndex));
            hw.Write(ctrlClrReg, Driver::ContextControl::kRun);
            hw.Write(Register32::kIsoXmitIntMaskClear, (1u << contextIndex));

            out.ok = false;
            return out;
        }

        // Copy quadlets to OMI descriptor (Descriptor 0)
        auto* desc0 = slab_.GetDescriptorPtr(pktSlot * Layout::kBlocksPerPacket);
        desc0->branchWord = meta.immediateHeader[0];
        desc0->statusWord = meta.immediateHeader[1];

        // Update OL descriptor (Descriptor 2) reqCount
        auto* desc2 = slab_.GetDescriptorPtr(pktSlot * Layout::kBlocksPerPacket + 2);
        desc2->control = (desc2->control & 0xFFFF0000u) | (meta.payloadLength & 0xFFFFu);
        AR_init_status(*desc2, meta.payloadLength);

        // Flush descriptor changes to device
        if (dmaMemory_) {
            dmaMemory_->PublishToDevice(reinterpret_cast<const std::byte*>(desc0), sizeof(OHCIDescriptorImmediate));
            dmaMemory_->PublishToDevice(reinterpret_cast<const std::byte*>(desc2), sizeof(OHCIDescriptor));
        }

        packetsFilled++;
    }

    if (packetsFilled > 0) {
        CommitRefill(packetsFilled);
    }

    out.ok = true;
    out.packetsFilled = packetsFilled;
    return out;
}

void IsochTxDmaRing::WakeHardwareIfIdle(Driver::HardwareInterface& hw, uint8_t contextIndex) noexcept {
    Register32 ctrlReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(contextIndex));
    const uint32_t ctrl = hw.Read(ctrlReg);

    const bool run = (ctrl & Driver::ContextControl::kRun) != 0;
    const bool dead = (ctrl & Driver::ContextControl::kDead) != 0;
    const bool active = (ctrl & Driver::ContextControl::kActive) != 0;

    if (run && !dead && !active) {
        Register32 ctrlSetReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControlSet(contextIndex));
        hw.Write(ctrlSetReg, Driver::ContextControl::kWake);
    }
}

void IsochTxDmaRing::DumpAtCmdPtr(Driver::HardwareInterface& hw, uint8_t contextIndex) const noexcept {
#ifndef ASFW_HOST_TEST
    Register32 cmdPtrReg = static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(contextIndex));
    const uint32_t cmdPtr = hw.Read(cmdPtrReg);
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
    const uint32_t bufBaseIOVA = static_cast<uint32_t>(slab_.PayloadRegion().deviceBase);

    ASFW_LOG(Isoch, "IT: DescRing Dump pkts %u-%u (total=%u pages=%u) DescBase=0x%08x BufBase=0x%08x Z=%u",
             startPacket, startPacket + numPackets - 1, totalPackets, Layout::kTotalPages,
             descBaseIOVA, bufBaseIOVA, Layout::kBlocksPerPacket);

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

        auto* desc2 = slab_.GetDescriptorPtr(descBase + 2);
        const uint32_t ctl1 = desc2->control;
        const uint32_t i1 = (ctl1 >> 18) & 0x3;
        const uint32_t b1 = (ctl1 >> 16) & 0x3;
        const uint32_t reqCount1 = ctl1 & 0xFFFF;
        const uint32_t branchAddr = desc2->branchWord & 0xFFFFFFF0u;
        const uint32_t branchZ = desc2->branchWord & 0xF;
        const uint16_t xferStatus = static_cast<uint16_t>(desc2->statusWord >> 16);

        const uint32_t computedIOVA = slab_.GetDescriptorIOVA(descBase);

        ASFW_LOG(Isoch, "  Pkt[%u] @desc%u IOVA=0x%08x OMI: ctl=0x%08x i=%u b=%u skip=0x%08x|%u Q0=0x%08x(spd=%u tag=%u ch=%u tcode=0x%x sy=%u) Q1=0x%08x(len=%u)",
                 pktIdx, descBase, computedIOVA, ctl0, i0, b0, skipAddr, skipZ,
                 itQ0, spd, tag, chan, tcode, sy,
                 itQ1, dataLen);
        ASFW_LOG(Isoch, "         OL:  ctl=0x%08x i=%u b=%u req=%u data=0x%08x br=0x%08x|%u st=0x%04x",
                 ctl1, i1, b1, reqCount1, desc2->dataAddress, branchAddr, branchZ, xferStatus);
    }
}

} // namespace ASFW::Isoch::Tx
