// IsochTxDmaRing.cpp

#include "IsochTxDmaRing.hpp"

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
    // Packet header Q0 (IEEE-1394 isoch header; same values as previous implementation).
    return ((2u & 0x7) << 16) |   // spd
           ((1u & 0x3) << 14) |   // tag
           ((channel & 0x3F) << 8) |
           ((0xAu & 0xF) << 4) |  // tcode = STREAM_DATA
           (0u & 0xF);
}

IsochTxDmaRing::PrimeStats IsochTxDmaRing::Prime(IIsochTxPacketProvider& provider) noexcept {
    constexpr uint32_t numPackets = Layout::kNumPackets;
    PrimeStats stats{};

    ASFW_LOG(Isoch, "IT: PrimeRing - packets=%u blocks=%u pages=%u descPerPage=%u",
             numPackets, Layout::kRingBlocks, Layout::kTotalPages, Layout::kDescriptorsPerPage);

    slab_.ValidateDescriptorLayout();

    for (uint32_t pktIdx = 0; pktIdx < numPackets; ++pktIdx) {
        const auto pkt = provider.NextSilentPacket(nextTransmitCycle_);
        nextTransmitCycle_ = (nextTransmitCycle_ + 1) % 8000;

        if (pkt.sizeBytes > Layout::kMaxPacketSize || pkt.sizeBytes > 0xFFFFu) {
            ASFW_LOG(Isoch, "IT: FATAL pkt.size=%u > max=%u pktIdx=%u",
                     pkt.sizeBytes, Layout::kMaxPacketSize, pktIdx);
            return stats;
        }

        const uint32_t descBase = pktIdx * Layout::kBlocksPerPacket;
        const uint32_t nextPktBase = ((pktIdx + 1) % numPackets) * Layout::kBlocksPerPacket;

        if (descBase >= Layout::kRingBlocks ||
            (descBase + Layout::kBlocksPerPacket - 1) >= Layout::kRingBlocks) {
            ASFW_LOG(Isoch, "IT: ❌ FATAL: descBase=%u OUT OF BOUNDS (max=%u) pktIdx=%u",
                     descBase, Layout::kRingBlocks - 1, pktIdx);
            return stats;
        }

        const uint32_t nextBlockIOVA = slab_.GetDescriptorIOVA(nextPktBase);

        uint8_t* payloadVirt = slab_.PayloadPtr(pktIdx);
        const uint32_t payloadIOVA = slab_.PayloadIOVA(pktIdx);
        if (!payloadVirt) {
            ASFW_LOG(Isoch, "IT: PrimeRing - no payload buffer");
            return stats;
        }

        if (pkt.sizeBytes > 0 && pkt.words) {
            uint32_t* dst32 = reinterpret_cast<uint32_t*>(payloadVirt);
            const size_t count32 = pkt.sizeBytes / 4;
            for (size_t k = 0; k < count32; ++k) {
                dst32[k] = pkt.words[k];
            }
        }

        const uint32_t isochHeaderQ0 = BuildIsochHeaderQ0(channel_);
        const uint32_t isochHeaderQ1 = (static_cast<uint32_t>(static_cast<uint16_t>(pkt.sizeBytes)) << 16);

        auto* immDesc = reinterpret_cast<OHCIDescriptorImmediate*>(slab_.GetDescriptorPtr(descBase));
        immDesc->common.control = (0x0200u << 16) | 8;
        immDesc->common.dataAddress = 0;
        immDesc->common.branchWord = (nextBlockIOVA & 0xFFFFFFF0u) | Layout::kBlocksPerPacket;
        immDesc->common.statusWord = 0;
        immDesc->immediateData[0] = isochHeaderQ0;
        immDesc->immediateData[1] = isochHeaderQ1;
        immDesc->immediateData[2] = 0;
        immDesc->immediateData[3] = 0;

        auto* lastDesc = slab_.GetDescriptorPtr(descBase + 2);

        const uint16_t lastReqCount = static_cast<uint16_t>(pkt.sizeBytes);
        const uint8_t intBits = ((pktIdx % 8) == 7) ? OHCIDescriptor::kIntAlways : OHCIDescriptor::kIntNever;

        const uint32_t lastControl =
            (0x1u << 28) |
            (0x1u << 27) |
            (0x0u << 24) |
            (static_cast<uint32_t>(intBits) << 20) |
            (0x3u << 18) |
            lastReqCount;

        lastDesc->control = lastControl;
        lastDesc->dataAddress = payloadIOVA;
        lastDesc->branchWord = (nextBlockIOVA & 0xFFFFFFF0u) | Layout::kBlocksPerPacket;
        lastDesc->statusWord = 0;

        stats.packetsAssembled++;
        if (pkt.isData) {
            stats.dataPackets++;
        } else {
            stats.noDataPackets++;
        }
    }

    softwareFillIndex_ = 0;
    ringPacketsAhead_ = numPackets;
    lastHwPacketIndex_ = 0;

    std::atomic_thread_fence(std::memory_order_release);
    ASFW::Driver::WriteBarrier();

    return stats;
}

