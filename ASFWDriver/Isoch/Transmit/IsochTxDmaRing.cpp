// IsochTxDmaRing.cpp

#include "IsochTxDmaRing.hpp"

#include <algorithm>
#include <DriverKit/IOLib.h>

namespace ASFW::Isoch::Tx {

using namespace ASFW::Async::HW;
using namespace ASFW::Driver;

void IsochTxDmaRing::ResetForStart() noexcept {
    softwareFillIndex_ = 0;
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

    const uint32_t aheadCount =
        (softwareFillIndex_ + Layout::kNumPackets - lastProcessedPkt) % Layout::kNumPackets;
    nextTransmitCycle_ = (hwCycle + aheadCount) % 8000;
}

void IsochTxDmaRing::CommitRefill(const uint32_t toFill) noexcept {
    softwareFillIndex_ = (softwareFillIndex_ + toFill) % Layout::kNumPackets;
    ringPacketsAhead_ += toFill;

    std::atomic_thread_fence(std::memory_order_release);
    ASFW::Driver::WriteBarrier();

    counters_.packetsRefilled.fetch_add(toFill, std::memory_order_relaxed);
}

IsochTxDmaRing::PrimeStats IsochTxDmaRing::Prime() noexcept {
    // Prime will be rewritten in Stage 3 using the shared metadata ring.
    // In Stage 2, it is a no-op that returns 0 assembled packets.
    PrimeStats stats{};
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

IsochTxDmaRing::RefillOutcome IsochTxDmaRing::Refill(Driver::HardwareInterface& hw,
                                                     uint8_t contextIndex) noexcept {
    counters_.calls.fetch_add(1, std::memory_order_relaxed);

    RefillOutcome out{};

    Register32 ctrlReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(contextIndex));
    const uint32_t ctrl = hw.Read(ctrlReg);
    const bool dead = (ctrl & Driver::ContextControl::kDead) != 0;
    if (dead) {
        counters_.exitDead.fetch_add(1, std::memory_order_relaxed);
        out.dead = true;
        return out;
    }

    Register32 cmdPtrReg = static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(contextIndex));
    const uint32_t cmdPtr = hw.Read(cmdPtrReg);
    const uint32_t cmdAddr = cmdPtr & 0xFFFFFFF0u;

    out.cmdPtr = cmdPtr;
    out.cmdAddr = cmdAddr;

    uint32_t hwLogicalIndex = 0;
    if (!slab_.DecodeCmdAddrToLogicalIndex(cmdAddr, hwLogicalIndex)) {
        counters_.exitDecodeFail.fetch_add(1, std::memory_order_relaxed);
        out.decodeFailed = true;
        return out;
    }

    const uint32_t hwPacketIndex = hwLogicalIndex / Layout::kBlocksPerPacket;
    if (hwPacketIndex >= Layout::kNumPackets) {
        counters_.exitHwOOB.fetch_add(1, std::memory_order_relaxed);
        out.hwOOB = true;
        return out;
    }

    out.hwPacketIndex = hwPacketIndex;

    const uint32_t deltaConsumed = ComputeDeltaConsumed(hwPacketIndex);
    const uint32_t gap = ringPacketsAhead_;
    UpdateGapCounters(gap);
    ResyncCycleTracking(hw, hwPacketIndex, deltaConsumed, out);

    // Stage 2: Refill loop is stubbed out. Descriptors are not populated.
    out.ok = true;
    return out;
}

void IsochTxDmaRing::WakeHardwareIfIdle(Driver::HardwareInterface& hw, uint8_t contextIndex) noexcept {
    Register32 ctrlReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContext(contextIndex));
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
