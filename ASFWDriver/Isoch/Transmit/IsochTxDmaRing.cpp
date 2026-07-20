// IsochTxDmaRing.cpp

#include "IsochTxDmaRing.hpp"

#include "../../Common/TimingUtils.hpp"

#include <algorithm>
#include <cstring>
#include <DriverKit/IOLib.h>

namespace ASFW::Isoch::Tx {

using namespace ASFW::Async::HW;
using namespace ASFW::Driver;

namespace {
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

// The generic AT helper intentionally rejects Z=1 because the normal ASFW
// packet program always occupies four descriptor blocks. Stage 5E proved that
// a true OHCI skip program is exactly one descriptor, so Stage 5G needs a
// narrowly-scoped encoder that permits only the two program sizes used by the
// finite cadence: Z=4 for data and Z=1 for skip.
[[nodiscard]] inline uint32_t MakeFiniteCadenceBranchWord(
    const uint32_t descriptorIOVA,
    const uint32_t zBlocks) noexcept {
    if (descriptorIOVA == 0U || (descriptorIOVA & 0xFU) != 0U ||
        (zBlocks != 1U && zBlocks != Layout::kBlocksPerPacket)) {
        return 0U;
    }
    return (descriptorIOVA & 0xFFFFFFF0U) | (zBlocks & 0xFU);
}
} // namespace

void IsochTxDmaRing::ResetForStart() noexcept {
    softwareFillAbsIdx_ = 0;
    lastHwPacketIndex_ = 0;
    ringPacketsAhead_ = 0;

    nextTransmitCycle_ = 0;
    cycleTrackingValid_ = false;
    lastHwTimestamp_ = 0;

    counters_.lastDmaGapPackets.store(Layout::kNumPackets, std::memory_order_relaxed);
    counters_.minDmaGapPackets.store(Layout::kNumPackets, std::memory_order_relaxed);

    counters_.wireDataPackets.store(0, std::memory_order_relaxed);
    counters_.wireZeroPcmPackets.store(0, std::memory_order_relaxed);
    counters_.wireInfoQuads.store(0, std::memory_order_relaxed);
    counters_.wirePcmDropouts.store(0, std::memory_order_relaxed);
    counters_.wireMaxAbs24.store(0, std::memory_order_relaxed);
    counters_.wireLastInfoQuad.store(0, std::memory_order_relaxed);
    counters_.wireFirstInfoAbsIdx.store(0, std::memory_order_relaxed);
    wireLastPacketHadInfo_ = false;
    wireFirstInfoLogged_ = false;
}

void IsochTxDmaRing::GaugeWirePayload(uint64_t fillAbsIdx,
                                      const uint8_t* packetBytes,
                                      uint32_t payloadLength,
                                      uint32_t immediateHeaderQ0LE) noexcept {
    constexpr uint32_t kCipHeaderBytes = 8;
    constexpr uint32_t kIdleSlotWord = 0x80000000u; // AM824 no-info / idle MIDI
    const uint32_t headerQ0 = OSSwapLittleToHostInt32(immediateHeaderQ0LE);
    const auto tag = ASFW::IsochTransport::DecodeIsochTxHeaderTag(headerQ0);
    const uint32_t streamHeaderBytes =
        tag == ASFW::IsochTransport::IsochPacketTag::kCip ? kCipHeaderBytes : 0U;
    if (payloadLength <= streamHeaderBytes) {
        return; // CIP header-only or a true empty no-CIP cycle.
    }

    const uint64_t dataPackets =
        counters_.wireDataPackets.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((dataPackets % 8192) == 0) {
        ASFW_LOG(Isoch,
                 "IT WIRE gauge data=%llu zeroPcm=%llu infoQuads=%llu dropouts=%llu maxAbs24=%u lastQuad=0x%08x",
                 dataPackets,
                 counters_.wireZeroPcmPackets.load(std::memory_order_relaxed),
                 counters_.wireInfoQuads.load(std::memory_order_relaxed),
                 counters_.wirePcmDropouts.load(std::memory_order_relaxed),
                 counters_.wireMaxAbs24.load(std::memory_order_relaxed),
                 counters_.wireLastInfoQuad.load(std::memory_order_relaxed));
    }

    const uint32_t quadCount = (payloadLength - streamHeaderBytes) / 4;
    const uint8_t* quadBytes = packetBytes + streamHeaderBytes;
    uint32_t infoQuads = 0;
    uint32_t lastInfoQuad = 0;
    uint32_t maxAbs24 = 0;
    for (uint32_t i = 0; i < quadCount; ++i, quadBytes += 4) {
        // Payload is stored in bus (big-endian) byte order.
        const uint32_t quad = (static_cast<uint32_t>(quadBytes[0]) << 24) |
                              (static_cast<uint32_t>(quadBytes[1]) << 16) |
                              (static_cast<uint32_t>(quadBytes[2]) << 8) |
                              static_cast<uint32_t>(quadBytes[3]);
        if (quad == 0 || quad == kIdleSlotWord) {
            continue;
        }
        ++infoQuads;
        lastInfoQuad = quad;
        // 24-bit two's-complement magnitude, label-agnostic (works for both
        // raw sign-extended 24-in-32 and 0x40-labelled AM824 MBLA slots).
        const int32_t sample24 = static_cast<int32_t>(quad << 8) >> 8;
        const uint32_t abs24 = static_cast<uint32_t>(
            sample24 < 0 ? -static_cast<int64_t>(sample24) : sample24);
        if (abs24 > maxAbs24) {
            maxAbs24 = abs24;
        }
    }

    if (infoQuads == 0) {
        counters_.wireZeroPcmPackets.fetch_add(1, std::memory_order_relaxed);
        if (wireLastPacketHadInfo_) {
            const uint64_t dropouts =
                counters_.wirePcmDropouts.fetch_add(1, std::memory_order_relaxed) + 1;
            if (dropouts <= 8 || (dropouts % 64) == 0) {
                ASFW_LOG(Isoch,
                         "IT WIRE PCM dropout #%llu at absIdx=%llu (nonzero->zero)",
                         dropouts, fillAbsIdx);
            }
        }
        wireLastPacketHadInfo_ = false;
        return;
    }

    counters_.wireInfoQuads.fetch_add(infoQuads, std::memory_order_relaxed);
    counters_.wireLastInfoQuad.store(lastInfoQuad, std::memory_order_relaxed);
    uint32_t previousMax = counters_.wireMaxAbs24.load(std::memory_order_relaxed);
    if (maxAbs24 > previousMax) {
        counters_.wireMaxAbs24.store(maxAbs24, std::memory_order_relaxed);
    }
    if (!wireFirstInfoLogged_) {
        counters_.wireFirstInfoAbsIdx.store(fillAbsIdx, std::memory_order_relaxed);
        ASFW_LOG(Isoch,
                 "IT WIRE first nonzero PCM absIdx=%llu infoQuads=%u/%u lastQuad=0x%08x maxAbs24=%u",
                 fillAbsIdx, infoQuads, quadCount, lastInfoQuad, maxAbs24);
        wireFirstInfoLogged_ = true;
    }
    wireLastPacketHadInfo_ = true;
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
    auto* processedFirst = slab_.GetDescriptorPtr(
        lastProcessedPkt * Layout::kBlocksPerPacket);
    if (dmaMemory_) {
        dmaMemory_->FetchFromDevice(
            reinterpret_cast<const std::byte*>(processedFirst),
            sizeof(*processedFirst));
    }
    auto* processedCompletion =
        CompletionDescriptorForPacket(lastProcessedPkt);
    if (dmaMemory_ && processedCompletion != processedFirst) {
        dmaMemory_->FetchFromDevice(
            reinterpret_cast<const std::byte*>(processedCompletion),
            sizeof(*processedCompletion));
    }

    const uint16_t hwTimestamp = static_cast<uint16_t>(
        processedCompletion->statusWord & 0xFFFFU);
    out.hwTimestamp = hwTimestamp;

    if (processedCompletion->statusWord == 0) {
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

void IsochTxDmaRing::ProgramSkipPacket(const uint32_t packetSlot) noexcept {
    const uint32_t descBase = packetSlot * Layout::kBlocksPerPacket;
    for (uint32_t block = 0; block < Layout::kBlocksPerPacket; ++block) {
        auto* desc = slab_.GetDescriptorPtr(descBase + block);
        std::memset(desc, 0, sizeof(OHCIDescriptor));
    }

    // A no-CIP idle cycle is a true OHCI skip: one zero-length standard
    // OUTPUT_LAST descriptor and no immediate isoch header. Keep the fixed
    // four-block slot geometry by branching directly to the next slot with
    // Z=4; the remaining three descriptors in this slot stay zeroed.
    auto* skip = slab_.GetDescriptorPtr(descBase);
    const uint32_t nextPacketSlot =
        (packetSlot + 1U) % Layout::kNumPackets;
    ITDescriptorBuilder::BuildOutputLast(
        *skip,
        {
            .dataIOVA = 0,
            .payloadSize = 0,
            .branchIOVA = slab_.GetDescriptorIOVA(
                nextPacketSlot * Layout::kBlocksPerPacket),
            .zValue = Layout::kBlocksPerPacket,
            .interruptBits =
                Core::IsTimingGroupBoundary(packetSlot)
                    ? OHCIDescriptor::kIntAlways
                    : OHCIDescriptor::kIntNever,
        });
}

bool IsochTxDmaRing::ProgramSingleSilencePacketForRunPreflight(
    uint8_t* payloadBase,
    const uint32_t numSlots,
    const uint32_t slotStrideBytes,
    const ASFW::IsochTransport::TxPacketMeta* metadataRing,
    SingleSilencePacketProgram& outProgram) noexcept {
    outProgram = {};
    if (!slab_.IsValid() || payloadBase == nullptr || metadataRing == nullptr ||
        numSlots == 0U || slotStrideBytes == 0U) {
        return false;
    }

    constexpr uint32_t kExpectedPayloadBytes = 1536U;
    for (uint32_t packetSlot = 0U;
         packetSlot < Layout::kNumPackets;
         ++packetSlot) {
        const uint32_t producerSlot = packetSlot % numSlots;
        const auto& meta = metadataRing[producerSlot];
        const uint32_t metaQ0 =
            OSSwapLittleToHostInt32(meta.immediateHeader[0]);
        const auto tag = ASFW::IsochTransport::DecodeIsochTxHeaderTag(metaQ0);
        if (ASFW::IsochTransport::ShouldSkipIsochTxPacket(
                tag, meta.payloadLength)) {
            continue;
        }
        if (tag != ASFW::IsochTransport::IsochPacketTag::kNoCipHeader ||
            meta.payloadLength != kExpectedPayloadBytes ||
            meta.payloadLength > slotStrideBytes) {
            return false;
        }

        const uint8_t* payload = payloadBase +
            static_cast<size_t>(producerSlot) * slotStrideBytes;
        for (uint32_t byte = 0U; byte < meta.payloadLength; ++byte) {
            if (payload[byte] != 0U) {
                ASFW_LOG(Isoch,
                         "IT: Stage 5F single-packet safety check rejected nonzero payload packet=%u slot=%u byte=%u value=0x%02x",
                         packetSlot,
                         producerSlot,
                         byte,
                         payload[byte]);
                return false;
            }
        }

        const uint32_t descBase = packetSlot * Layout::kBlocksPerPacket;
        auto* immDesc = reinterpret_cast<OHCIDescriptorImmediate*>(
            slab_.GetDescriptorPtr(descBase));
        auto* desc2 = slab_.GetDescriptorPtr(
            descBase + Layout::kFirstPayloadBlock);
        auto* completion = slab_.GetDescriptorPtr(
            descBase + Layout::kCompletionBlock);

        const uint32_t wireQ0 =
            OSSwapLittleToHostInt32(immDesc->immediateData[0]);
        const uint32_t wireQ1 =
            OSSwapLittleToHostInt32(immDesc->immediateData[1]);
        const uint8_t wireChannel =
            static_cast<uint8_t>((wireQ0 >> 8U) & 0x3FU);
        if (ASFW::IsochTransport::DecodeIsochTxHeaderTag(wireQ0) !=
                ASFW::IsochTransport::IsochPacketTag::kNoCipHeader ||
            ASFW::IsochTransport::DecodeIsochTxHeaderSpeed(wireQ0) !=
                ASFW::IsochTransport::kIsochSpeedS400 ||
            ASFW::IsochTransport::DecodeIsochTxHeaderTCode(wireQ0) !=
                ASFW::IsochTransport::kIsochDataBlockTCode ||
            ASFW::IsochTransport::DecodeIsochTxHeaderDataLength(wireQ1) !=
                meta.payloadLength ||
            wireChannel != channel_) {
            ASFW_LOG(Isoch,
                     "IT: Stage 5F single-packet header validation failed packet=%u tag=%u speed=%u tcode=0x%x channel=%u/%u dataLength=%u/%u",
                     packetSlot,
                     static_cast<uint32_t>(
                         ASFW::IsochTransport::DecodeIsochTxHeaderTag(wireQ0)),
                     ASFW::IsochTransport::DecodeIsochTxHeaderSpeed(wireQ0),
                     ASFW::IsochTransport::DecodeIsochTxHeaderTCode(wireQ0),
                     wireChannel,
                     channel_,
                     ASFW::IsochTransport::DecodeIsochTxHeaderDataLength(wireQ1),
                     meta.payloadLength);
            return false;
        }

        // Convert the normal circular packet program to a finite one-packet
        // program. The immediate descriptor's self-link remains the OHCI skip
        // address for a missed cycle; the OUTPUT_LAST branch is the queue tail.
        const uint32_t intMask = 0x3U <<
            (OHCIDescriptor::kIntShift + OHCIDescriptor::kControlHighShift);
        completion->control &= ~intMask;
        completion->branchWord = 0U;
        AR_init_status(*completion, 0U);
        desc2->statusWord = 0U;
        immDesc->common.statusWord = 0U;

        if (dmaMemory_) {
            dmaMemory_->PublishToDevice(
                reinterpret_cast<const std::byte*>(payload),
                meta.payloadLength);
            dmaMemory_->PublishBarrier();
            dmaMemory_->PublishToDevice(
                reinterpret_cast<const std::byte*>(immDesc),
                sizeof(OHCIDescriptorImmediate));
            dmaMemory_->PublishToDevice(
                reinterpret_cast<const std::byte*>(desc2),
                sizeof(OHCIDescriptor));
            dmaMemory_->PublishToDevice(
                reinterpret_cast<const std::byte*>(completion),
                sizeof(OHCIDescriptor));
        } else {
            ASFW::Driver::WriteBarrier();
        }

        const uint64_t descriptorIOVA =
            slab_.GetDescriptorIOVA(descBase);
        if (descriptorIOVA == 0U || descriptorIOVA > 0xFFFFFFFFULL) {
            return false;
        }

        outProgram.packetSlot = packetSlot;
        outProgram.producerSlot = producerSlot;
        outProgram.commandPtr =
            static_cast<uint32_t>(descriptorIOVA) |
            Layout::kBlocksPerPacket;
        outProgram.payloadLength = meta.payloadLength;
        return true;
    }
    return false;
}

IsochTxDmaRing::SingleSilencePacketCompletion
IsochTxDmaRing::FetchSingleSilencePacketCompletionForRunPreflight(
    const uint32_t packetSlot) noexcept {
    SingleSilencePacketCompletion out{};
    if (!slab_.IsValid() || packetSlot >= Layout::kNumPackets) {
        return out;
    }

    auto* completion = slab_.GetDescriptorPtr(
        packetSlot * Layout::kBlocksPerPacket +
        Layout::kCompletionBlock);
    if (dmaMemory_) {
        dmaMemory_->FetchFromDevice(
            reinterpret_cast<const std::byte*>(completion),
            sizeof(*completion));
    } else {
        ASFW::Driver::ReadBarrier();
    }
    out.transferStatus = AT_xferStatus(*completion);
    out.timestamp = AT_timeStamp(*completion);
    return out;
}

bool IsochTxDmaRing::ProgramFiniteSilenceCadenceForRunPreflight(
    uint8_t* payloadBase,
    const uint32_t numSlots,
    const uint32_t slotStrideBytes,
    const ASFW::IsochTransport::TxPacketMeta* metadataRing,
    FiniteSilenceCadenceProgram& outProgram) noexcept {
    outProgram = {};
    if (!slab_.IsValid() || payloadBase == nullptr || metadataRing == nullptr ||
        numSlots == 0U || slotStrideBytes == 0U) {
        return false;
    }

    static_assert(Layout::kNumPackets == 48U);
    constexpr uint32_t kExpectedPayloadBytes = 1536U;
    constexpr uint32_t kExpectedDataPackets = 36U;
    constexpr uint32_t kExpectedSkipPackets = 12U;

    // The FF800 packetizer's valid 192 kHz cadence may be committed with any
    // of the four phase rotations.  Stage 5G requires the finite command list
    // itself to begin D-D-D-S, so select the phase whose first three physical
    // slots are data and whose fourth is a skip.  The previous implementation
    // assumed physical slot zero was always data; on the real run it was the
    // skip phase (S-D-D-D), causing packet 0 to be rejected before DMA.
    uint32_t cadenceStartSlot = Layout::kNumPackets;
    for (uint32_t candidate = 0U; candidate < 4U; ++candidate) {
        bool candidateValid = true;
        for (uint32_t logicalSlot = 0U;
             logicalSlot < Layout::kNumPackets;
             ++logicalSlot) {
            const uint32_t physicalSlot =
                (candidate + logicalSlot) % Layout::kNumPackets;
            const uint32_t producerSlot = physicalSlot % numSlots;
            const auto& meta = metadataRing[producerSlot];
            const uint32_t metaQ0 =
                OSSwapLittleToHostInt32(meta.immediateHeader[0]);
            const uint32_t metaQ1 =
                OSSwapLittleToHostInt32(meta.immediateHeader[1]);
            const auto tag =
                ASFW::IsochTransport::DecodeIsochTxHeaderTag(metaQ0);
            const bool actualSkip =
                ASFW::IsochTransport::ShouldSkipIsochTxPacket(
                    tag, meta.payloadLength);
            const bool expectedSkip = (logicalSlot & 0x3U) == 0x3U;
            if (actualSkip != expectedSkip ||
                tag != ASFW::IsochTransport::IsochPacketTag::kNoCipHeader ||
                ASFW::IsochTransport::DecodeIsochTxHeaderSpeed(metaQ0) !=
                    ASFW::IsochTransport::kIsochSpeedS400 ||
                ASFW::IsochTransport::DecodeIsochTxHeaderTCode(metaQ0) !=
                    ASFW::IsochTransport::kIsochDataBlockTCode ||
                ASFW::IsochTransport::DecodeIsochTxHeaderDataLength(metaQ1) !=
                    meta.payloadLength) {
                candidateValid = false;
                break;
            }
        }
        if (candidateValid) {
            cadenceStartSlot = candidate;
            break;
        }
    }

    if (cadenceStartSlot >= Layout::kNumPackets) {
        uint32_t observedSkipMask = 0U;
        for (uint32_t physicalSlot = 0U; physicalSlot < 4U; ++physicalSlot) {
            const auto& meta = metadataRing[physicalSlot % numSlots];
            const uint32_t metaQ0 =
                OSSwapLittleToHostInt32(meta.immediateHeader[0]);
            const auto tag =
                ASFW::IsochTransport::DecodeIsochTxHeaderTag(metaQ0);
            if (ASFW::IsochTransport::ShouldSkipIsochTxPacket(
                    tag, meta.payloadLength)) {
                observedSkipMask |= (1U << physicalSlot);
            }
        }
        ASFW_LOG(Isoch,
                 "IT: Stage 5G could not phase-align committed cadence observedFirst4SkipMask=0x%x expectedRotatedDDD-S=1",
                 observedSkipMask);
        return false;
    }

    ASFW_LOG(Isoch,
             "IT: Stage 5G cadence phase aligned physicalStartSlot=%u logicalPattern=DDD-S",
             cadenceStartSlot);

    uint32_t dataPackets = 0U;
    uint32_t skipPackets = 0U;
    for (uint32_t logicalSlot = 0U;
         logicalSlot < Layout::kNumPackets;
         ++logicalSlot) {
        const bool expectedSkip = (logicalSlot & 0x3U) == 0x3U;
        const uint32_t physicalSlot =
            (cadenceStartSlot + logicalSlot) % Layout::kNumPackets;
        const uint32_t producerSlot = physicalSlot % numSlots;
        const auto& meta = metadataRing[producerSlot];
        const uint32_t metaQ0 =
            OSSwapLittleToHostInt32(meta.immediateHeader[0]);
        const uint32_t metaQ1 =
            OSSwapLittleToHostInt32(meta.immediateHeader[1]);
        const auto tag = ASFW::IsochTransport::DecodeIsochTxHeaderTag(metaQ0);
        const bool actualSkip =
            ASFW::IsochTransport::ShouldSkipIsochTxPacket(
                tag, meta.payloadLength);

        if (actualSkip != expectedSkip ||
            tag != ASFW::IsochTransport::IsochPacketTag::kNoCipHeader ||
            ASFW::IsochTransport::DecodeIsochTxHeaderSpeed(metaQ0) !=
                ASFW::IsochTransport::kIsochSpeedS400 ||
            ASFW::IsochTransport::DecodeIsochTxHeaderTCode(metaQ0) !=
                ASFW::IsochTransport::kIsochDataBlockTCode ||
            ASFW::IsochTransport::DecodeIsochTxHeaderDataLength(metaQ1) !=
                meta.payloadLength) {
            ASFW_LOG(Isoch,
                     "IT: Stage 5G cadence metadata rejected logical=%u physical=%u expectedSkip=%u actualSkip=%u tag=%u speed=%u tcode=0x%x dataLength=%u/%u",
                     logicalSlot,
                     physicalSlot,
                     expectedSkip ? 1U : 0U,
                     actualSkip ? 1U : 0U,
                     static_cast<uint32_t>(tag),
                     ASFW::IsochTransport::DecodeIsochTxHeaderSpeed(metaQ0),
                     ASFW::IsochTransport::DecodeIsochTxHeaderTCode(metaQ0),
                     ASFW::IsochTransport::DecodeIsochTxHeaderDataLength(metaQ1),
                     meta.payloadLength);
            return false;
        }

        const uint32_t descBase =
            physicalSlot * Layout::kBlocksPerPacket;
        const bool isLast = logicalSlot + 1U == Layout::kNumPackets;
        const bool nextIsSkip = !isLast &&
            (((logicalSlot + 1U) & 0x3U) == 0x3U);
        const uint32_t nextZ =
            nextIsSkip ? 1U : Layout::kBlocksPerPacket;
        const uint32_t nextPhysicalSlot = isLast
            ? 0U
            : ((cadenceStartSlot + logicalSlot + 1U) %
               Layout::kNumPackets);
        const uint32_t nextDescriptorIOVA = isLast
            ? 0U
            : slab_.GetDescriptorIOVA(
                nextPhysicalSlot * Layout::kBlocksPerPacket);

        if (expectedSkip) {
            ++skipPackets;
            if (meta.payloadLength != 0U) {
                return false;
            }

            for (uint32_t block = 0U;
                 block < Layout::kBlocksPerPacket;
                 ++block) {
                auto* desc = slab_.GetDescriptorPtr(descBase + block);
                std::memset(desc, 0, sizeof(OHCIDescriptor));
            }

            auto* skip = slab_.GetDescriptorPtr(descBase);
            skip->control = OHCIDescriptor::BuildControl({
                .reqCount = 0U,
                .command = OHCIDescriptor::kCmdOutputLast,
                .key = OHCIDescriptor::kKeyStandard,
                .interruptBits = OHCIDescriptor::kIntNever,
                .branchBits = OHCIDescriptor::kBranchAlways,
            });
            skip->control |=
                (1U << (OHCIDescriptor::kStatusShift +
                        OHCIDescriptor::kControlHighShift));
            skip->dataAddress = 0U;
            skip->branchWord = isLast
                ? 0U
                : MakeFiniteCadenceBranchWord(nextDescriptorIOVA, nextZ);
            if (!isLast && skip->branchWord == 0U) {
                return false;
            }
            AR_init_status(*skip, 0U);
            continue;
        }

        ++dataPackets;
        if (meta.payloadLength != kExpectedPayloadBytes ||
            meta.payloadLength > slotStrideBytes) {
            ASFW_LOG(Isoch,
                     "IT: Stage 5G data shape rejected logical=%u physical=%u producer=%u payloadBytes=%u/%u stride=%u",
                     logicalSlot,
                     physicalSlot,
                     producerSlot,
                     meta.payloadLength,
                     kExpectedPayloadBytes,
                     slotStrideBytes);
            return false;
        }

        const uint8_t* payload = payloadBase +
            static_cast<size_t>(producerSlot) * slotStrideBytes;
        for (uint32_t byte = 0U; byte < meta.payloadLength; ++byte) {
            if (payload[byte] != 0U) {
                ASFW_LOG(Isoch,
                         "IT: Stage 5G silence safety check rejected logical=%u physical=%u producer=%u byte=%u value=0x%02x",
                         logicalSlot,
                         physicalSlot,
                         producerSlot,
                         byte,
                         payload[byte]);
                return false;
            }
        }

        auto* immDesc = reinterpret_cast<OHCIDescriptorImmediate*>(
            slab_.GetDescriptorPtr(descBase));
        auto* payloadDesc = slab_.GetDescriptorPtr(
            descBase + Layout::kFirstPayloadBlock);
        auto* completion = slab_.GetDescriptorPtr(
            descBase + Layout::kCompletionBlock);
        const uint32_t wireQ0 =
            OSSwapLittleToHostInt32(immDesc->immediateData[0]);
        const uint32_t wireQ1 =
            OSSwapLittleToHostInt32(immDesc->immediateData[1]);
        const uint8_t wireChannel =
            static_cast<uint8_t>((wireQ0 >> 8U) & 0x3FU);
        const uint32_t firstFragmentBytes =
            payloadDesc->control & 0xFFFFU;
        const uint32_t lastFragmentBytes =
            completion->control & 0xFFFFU;
        if (ASFW::IsochTransport::DecodeIsochTxHeaderTag(wireQ0) !=
                ASFW::IsochTransport::IsochPacketTag::kNoCipHeader ||
            ASFW::IsochTransport::DecodeIsochTxHeaderSpeed(wireQ0) !=
                ASFW::IsochTransport::kIsochSpeedS400 ||
            ASFW::IsochTransport::DecodeIsochTxHeaderTCode(wireQ0) !=
                ASFW::IsochTransport::kIsochDataBlockTCode ||
            ASFW::IsochTransport::DecodeIsochTxHeaderDataLength(wireQ1) !=
                kExpectedPayloadBytes ||
            wireChannel != channel_ ||
            firstFragmentBytes + lastFragmentBytes != kExpectedPayloadBytes ||
            payloadDesc->dataAddress == 0U ||
            completion->dataAddress == 0U) {
            ASFW_LOG(Isoch,
                     "IT: Stage 5G primed descriptor validation failed logical=%u physical=%u tag=%u speed=%u tcode=0x%x channel=%u/%u dataLength=%u fragments=%u+%u",
                     logicalSlot,
                     physicalSlot,
                     static_cast<uint32_t>(
                         ASFW::IsochTransport::DecodeIsochTxHeaderTag(wireQ0)),
                     ASFW::IsochTransport::DecodeIsochTxHeaderSpeed(wireQ0),
                     ASFW::IsochTransport::DecodeIsochTxHeaderTCode(wireQ0),
                     wireChannel,
                     channel_,
                     ASFW::IsochTransport::DecodeIsochTxHeaderDataLength(wireQ1),
                     firstFragmentBytes,
                     lastFragmentBytes);
            return false;
        }

        const uint32_t intMask = 0x3U <<
            (OHCIDescriptor::kIntShift +
             OHCIDescriptor::kControlHighShift);
        completion->control &= ~intMask;
        completion->branchWord = isLast
            ? 0U
            : MakeFiniteCadenceBranchWord(nextDescriptorIOVA, nextZ);
        if (!isLast && completion->branchWord == 0U) {
            return false;
        }
        AR_init_status(*completion, 0U);
        payloadDesc->statusWord = 0U;
        immDesc->common.statusWord = 0U;

        if (dmaMemory_) {
            dmaMemory_->PublishToDevice(
                reinterpret_cast<const std::byte*>(payload),
                meta.payloadLength);
        }
    }

    if (dataPackets != kExpectedDataPackets ||
        skipPackets != kExpectedSkipPackets) {
        return false;
    }

    if (dmaMemory_) {
        dmaMemory_->PublishBarrier();
        dmaMemory_->PublishToDevice(
            slab_.DescriptorRegion().virtualBase,
            slab_.DescriptorRegion().size);
    } else {
        ASFW::Driver::WriteBarrier();
    }

    const uint64_t descriptorIOVA = slab_.GetDescriptorIOVA(
        cadenceStartSlot * Layout::kBlocksPerPacket);
    if (descriptorIOVA == 0U || descriptorIOVA > 0xFFFFFFFFULL) {
        return false;
    }

    for (uint32_t logicalSlot = 0U;
         logicalSlot < Layout::kNumPackets;
         ++logicalSlot) {
        const bool isSkip = (logicalSlot & 0x3U) == 0x3U;
        const uint32_t physicalSlot =
            (cadenceStartSlot + logicalSlot) % Layout::kNumPackets;
        const auto* branchOwner = isSkip
            ? slab_.GetDescriptorPtr(
                physicalSlot * Layout::kBlocksPerPacket)
            : slab_.GetDescriptorPtr(
                physicalSlot * Layout::kBlocksPerPacket +
                Layout::kCompletionBlock);
        const bool isLast = logicalSlot + 1U == Layout::kNumPackets;
        const bool nextIsSkip = !isLast &&
            (((logicalSlot + 1U) & 0x3U) == 0x3U);
        const uint32_t nextPhysicalSlot = isLast
            ? 0U
            : ((cadenceStartSlot + logicalSlot + 1U) %
               Layout::kNumPackets);
        const uint32_t expectedBranch = isLast
            ? 0U
            : MakeFiniteCadenceBranchWord(
                slab_.GetDescriptorIOVA(
                    nextPhysicalSlot * Layout::kBlocksPerPacket),
                nextIsSkip ? 1U : Layout::kBlocksPerPacket);
        if (branchOwner->branchWord != expectedBranch ||
            (!isLast && expectedBranch == 0U)) {
            ASFW_LOG(Isoch,
                     "IT: Stage 5G finite branch validation failed logical=%u physical=%u skip=%u actual=0x%08x expected=0x%08x",
                     logicalSlot,
                     physicalSlot,
                     isSkip ? 1U : 0U,
                     branchOwner->branchWord,
                     expectedBranch);
            return false;
        }
    }

    outProgram.commandPtr =
        static_cast<uint32_t>(descriptorIOVA) |
        Layout::kBlocksPerPacket;
    outProgram.startPacketSlot = cadenceStartSlot;
    outProgram.descriptorCount = Layout::kNumPackets;
    outProgram.dataPacketCount = dataPackets;
    outProgram.skipPacketCount = skipPackets;
    outProgram.payloadLength = kExpectedPayloadBytes;
    return true;
}

bool IsochTxDmaRing::ProgramBoundedCircularSilenceCadenceForPreflight(
    uint8_t* payloadBase,
    const uint32_t numSlots,
    const uint32_t slotStrideBytes,
    const ASFW::IsochTransport::TxPacketMeta* metadataRing,
    BoundedCircularSilenceCadenceProgram& outProgram) noexcept {
    outProgram = {};

    FiniteSilenceCadenceProgram finite{};
    if (!ProgramFiniteSilenceCadenceForRunPreflight(
            payloadBase,
            numSlots,
            slotStrideBytes,
            metadataRing,
            finite)) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5H could not construct the Stage 5G-validated silence cadence");
        return false;
    }

    if (finite.descriptorCount != Layout::kNumPackets ||
        finite.dataPacketCount != 36U ||
        finite.skipPacketCount != 12U ||
        finite.payloadLength != 1536U ||
        finite.startPacketSlot >= Layout::kNumPackets) {
        return false;
    }

    // Logical slot 47 is always the skip in the twelfth D-D-D-S group. Its
    // one-descriptor program must advertise Z=4 when branching back to the
    // first logical slot, which is guaranteed to be a normal data program.
    const uint32_t lastPhysicalSlot =
        (finite.startPacketSlot + Layout::kNumPackets - 1U) %
        Layout::kNumPackets;
    auto* lastSkip = slab_.GetDescriptorPtr(
        lastPhysicalSlot * Layout::kBlocksPerPacket);
    const uint32_t firstDescriptorIOVA = slab_.GetDescriptorIOVA(
        finite.startPacketSlot * Layout::kBlocksPerPacket);
    const uint32_t circularBranch = MakeFiniteCadenceBranchWord(
        firstDescriptorIOVA, Layout::kBlocksPerPacket);
    if (circularBranch == 0U) {
        return false;
    }
    lastSkip->branchWord = circularBranch;

    if (dmaMemory_) {
        dmaMemory_->PublishBarrier();
        dmaMemory_->PublishToDevice(
            slab_.DescriptorRegion().virtualBase,
            slab_.DescriptorRegion().size);
    } else {
        ASFW::Driver::WriteBarrier();
    }

    // Re-read every branch owner after publication. The first 47 links retain
    // Stage 5G's phase-correct Z values; only the final skip is now circular.
    for (uint32_t logicalSlot = 0U;
         logicalSlot < Layout::kNumPackets;
         ++logicalSlot) {
        const bool isSkip = (logicalSlot & 0x3U) == 0x3U;
        const uint32_t physicalSlot =
            (finite.startPacketSlot + logicalSlot) % Layout::kNumPackets;
        const auto* branchOwner = isSkip
            ? slab_.GetDescriptorPtr(
                physicalSlot * Layout::kBlocksPerPacket)
            : slab_.GetDescriptorPtr(
                physicalSlot * Layout::kBlocksPerPacket +
                Layout::kCompletionBlock);
        const uint32_t nextLogicalSlot =
            (logicalSlot + 1U) % Layout::kNumPackets;
        const uint32_t nextPhysicalSlot =
            (finite.startPacketSlot + nextLogicalSlot) %
            Layout::kNumPackets;
        const bool nextIsSkip = (nextLogicalSlot & 0x3U) == 0x3U;
        const uint32_t expectedBranch = MakeFiniteCadenceBranchWord(
            slab_.GetDescriptorIOVA(
                nextPhysicalSlot * Layout::kBlocksPerPacket),
            nextIsSkip ? 1U : Layout::kBlocksPerPacket);
        if (expectedBranch == 0U ||
            branchOwner->branchWord != expectedBranch) {
            ASFW_LOG(Isoch,
                     "IT: Stage 5H circular branch validation failed logical=%u physical=%u skip=%u actual=0x%08x expected=0x%08x",
                     logicalSlot,
                     physicalSlot,
                     isSkip ? 1U : 0U,
                     branchOwner->branchWord,
                     expectedBranch);
            return false;
        }
    }

    outProgram.commandPtr = finite.commandPtr;
    outProgram.startPacketSlot = finite.startPacketSlot;
    outProgram.descriptorCount = finite.descriptorCount;
    outProgram.dataPacketCount = finite.dataPacketCount;
    outProgram.skipPacketCount = finite.skipPacketCount;
    outProgram.payloadLength = finite.payloadLength;

    ASFW_LOG(Isoch,
             "IT: ✅ Stage 5H bounded circular descriptor ring published descriptors=%u dataPackets=%u skip=%u payloadBytes=%u pattern=DDD-S finalBranchToStart=1 interrupts=0 refill=0 dmaPublish=1",
             outProgram.descriptorCount,
             outProgram.dataPacketCount,
             outProgram.skipPacketCount,
             outProgram.payloadLength);
    return true;
}

IsochTxDmaRing::CadenceAnchorCompletion
IsochTxDmaRing::FetchCadenceAnchorCompletionForPreflight(
    const uint32_t startPacketSlot) noexcept {
    CadenceAnchorCompletion out{};
    if (!slab_.IsValid() || startPacketSlot >= Layout::kNumPackets) {
        return out;
    }
    const auto* completion = slab_.GetDescriptorPtr(
        startPacketSlot * Layout::kBlocksPerPacket +
        Layout::kCompletionBlock);
    if (dmaMemory_) {
        dmaMemory_->FetchFromDevice(
            reinterpret_cast<const std::byte*>(completion),
            sizeof(*completion));
    } else {
        ASFW::Driver::ReadBarrier();
    }
    out.transferStatus = AT_xferStatus(*completion);
    out.timestamp = AT_timeStamp(*completion);
    return out;
}

IsochTxDmaRing::FiniteSilenceCadenceCompletion
IsochTxDmaRing::FetchFiniteSilenceCadenceCompletionForRunPreflight(
    const uint32_t startPacketSlot) noexcept {
    FiniteSilenceCadenceCompletion out{};
    if (!slab_.IsValid() || startPacketSlot >= Layout::kNumPackets) {
        return out;
    }

    const auto region = slab_.DescriptorRegion();
    if (dmaMemory_) {
        dmaMemory_->FetchFromDevice(region.virtualBase, region.size);
    } else {
        ASFW::Driver::ReadBarrier();
    }

    uint32_t dataIndex = 0U;
    for (uint32_t logicalSlot = 0U;
         logicalSlot < Layout::kNumPackets;
         ++logicalSlot) {
        const bool isSkip = (logicalSlot & 0x3U) == 0x3U;
        const uint32_t physicalSlot =
            (startPacketSlot + logicalSlot) % Layout::kNumPackets;
        const auto* completion = isSkip
            ? slab_.GetDescriptorPtr(
                physicalSlot * Layout::kBlocksPerPacket)
            : slab_.GetDescriptorPtr(
                physicalSlot * Layout::kBlocksPerPacket +
                Layout::kCompletionBlock);
        const uint16_t transferStatus = AT_xferStatus(*completion);
        const uint16_t timestamp = AT_timeStamp(*completion);
        if (transferStatus != 0U) {
            ++out.completedDescriptors;
            const uint8_t eventCode =
                static_cast<uint8_t>(transferStatus & 0x1FU);
            if (eventCode != 0x11U) {
                ++out.eventErrors;
            }
            if (!isSkip) {
                ++out.completedDataDescriptors;
                ++out.actualBusPackets;
                if (dataIndex < out.dataTimestamps.size()) {
                    out.dataTimestamps[dataIndex] = timestamp;
                }
                ++dataIndex;
            }
        }
        if (logicalSlot + 1U == Layout::kNumPackets) {
            out.lastTransferStatus = transferStatus;
            out.lastTimestamp = timestamp;
        }
    }

    if (out.completedDataDescriptors == out.dataTimestamps.size()) {
        out.cadenceValid = true;
        uint32_t previousCycle =
            (out.dataTimestamps[0] & 0x1FFFU) % 8000U;
        for (uint32_t data = 1U;
             data < out.dataTimestamps.size();
             ++data) {
            const uint32_t cycle =
                (out.dataTimestamps[data] & 0x1FFFU) % 8000U;
            const uint32_t gap =
                (cycle + 8000U - previousCycle) % 8000U;
            const uint32_t expectedGap =
                (data % 3U) == 0U ? 2U : 1U;
            if (gap != expectedGap) {
                out.cadenceValid = false;
                break;
            }
            previousCycle = cycle;
        }
    }
    return out;
}

bool IsochTxDmaRing::ProgramAllSkipPacketsForRunPreflight() noexcept {
    if (!slab_.IsValid()) {
        return false;
    }

    for (uint32_t packetSlot = 0;
         packetSlot < Layout::kNumPackets;
         ++packetSlot) {
        const uint32_t descBase = packetSlot * Layout::kBlocksPerPacket;
        for (uint32_t block = 0; block < Layout::kBlocksPerPacket; ++block) {
            auto* desc = slab_.GetDescriptorPtr(descBase + block);
            std::memset(desc, 0, sizeof(OHCIDescriptor));
        }

        auto* skip = slab_.GetDescriptorPtr(descBase);
        const bool isLast = packetSlot + 1U == Layout::kNumPackets;
        const uint32_t nextDescriptorIOVA = isLast
            ? 0U
            : slab_.GetDescriptorIOVA(
                (packetSlot + 1U) * Layout::kBlocksPerPacket);

        // A queued OHCI transmit skip is a one-descriptor program (Z=1),
        // even though ASFW reserves four physical descriptor slots per audio
        // cycle.  Advertising Z=4 made the controller consume the first
        // OUTPUT_LAST and then interpret the three zero padding descriptors as
        // part of the same program, so only descriptor 0 ever completed.
        skip->control = OHCIDescriptor::BuildControl({
            .reqCount = 0,
            .command = OHCIDescriptor::kCmdOutputLast,
            .key = OHCIDescriptor::kKeyStandard,
            .interruptBits = OHCIDescriptor::kIntNever,
            .branchBits = OHCIDescriptor::kBranchAlways,
        });
        skip->control |=
            (1u << (OHCIDescriptor::kStatusShift +
                    OHCIDescriptor::kControlHighShift));
        skip->dataAddress = 0U;
        skip->branchWord = isLast
            ? 0U
            : ((nextDescriptorIOVA & 0xFFFFFFF0U) | 1U);
        AR_init_status(*skip, 0U);
    }

    // Publish the complete finite descriptor chain before CommandPtr/RUN.
    if (dmaMemory_) {
        dmaMemory_->PublishToDevice(
            slab_.DescriptorRegion().virtualBase,
            slab_.DescriptorRegion().size);
    } else {
        ASFW::Driver::WriteBarrier();
    }

    for (uint32_t packetSlot = 0;
         packetSlot < Layout::kNumPackets;
         ++packetSlot) {
        if (!IsSkipDescriptor(packetSlot)) {
            return false;
        }
        const auto* desc = slab_.GetDescriptorPtr(
            packetSlot * Layout::kBlocksPerPacket);
        const bool isLast = packetSlot + 1U == Layout::kNumPackets;
        if (isLast) {
            if (desc->branchWord != 0U) {
                return false;
            }
        } else if (desc->branchWord == 0U ||
                   (desc->branchWord & 0xFU) != 1U) {
            return false;
        }
    }
    return true;
}

IsochTxDmaRing::AllSkipCompletionSnapshot
IsochTxDmaRing::FetchAllSkipCompletionForRunPreflight() noexcept {
    AllSkipCompletionSnapshot out{};
    if (!slab_.IsValid()) {
        return out;
    }

    const auto region = slab_.DescriptorRegion();
    if (dmaMemory_) {
        dmaMemory_->FetchFromDevice(region.virtualBase, region.size);
    } else {
        ASFW::Driver::ReadBarrier();
    }

    for (uint32_t packetSlot = 0;
         packetSlot < Layout::kNumPackets;
         ++packetSlot) {
        const auto* desc = slab_.GetDescriptorPtr(
            packetSlot * Layout::kBlocksPerPacket);
        if (AT_xferStatus(*desc) != 0U) {
            ++out.completedDescriptors;
        }
        if (packetSlot + 1U == Layout::kNumPackets) {
            out.lastTransferStatus = AT_xferStatus(*desc);
            out.lastTimestamp = AT_timeStamp(*desc);
        }
    }
    return out;
}

bool IsochTxDmaRing::IsSkipDescriptor(
    const uint32_t packetSlot) const noexcept {
    const auto* desc = slab_.GetDescriptorPtr(
        packetSlot * Layout::kBlocksPerPacket);
    const uint32_t controlHigh =
        desc->control >> OHCIDescriptor::kControlHighShift;
    const uint8_t command = static_cast<uint8_t>(
        (controlHigh >> OHCIDescriptor::kCmdShift) & 0xFU);
    const uint8_t key = static_cast<uint8_t>(
        (controlHigh >> OHCIDescriptor::kKeyShift) & 0x7U);
    const uint16_t requestCount =
        static_cast<uint16_t>(desc->control & 0xFFFFU);
    return command == OHCIDescriptor::kCmdOutputLast &&
           key == OHCIDescriptor::kKeyStandard &&
           requestCount == 0U;
}

IsochTxDmaRing::OHCIDescriptor*
IsochTxDmaRing::CompletionDescriptorForPacket(
    const uint32_t packetSlot) noexcept {
    if (IsSkipDescriptor(packetSlot)) {
        return slab_.GetDescriptorPtr(
            packetSlot * Layout::kBlocksPerPacket);
    }
    return slab_.GetDescriptorPtr(
        packetSlot * Layout::kBlocksPerPacket +
        Layout::kCompletionBlock);
}

IsochTxDmaRing::PrimeStats IsochTxDmaRing::Prime(
    const TxPayloadDmaMap& payloadDmaMap,
    const uint32_t numSlots,
    const uint32_t slotStrideBytes,
    const ASFW::IsochTransport::TxPacketMeta* metadataRing,
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
            ASFW::IsochTransport::ExpectedCommitGen(pktIdx, numSlots);
        if (meta.commitGen.load(std::memory_order_acquire) != expectedGen) {
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

        const uint32_t headerQ0 =
            OSSwapLittleToHostInt32(meta.immediateHeader[0]);
        const auto packetTag =
            ASFW::IsochTransport::DecodeIsochTxHeaderTag(headerQ0);
        if (ASFW::IsochTransport::ShouldSkipIsochTxPacket(
                packetTag, meta.payloadLength)) {
            ProgramSkipPacket(pktIdx);
            continue;
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

    stats.packetsAssembled = numPackets;
    ASFW_LOG(Isoch, "IT: Dynamic descriptor ring primed. numPackets=%u softwareFillIdx=%llu",
             numPackets, softwareFillAbsIdx_);
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

const char* IsochTxDmaRing::RefillFailureReasonName(
    RefillFailureReason reason) noexcept {
    switch (reason) {
        case RefillFailureReason::None:
            return "none";
        case RefillFailureReason::InvalidSharedContract:
            return "invalid-shared-contract";
        case RefillFailureReason::DeadContext:
            return "dead-context";
        case RefillFailureReason::ProducerFatalStatus:
            return "producer-fatal-status";
        case RefillFailureReason::CommandPointerDecode:
            return "command-pointer-decode";
        case RefillFailureReason::UncommittedSlot:
            return "uncommitted-slot";
        case RefillFailureReason::InvalidPacketSize:
            return "invalid-packet-size";
        case RefillFailureReason::PayloadMapping:
            return "payload-mapping";
    }
    return "unknown";
}

IsochTxDmaRing::RefillOutcome IsochTxDmaRing::Refill(
    Driver::HardwareInterface& hw,
    uint8_t contextIndex,
    ASFW::IsochTransport::TxPacketMeta* metadataRing,
    ASFW::IsochTransport::TxStreamControl* controlBlock,
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

    // 1. Capture CYCLE_TIMER and host time, and publish under seqlock.
    // Clock-smoothing / filtering is handled natively by AudioDriverKit's clock algorithms.
    const uint32_t refillCycleTimer =
        hw.Read(static_cast<Register32>(Register32::kCycleTimer));
    {
        const uint64_t hostTime = mach_absolute_time();

        ASFW::IsochTransport::ClockPairSample sample{};
        sample.hostTimeMid = hostTime;
        sample.cycleTimer32 = refillCycleTimer;
        controlBlock->clockPair.Publish(sample);
    }

    // 2. Check context status
    const Register32 ctrlReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(contextIndex));
    const uint32_t ctrl = hw.Read(ctrlReg);
    out.contextControl = ctrl;
    const bool dead = (ctrl & Driver::ContextControl::kDead) != 0;
    if (dead) {
        counters_.exitDead.fetch_add(1, std::memory_order_relaxed);
        controlBlock->statusWord.store(ASFW::IsochTransport::TxStreamStatus::kDeadContext, std::memory_order_release);
        controlBlock->streamGeneration.fetch_add(1, std::memory_order_release);
        out.dead = true;
        out.failureReason = RefillFailureReason::DeadContext;
        out.streamStatus = static_cast<uint32_t>(
            ASFW::IsochTransport::TxStreamStatus::kDeadContext);
        return out;
    }
    const auto streamStatus =
        controlBlock->statusWord.load(std::memory_order_acquire);
    out.streamStatus = static_cast<uint32_t>(streamStatus);
    if (streamStatus ==
        ASFW::IsochTransport::TxStreamStatus::kUnderrunFatal) {
        out.failureReason = RefillFailureReason::ProducerFatalStatus;
        out.producerFailureAvailable =
            controlBlock->producerFailure.TryRead(
                out.producerFailure);
        return out;
    }

    // 3. Decode hardware pointer and advance completed cursor
    uint32_t hwPacketIndex = 0;
    uint32_t cmdPtr = 0;
    if (!DecodeHardwarePacketIndex(hw, contextIndex, hwPacketIndex, cmdPtr)) {
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

    const uint32_t deltaConsumed = ComputeDeltaConsumed(hwPacketIndex);
    const uint32_t gap = ringPacketsAhead_;
    UpdateGapCounters(gap);
    ResyncCycleTracking(hw, hwPacketIndex, deltaConsumed, out);

    // Track the worst single coalesced completion. The committed slack
    // (kTxPreparationSlackPackets) must cover this; a new high-water at or above
    // the slack is one phase-slip away from an underrun, so surface it.
    {
        uint32_t prevMax =
            counters_.maxDeltaConsumed.load(std::memory_order_relaxed);
        while (deltaConsumed > prevMax &&
               !counters_.maxDeltaConsumed.compare_exchange_weak(
                   prevMax, deltaConsumed, std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
        constexpr uint32_t kSlack =
            ASFW::IsochTransport::AudioTimingGeometry::
                kTxPreparationSlackPackets;
        if (deltaConsumed > prevMax && deltaConsumed * 2 >= kSlack) {
            ASFW_LOG(
                Isoch,
                "IT deltaConsumed high-water=%u (slack budget=%u) — coalesced "
                "completion approaching the coverage bound",
                deltaConsumed,
                kSlack);
        }
    }

    // Fetch and publish completed stamps
    const uint64_t completedAbsIdx = controlBlock->completionCursor.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < deltaConsumed; ++i) {
        const uint64_t currentAbsIdx = completedAbsIdx + i;
        const uint32_t completedPktSlot = static_cast<uint32_t>(currentAbsIdx % Layout::kNumPackets);

        auto* firstDesc = slab_.GetDescriptorPtr(
            completedPktSlot * Layout::kBlocksPerPacket);
        if (dmaMemory_) {
            dmaMemory_->FetchFromDevice(
                reinterpret_cast<const std::byte*>(firstDesc),
                sizeof(*firstDesc));
        }
        auto* completionDesc =
            CompletionDescriptorForPacket(completedPktSlot);
        if (dmaMemory_ && completionDesc != firstDesc) {
            dmaMemory_->FetchFromDevice(
                reinterpret_cast<const std::byte*>(completionDesc),
                sizeof(*completionDesc));
        }

        const uint16_t hwTimestamp = static_cast<uint16_t>(
            completionDesc->statusWord & 0xFFFFU);

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
            controlBlock->preparationRequestGeneration.load(
                std::memory_order_relaxed);
        const uint64_t handled =
            controlBlock->preparationHandledGeneration.load(
                std::memory_order_acquire);
        if (requested == handled) {
            const uint64_t generation = requested + 1;
            controlBlock->preparationRequestHostTicks.store(
                mach_absolute_time(), std::memory_order_relaxed);
            controlBlock->preparationRequestGeneration.store(
                generation, std::memory_order_release);
            controlBlock->preparationRequestCount.fetch_add(
                1, std::memory_order_relaxed);
            out.preparationRequestGeneration = generation;
        } else {
            controlBlock->preparationCoalescedCount.fetch_add(
                1, std::memory_order_relaxed);
        }
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

        if (commitGen != expectedGen) {
            const uint64_t requestGeneration =
                controlBlock->preparationRequestGeneration.load(
                    std::memory_order_acquire);
            const uint64_t handledGeneration =
                controlBlock->preparationHandledGeneration.load(
                    std::memory_order_acquire);
            const uint64_t requestHostTicks =
                controlBlock->preparationRequestHostTicks.load(
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
                ASFW::IsochTransport::TxStreamStatus::kUnderrunFatal,
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
            // lap behind => the producer's exposeCursor never reached this
            // packet (coverage/margin). The producer cursors localize it.
            ASFW_LOG(
                Isoch,
                "IT FATAL dump: fatalAbs=%llu slot=%u expectedGen=%llu commitGen=%llu "
                "slotLastPacketAbs=%llu exposeCursor=%llu completionCursor=%llu "
                "softwareFillAbs=%llu ringPacketsAhead=%u deltaConsumed=%u i=%u numSlots=%u "
                "prepReq=%llu prepHandled=%llu prepAgeUs=%llu prepCoalesced=%llu",
                fillAbsIdx,
                pktSlot,
                expectedGen,
                commitGen,
                meta.packetIndex,
                controlBlock->exposeCursor.load(std::memory_order_acquire),
                controlBlock->completionCursor.load(std::memory_order_acquire),
                softwareFillAbsIdx_,
                ringPacketsAhead_,
                deltaConsumed,
                i,
                numSlots,
                requestGeneration,
                handledGeneration,
                requestAgeUs,
                controlBlock->preparationCoalescedCount.load(
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

        const uint32_t headerQ0 =
            OSSwapLittleToHostInt32(meta.immediateHeader[0]);
        const auto packetTag =
            ASFW::IsochTransport::DecodeIsochTxHeaderTag(headerQ0);
        if (ASFW::IsochTransport::ShouldSkipIsochTxPacket(
                packetTag, payloadLength)) {
            ProgramSkipPacket(hwSlot);
            if (dmaMemory_) {
                for (uint32_t block = 0;
                     block < Layout::kBlocksPerPacket;
                     ++block) {
                    const auto* desc = slab_.GetDescriptorPtr(
                        hwSlot * Layout::kBlocksPerPacket + block);
                    dmaMemory_->PublishToDevice(
                        reinterpret_cast<const std::byte*>(desc),
                        sizeof(*desc));
                }
            }
            ++packetsFilled;
            continue;
        }

        if (payloadBase) {
            GaugeWirePayload(fillAbsIdx,
                             payloadBase + payloadOffset,
                             payloadLength,
                             meta.immediateHeader[0]);
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
        std::memset(immDesc, 0, sizeof(OHCIDescriptorImmediate));
        immDesc->common.control = OHCIDescriptor::BuildControl({
            .reqCount = 8,
            .command = OHCIDescriptor::kCmdOutputMore,
            .key = OHCIDescriptor::kKeyImmediate,
            .interruptBits = OHCIDescriptor::kIntNever,
            .branchBits = OHCIDescriptor::kBranchNever,
        });
        immDesc->immediateData[0] =
            StampHeaderChannel(meta.immediateHeader[0], channel_);
        immDesc->immediateData[1] = meta.immediateHeader[1];
        immDesc->common.branchWord = MakeBranchWordAT(
            slab_.GetDescriptorIOVA(descBase),
            Layout::kBlocksPerPacket);

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
        desc2->branchWord = 0;
        desc2->statusWord = 0;

        auto* desc3 =
            slab_.GetDescriptorPtr(descBase + Layout::kCompletionBlock);
        std::memset(desc3, 0, sizeof(OHCIDescriptor));
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