IsochTxDmaRing::RefillOutcome IsochTxDmaRing::Refill(Driver::HardwareInterface& hw,
                                                     uint8_t contextIndex,
                                                     IIsochTxPacketProvider& provider,
                                                     IsochTxCaptureHook* captureHook,
                                                     IIsochTxAudioInjector* injector) noexcept {
    counters_.calls.fetch_add(1, std::memory_order_relaxed);

    RefillOutcome out{};

    Register32 ctrlReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(contextIndex));
    const uint32_t ctrl = hw.Read(ctrlReg);
    const bool dead = (ctrl & ContextControl::kDead) != 0;
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

    // Use page-aware inverse mapping for cmdPtr decoding
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

    // Fill-ahead policy tracking
    const uint32_t prevHwPacketIndex = lastHwPacketIndex_;
    uint32_t deltaConsumed = 0;
    if (hwPacketIndex >= prevHwPacketIndex) {
        deltaConsumed = hwPacketIndex - prevHwPacketIndex;
    } else {
        deltaConsumed = (Layout::kNumPackets - prevHwPacketIndex) + hwPacketIndex;
    }
    lastHwPacketIndex_ = hwPacketIndex;

    ringPacketsAhead_ -= deltaConsumed;
    if (ringPacketsAhead_ > Layout::kNumPackets) {
        ringPacketsAhead_ = 0;
    }

    // Gap monitoring
    const uint32_t gap = ringPacketsAhead_;
    counters_.lastDmaGapPackets.store(gap, std::memory_order_relaxed);
    uint32_t prevMin = counters_.minDmaGapPackets.load(std::memory_order_relaxed);
    while (gap < prevMin && !counters_.minDmaGapPackets.compare_exchange_weak(
               prevMin, gap, std::memory_order_relaxed, std::memory_order_relaxed)) {}
    constexpr uint32_t kCriticalGapThreshold = Layout::kNumPackets / 5;
    if (gap < kCriticalGapThreshold) {
        counters_.criticalGapEvents.fetch_add(1, std::memory_order_relaxed);
    }

    // Cycle resync from hardware timestamp (descriptor completion timestamp)
    if (deltaConsumed > 0 && cycleTrackingValid_) {
        const uint32_t lastProcessedPkt = (hwPacketIndex + Layout::kNumPackets - 1) % Layout::kNumPackets;
        auto* processedOL = slab_.GetDescriptorPtr(lastProcessedPkt * Layout::kBlocksPerPacket + 2);
        const uint16_t hwTimestamp = static_cast<uint16_t>(processedOL->statusWord & 0xFFFF);
        out.hwTimestamp = hwTimestamp;

        if (processedOL->statusWord != 0) {
            const uint32_t hwCycle = hwTimestamp & 0x1FFF;
            lastHwTimestamp_ = hwTimestamp;

            const uint32_t aheadCount = (softwareFillIndex_ + Layout::kNumPackets - lastProcessedPkt) % Layout::kNumPackets;
            nextTransmitCycle_ = (hwCycle + aheadCount) % 8000;
        }
    }

    // Phase 2: keep ring full with silent/cadence-correct packets
    const uint32_t toFill = (ringPacketsAhead_ < Layout::kMaxWriteAhead)
        ? (Layout::kMaxWriteAhead - ringPacketsAhead_) : 0;

    if (toFill > 0) {
        counters_.refills.fetch_add(1, std::memory_order_relaxed);

        for (uint32_t i = 0; i < toFill; ++i) {
            const uint32_t pktIdx = (softwareFillIndex_ + i) % Layout::kNumPackets;
            const uint32_t descBase = pktIdx * Layout::kBlocksPerPacket;

            uint8_t* payloadVirt = slab_.PayloadPtr(pktIdx);
            const uint32_t payloadIOVA = slab_.PayloadIOVA(pktIdx);
            if (!payloadVirt) {
                counters_.fatalDescriptorBounds.fetch_add(1, std::memory_order_relaxed);
                return out;
            }

            if (captureHook) {
                auto* existingLastDesc = slab_.GetDescriptorPtr(descBase + 2);
                captureHook->CaptureBeforeOverwrite(pktIdx,
                                                    hwPacketIndex,
                                                    cmdPtr,
                                                    existingLastDesc,
                                                    reinterpret_cast<const uint32_t*>(payloadVirt));
            }

            const auto pkt = provider.NextSilentPacket(nextTransmitCycle_);
            nextTransmitCycle_ = (nextTransmitCycle_ + 1) % 8000;

            if (pkt.sizeBytes > Layout::kMaxPacketSize || pkt.sizeBytes > 0xFFFFu) {
                counters_.fatalPacketSize.fetch_add(1, std::memory_order_relaxed);
                return out;
            }

            if (descBase >= Layout::kRingBlocks || (descBase + Layout::kBlocksPerPacket - 1) >= Layout::kRingBlocks) {
                counters_.fatalDescriptorBounds.fetch_add(1, std::memory_order_relaxed);
                return out;
            }

            if (pkt.sizeBytes > 0 && pkt.words) {
                uint32_t* dst32 = reinterpret_cast<uint32_t*>(payloadVirt);
                const size_t count32 = pkt.sizeBytes / 4;
                for (size_t k = 0; k < count32; ++k) {
                    dst32[k] = pkt.words[k];
                }
            }

            auto* lastDesc = slab_.GetDescriptorPtr(descBase + 2);
            const uint16_t lastReqCount = static_cast<uint16_t>(pkt.sizeBytes);

            const uint32_t existingControl = lastDesc->control & 0xFFFF0000u;
            lastDesc->control = existingControl | lastReqCount;
            lastDesc->dataAddress = payloadIOVA;
            lastDesc->statusWord = 0;

            auto* immDesc = reinterpret_cast<OHCIDescriptorImmediate*>(slab_.GetDescriptorPtr(descBase));
            const uint32_t isochHeaderQ1 = (static_cast<uint32_t>(pkt.sizeBytes) & 0xFFFFu) << 16;
            immDesc->immediateData[1] = isochHeaderQ1;

            out.packetsFilled++;
            if (pkt.isData) {
                out.dataPackets++;
            } else {
                out.noDataPackets++;
            }
        }

        softwareFillIndex_ = (softwareFillIndex_ + toFill) % Layout::kNumPackets;
        ringPacketsAhead_ += toFill;

        std::atomic_thread_fence(std::memory_order_release);
        ASFW::Driver::WriteBarrier();

        counters_.packetsRefilled.fetch_add(toFill, std::memory_order_relaxed);
    }

    // Phase 3: near-HW audio injection
    if (injector) {
        injector->InjectNearHw(hwPacketIndex, slab_);
    }

    out.ok = true;
    return out;
}

void IsochTxDmaRing::WakeHardwareIfIdle(Driver::HardwareInterface& hw, uint8_t contextIndex) noexcept {
    Register32 ctrlReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(contextIndex));
    const uint32_t ctrl = hw.Read(ctrlReg);

    const bool run = (ctrl & ContextControl::kRun) != 0;
    const bool dead = (ctrl & ContextControl::kDead) != 0;
    const bool active = (ctrl & ContextControl::kActive) != 0;

    if (run && !dead && !active) {
        Register32 ctrlSetReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControlSet(contextIndex));
        hw.Write(ctrlSetReg, ContextControl::kWake);
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

void IsochTxDmaRing::DumpPayloadBuffers(uint32_t numPackets) const noexcept {
    const auto buf = slab_.PayloadRegion();
    if (!buf.virtualBase) {
        ASFW_LOG(Isoch, "IT: DumpPayloadBuffers - no buffer allocated");
        return;
    }

    const uint32_t numTotalPackets = Layout::kNumPackets;
    if (numPackets > numTotalPackets) numPackets = numTotalPackets;

    ASFW_LOG(Isoch, "IT: === DMA Payload Buffer Dump (first %u of %u packets) ===", numPackets, numTotalPackets);

    for (uint32_t pktIdx = 0; pktIdx < numPackets; ++pktIdx) {
        const uint8_t* payloadVirt = reinterpret_cast<const uint8_t*>(buf.virtualBase) +
                                     (pktIdx * Layout::kMaxPacketSize);
        const uint32_t* payload32 = reinterpret_cast<const uint32_t*>(payloadVirt);

        const uint32_t cip0 = payload32[0];
        const uint32_t cip1 = payload32[1];

        const uint32_t aud0 = payload32[2];
        const uint32_t aud1 = payload32[3];
        const uint32_t aud2 = payload32[4];
        const uint32_t aud3 = payload32[5];

        const bool isNoData = (aud0 == 0 && aud1 == 0);
        const bool isSilence = ((aud0 & 0xFFFFFF) == 0) && ((aud1 & 0xFFFFFF) == 0);

        ASFW_LOG(Isoch, "  Pkt[%u] CIP=[%08x %08x] Audio=[%08x %08x %08x %08x] %{public}s%{public}s",
                 pktIdx, cip0, cip1, aud0, aud1, aud2, aud3,
                 isNoData ? "NO-DATA" : "DATA",
                 (isSilence && !isNoData) ? " (SILENCE!)" : "");
    }

    ASFW_LOG(Isoch, "IT: === End DMA Buffer Dump ===");
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
