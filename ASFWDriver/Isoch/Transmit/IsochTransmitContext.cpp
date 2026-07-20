// IsochTransmitContext.cpp
// ASFW - Isochronous Transmit Context (orchestrator)
//
// NOTE:
// OHCI IT programming details are in Tx::IsochTxDmaRing.
//

#include "IsochTransmitContext.hpp"
#include "../../Hardware/OHCIConstants.hpp"
#include "../../Logging/LogConfig.hpp"
#include "../../Common/TimingUtils.hpp"

#include <DriverKit/IOLib.h>
#include <algorithm>
#include <cstring>

namespace ASFW::Isoch {

using namespace ASFW::Driver;

namespace {

const char* TxStateName(ITState state) noexcept {
    switch (state) {
    case ITState::Unconfigured:
        return "unconfigured";
    case ITState::Configured:
        return "configured";
    case ITState::Running:
        return "running";
    case ITState::Stopped:
        return "stopped";
    }
    return "unknown";
}

} // namespace

std::unique_ptr<IsochTransmitContext> IsochTransmitContext::Create(
    Driver::HardwareInterface* hw,
    std::shared_ptr<Memory::IIsochDMAMemory> dmaMemory) noexcept {

    auto ctx = std::make_unique<IsochTransmitContext>();
    if (!ctx) return nullptr;

    ctx->hardware_ = hw;
    ctx->dmaMemory_ = std::move(dmaMemory);

    return ctx;
}

IsochTransmitContext::~IsochTransmitContext() noexcept = default;

kern_return_t IsochTransmitContext::Configure(uint8_t channel, uint8_t sid) noexcept {
    if (state_ != State::Unconfigured && state_ != State::Stopped) {
        ASFW_LOG(Isoch, "IT: Configure rejected - state=%{public}s", TxStateName(state_));
        return kIOReturnBusy;
    }

    channel_ = channel;
    ring_.SetChannel(channel_);
    finiteCadencePrepared_ = false;
    finiteCadenceProgram_ = {};
    boundedCircularCadencePrepared_ = false;
    boundedCircularCadenceProgram_ = {};

    if (dmaMemory_) {
        // Allocate-once policy
        if (!ring_.HasRings()) {
            const kern_return_t kr = ring_.SetupRings(*dmaMemory_);
            if (kr != kIOReturnSuccess) {
                ASFW_LOG(Isoch, "IT: SetupRings failed");
                return kr;
            }
        }
    }

    state_ = State::Configured;
    ASFW_LOG(Isoch, "IT: Configured ch=%u sid=%u", channel, sid);
    return kIOReturnSuccess;
}

kern_return_t IsochTransmitContext::SetSharedMemoryDescriptors(
    IOMemoryDescriptor* payloadSlab,
    IOMemoryDescriptor* metadataRing,
    IOMemoryDescriptor* controlBlock,
    uint32_t interruptInterval,
    uint32_t ztsPeriodFrames) noexcept {

    if (!payloadSlab || !metadataRing || !controlBlock) {
        return kIOReturnBadArgument;
    }

    // Unmap any existing maps first
    payloadMap_ = nullptr;
    metadataMap_ = nullptr;
    controlMap_ = nullptr;
    payloadBase_ = nullptr;
    metadataRing_ = nullptr;
    controlBlock_ = nullptr;
    if (payloadDmaCmd_) {
        payloadDmaCmd_->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        payloadDmaCmd_ = nullptr;
    }
    payloadDmaMap_.Reset();
    finiteCadencePrepared_ = false;
    finiteCadenceProgram_ = {};
    boundedCircularCadencePrepared_ = false;
    boundedCircularCadenceProgram_ = {};

    // 1. Map Payload Slab
    IOMemoryMap* pMap = nullptr;
    kern_return_t kr = payloadSlab->CreateMapping(0, 0, 0, 0, 0, &pMap);
    if (kr != kIOReturnSuccess || !pMap) {
        ASFW_LOG(Isoch, "IT: Failed to map payload slab: 0x%08x", kr);
        return kr;
    }
    payloadMap_ = OSSharedPtr<IOMemoryMap>(pMap, OSNoRetain);
    payloadBase_ = reinterpret_cast<uint8_t*>(payloadMap_->GetAddress());

    // Prepare Payload Slab for DMA (to get IOVA)
    if (hardware_) {
        auto dmaCmd = hardware_->CreateDMACommand();
        if (!dmaCmd) {
            ASFW_LOG(Isoch, "IT: Failed to create DMA command for payload slab");
            return kIOReturnNoMemory;
        }

        // CRITICAL / DriverKit RPC Limit:
        // IODMACommand::PrepareForDMA has a fixed signature of:
        //   PrepareForDMA(..., IOAddressSegment segments[32])
        // If we initialize segmentCount to segments.size() (64) and pass it, the DriverKit
        // user-kernel RPC serialization stub flags the count > 32 as an array bounds/overrun violation
        // and immediately aborts the syscall returning kIOReturnOverrun (0xe00002e8).
        //
        // Thus, segmentCount MUST be initialized to at most 32. Since the 256KB payload slab is
        // only 16 pages under Apple Silicon's 16KB page layout, it easily fits in 32 segments.
        std::array<IOAddressSegment, 32> segments{};
        uint32_t segmentCount = std::min<uint32_t>(segments.size(), 32);
        uint64_t flags = 0;
        uint64_t slabLen = 0;
        payloadSlab->GetLength(&slabLen);

        kr = dmaCmd->PrepareForDMA(kIODMACommandPrepareForDMANoOptions, payloadSlab, 0, slabLen, &flags,
                                    &segmentCount, segments.data());
        if (kr != kIOReturnSuccess) {
            ASFW_LOG(Isoch, "IT: PrepareForDMA failed for payload slab: 0x%08x", kr);
            return kr;
        }

        std::array<Tx::TxPayloadDmaSegment, Tx::TxPayloadDmaMap::kMaxSegments>
            payloadSegments{};
        if (segmentCount == 0 || segmentCount > payloadSegments.size()) {
            ASFW_LOG(Isoch,
                     "IT: Payload DMA returned invalid segment count=%u capacity=%zu",
                     segmentCount,
                     payloadSegments.size());
            dmaCmd->CompleteDMA(kIODMACommandCompleteDMANoOptions);
            return kIOReturnInternalError;
        }
        for (uint32_t i = 0; i < segmentCount; ++i) {
            payloadSegments[i] = Tx::TxPayloadDmaSegment{
                .deviceAddress = segments[i].address,
                .length = segments[i].length,
            };
        }

        if (!payloadDmaMap_.Configure(
                std::span<const Tx::TxPayloadDmaSegment>(
                    payloadSegments.data(), segmentCount),
                slabLen)) {
            ASFW_LOG(
                Isoch,
                "IT: Payload DMA map does not provide complete 32-bit slab coverage bytes=%llu segments=%u",
                slabLen,
                segmentCount);
            dmaCmd->CompleteDMA(kIODMACommandCompleteDMANoOptions);
            return kIOReturnInternalError;
        }

        payloadDmaCmd_ = std::move(dmaCmd);
        ASFW_LOG(Isoch,
                 "IT: Payload DMA mapped bytes=%llu segments=%zu",
                 payloadDmaMap_.SlabLength(),
                 payloadDmaMap_.SegmentCount());
        for (std::size_t i = 0; i < payloadDmaMap_.SegmentCount(); ++i) {
            const auto* segment = payloadDmaMap_.SegmentAt(i);
            ASFW_LOG(Isoch,
                     "IT: Payload DMA segment[%zu] slabOffset=%llu iova=0x%llx length=%llu",
                     i,
                     segment->slabOffset,
                     segment->deviceAddress,
                     segment->length);
        }
    }

    // 2. Map Metadata Ring
    IOMemoryMap* mMap = nullptr;
    kr = metadataRing->CreateMapping(0, 0, 0, 0, 0, &mMap);
    if (kr != kIOReturnSuccess || !mMap) {
        ASFW_LOG(Isoch, "IT: Failed to map metadata ring: 0x%08x", kr);
        return kr;
    }
    metadataMap_ = OSSharedPtr<IOMemoryMap>(mMap, OSNoRetain);
    metadataRing_ = reinterpret_cast<ASFW::IsochTransport::TxPacketMeta*>(metadataMap_->GetAddress());

    // 3. Map Control Block
    IOMemoryMap* cMap = nullptr;
    kr = controlBlock->CreateMapping(0, 0, 0, 0, 0, &cMap);
    if (kr != kIOReturnSuccess || !cMap) {
        ASFW_LOG(Isoch, "IT: Failed to map control block: 0x%08x", kr);
        return kr;
    }
    controlMap_ = OSSharedPtr<IOMemoryMap>(cMap, OSNoRetain);
    controlBlock_ = reinterpret_cast<ASFW::IsochTransport::TxStreamControl*>(controlMap_->GetAddress());

    // Populate structural fields
    uint64_t metadataLen = 0;
    metadataRing->GetLength(&metadataLen);
    uint64_t payloadLen = 0;
    payloadSlab->GetLength(&payloadLen);

    const uint32_t numSlots = static_cast<uint32_t>(metadataLen / sizeof(ASFW::IsochTransport::TxPacketMeta));
    const uint32_t maxPacketBytes = static_cast<uint32_t>(payloadLen / numSlots);

    controlBlock_->abiVersion = ASFW::IsochTransport::kTransportAbiVersion;
    controlBlock_->numSlots = numSlots;
    controlBlock_->slotStrideBytes = maxPacketBytes;
    controlBlock_->maxPacketBytes = maxPacketBytes;
    controlBlock_->interruptInterval = interruptInterval;
    controlBlock_->ztsPeriodFrames = ztsPeriodFrames;

    // Reset consumer-owned runtime counters / status only. exposeCursor is
    // PRODUCER-owned (the audio side advances it as it commits slots) and the
    // audio-side prefill has already run by the time this consumer maps the
    // block — resetting it here would stomp the prefill's committed lead back to
    // zero, desyncing the pump's lead math and starving the ring after the
    // prefilled packets drain. The buffer is freshly allocated (zero-filled)
    // each start, so exposeCursor is 0 unless the prefill legitimately set it.
    controlBlock_->streamGeneration.store(0, std::memory_order_relaxed);
    controlBlock_->statusWord.store(ASFW::IsochTransport::TxStreamStatus::kStopped, std::memory_order_relaxed);
    controlBlock_->completionCursor.store(0, std::memory_order_relaxed);
    controlBlock_->completionStampCount.store(0, std::memory_order_relaxed);
    controlBlock_->preparationRequestGeneration.store(
        0, std::memory_order_relaxed);
    controlBlock_->preparationHandledGeneration.store(
        0, std::memory_order_relaxed);
    controlBlock_->preparationRequestHostTicks.store(
        0, std::memory_order_relaxed);
    controlBlock_->preparationRequestCount.store(
        0, std::memory_order_relaxed);
    controlBlock_->preparationCoalescedCount.store(
        0, std::memory_order_relaxed);
    controlBlock_->producerFailure.Reset();

    ASFW_LOG(Isoch, "IT: Mapped shared memory. payloadSegments=%zu metadataRing=%p controlBlock=%p slots=%u maxBytes=%u",
             payloadDmaMap_.SegmentCount(), metadataRing_, controlBlock_, numSlots, maxPacketBytes);
    return kIOReturnSuccess;
}

kern_return_t IsochTransmitContext::PrimeForPreflight() noexcept {
    if (state_ != State::Configured && state_ != State::Stopped) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5E descriptor preflight rejected - state=%{public}s",
                 TxStateName(state_));
        return kIOReturnNotReady;
    }
    if (!hardware_ || !ring_.HasRings()) {
        ASFW_LOG(Isoch, "IT: Stage 5E descriptor preflight missing hardware/ring");
        return kIOReturnNoResources;
    }
    if (!payloadBase_ || !payloadDmaMap_.IsValid() || !metadataRing_ || !controlBlock_ ||
        controlBlock_->numSlots == 0 || controlBlock_->slotStrideBytes == 0 ||
        controlBlock_->maxPacketBytes == 0) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5E descriptor preflight shared TX contract incomplete");
        return kIOReturnNotReady;
    }

    ring_.ResetForStart();
    ring_.SeedCycleTracking(*hardware_);

    const uint64_t exposeCursor =
        controlBlock_->exposeCursor.load(std::memory_order_acquire);
    const auto primeStats = ring_.Prime(payloadDmaMap_,
                                        controlBlock_->numSlots,
                                        controlBlock_->slotStrideBytes,
                                        metadataRing_,
                                        exposeCursor);
    if (primeStats.packetsAssembled != Tx::Layout::kNumPackets) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5E descriptor preflight prime failed assembled=%llu expected=%u expose=%llu",
                 primeStats.packetsAssembled,
                 Tx::Layout::kNumPackets,
                 exposeCursor);
        return kIOReturnInternalError;
    }

    constexpr uint32_t kExpectedDataPackets = 36U;
    constexpr uint32_t kExpectedSkipPackets = 12U;
    constexpr uint32_t kExpectedDataBytes = 1536U;

    uint32_t dataPackets = 0;
    uint32_t skipPackets = 0;
    bool metadataValid = true;
    bool payloadShapeValid = true;
    for (uint32_t packet = 0; packet < Tx::Layout::kNumPackets; ++packet) {
        const uint32_t slot = packet % controlBlock_->numSlots;
        const auto& meta = metadataRing_[slot];
        const uint32_t q0 = OSSwapLittleToHostInt32(meta.immediateHeader[0]);
        const auto tag = ASFW::IsochTransport::DecodeIsochTxHeaderTag(q0);
        const bool skip = ASFW::IsochTransport::ShouldSkipIsochTxPacket(
            tag, meta.payloadLength);
        if (skip) {
            ++skipPackets;
            payloadShapeValid = payloadShapeValid && meta.payloadLength == 0U;
        } else {
            ++dataPackets;
            payloadShapeValid = payloadShapeValid &&
                                meta.payloadLength == kExpectedDataBytes;
        }
        if (tag != ASFW::IsochTransport::IsochPacketTag::kNoCipHeader ||
            ASFW::IsochTransport::DecodeIsochTxHeaderSpeed(q0) !=
                ASFW::IsochTransport::kIsochSpeedS400 ||
            ASFW::IsochTransport::DecodeIsochTxHeaderTCode(q0) !=
                ASFW::IsochTransport::kIsochDataBlockTCode ||
            ASFW::IsochTransport::DecodeIsochTxHeaderDataLength(
                OSSwapLittleToHostInt32(meta.immediateHeader[1])) !=
                meta.payloadLength) {
            metadataValid = false;
            break;
        }
    }

    const bool cadenceValid =
        dataPackets == kExpectedDataPackets &&
        skipPackets == kExpectedSkipPackets;
    const uint64_t descriptorIOVA = ring_.Slab().DescriptorRegion().deviceBase;
    if (!metadataValid || !payloadShapeValid || !cadenceValid ||
        descriptorIOVA == 0 || descriptorIOVA > 0xFFFFFFFFULL) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5E descriptor preflight validation failed metadata=%u payloadShape=%u cadence=%u data=%u/%u skip=%u/%u descriptorIOVA=0x%llx",
                 metadataValid ? 1U : 0U,
                 payloadShapeValid ? 1U : 0U,
                 cadenceValid ? 1U : 0U,
                 dataPackets,
                 kExpectedDataPackets,
                 skipPackets,
                 kExpectedSkipPackets,
                 descriptorIOVA);
        return kIOReturnInternalError;
    }

    packetsAssembled_ = primeStats.packetsAssembled;
    ASFW_LOG(Isoch,
             "IT: ✅ Stage 5E FF800 descriptor ring primed packets=%u data=%u skip=%u descriptorIOVA=0x%08x expose=%llu noCommandPtr=1 noRun=1",
             Tx::Layout::kNumPackets,
             dataPackets,
             skipPackets,
             static_cast<uint32_t>(descriptorIOVA),
             exposeCursor);
    ring_.DumpDescriptorRing(0, 8);
    return kIOReturnSuccess;
}

kern_return_t IsochTransmitContext::ProgramCommandPtrForPreflight() noexcept {
    if (state_ != State::Configured && state_ != State::Stopped) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5E CommandPtr preflight rejected - state=%{public}s",
                 TxStateName(state_));
        return kIOReturnNotReady;
    }
    if (!hardware_ || !ring_.HasRings()) {
        ASFW_LOG(Isoch, "IT: Stage 5E CommandPtr preflight missing hardware/ring");
        return kIOReturnNoResources;
    }

    const uint64_t descriptorIOVA = ring_.Slab().DescriptorRegion().deviceBase;
    if (descriptorIOVA == 0 || descriptorIOVA > 0xFFFFFFFFULL) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5E invalid descriptor IOVA 0x%llx",
                 descriptorIOVA);
        return kIOReturnInternalError;
    }

    const Register32 cmdPtrReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitCommandPtr(contextIndex_));
    const Register32 ctrlReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControl(contextIndex_));
    const Register32 ctrlClrReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControlClear(contextIndex_));

    // Make the safety invariant explicit before touching CommandPtr. No IT
    // interrupt is enabled and RUN is forced clear; a CommandPtr by itself is
    // inert and cannot launch DMA.
    hardware_->Write(ctrlClrReg, Driver::ContextControl::kRun);
    hardware_->Write(Register32::kIsoXmitIntMaskClear, (1u << contextIndex_));

    const uint32_t expectedCmd =
        static_cast<uint32_t>(descriptorIOVA) | Tx::Layout::kBlocksPerPacket;
    hardware_->Write(cmdPtrReg, expectedCmd);

    // The readbacks also flush posted MMIO writes.
    const uint32_t readCmd = hardware_->Read(cmdPtrReg);
    const uint32_t readCtl = hardware_->Read(ctrlReg);
    const bool commandMatches = readCmd == expectedCmd;
    const bool runClear = (readCtl & Driver::ContextControl::kRun) == 0;
    const bool activeClear = (readCtl & Driver::ContextControl::kActive) == 0;
    const bool deadClear = (readCtl & Driver::ContextControl::kDead) == 0;

    if (!commandMatches || !runClear || !activeClear || !deadClear) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5E CommandPtr validation failed expected=0x%08x actual=0x%08x control=0x%08x runClear=%u activeClear=%u deadClear=%u",
                 expectedCmd,
                 readCmd,
                 readCtl,
                 runClear ? 1U : 0U,
                 activeClear ? 1U : 0U,
                 deadClear ? 1U : 0U);
        return kIOReturnInternalError;
    }

    ASFW_LOG(Isoch,
             "IT: ✅ Stage 5E inert CommandPtr programmed expected=0x%08x actual=0x%08x descriptorIOVA=0x%08x Z=%u control=0x%08x noRun=1 noInterrupt=1 noPacket=1",
             expectedCmd,
             readCmd,
             static_cast<uint32_t>(descriptorIOVA),
             Tx::Layout::kBlocksPerPacket,
             readCtl);
    return kIOReturnSuccess;
}

kern_return_t IsochTransmitContext::RunAllSkipForPreflight(
    uint32_t durationMs) noexcept {
    if (state_ != State::Configured && state_ != State::Stopped) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5E finite all-skip completion rejected - state=%{public}s",
                 TxStateName(state_));
        return kIOReturnNotReady;
    }
    if (!hardware_ || !ring_.HasRings()) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5E finite all-skip completion missing hardware/ring");
        return kIOReturnNoResources;
    }
    if (durationMs == 0U || durationMs > 20U) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5E finite all-skip completion invalid timeout=%u ms",
                 durationMs);
        return kIOReturnBadArgument;
    }
    if (!ring_.ProgramAllSkipPacketsForRunPreflight()) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5E could not construct/publish finite 48-skip descriptor chain");
        return kIOReturnInternalError;
    }

    ASFW_LOG(Isoch,
             "IT: ✅ Stage 5E finite all-skip descriptor chain published descriptors=48 terminalBranch=0 bytes=%zu dmaPublish=1",
             ring_.Slab().DescriptorRegion().size);

    const uint64_t descriptorIOVA = ring_.Slab().DescriptorRegion().deviceBase;
    if (descriptorIOVA == 0U || descriptorIOVA > 0xFFFFFFFFULL) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5E invalid descriptor IOVA 0x%llx",
                 descriptorIOVA);
        return kIOReturnInternalError;
    }

    const Register32 cmdPtrReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitCommandPtr(contextIndex_));
    const Register32 ctrlReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControl(contextIndex_));
    const Register32 ctrlSetReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControlSet(contextIndex_));
    const Register32 ctrlClrReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControlClear(contextIndex_));

    hardware_->Write(ctrlClrReg, Driver::ContextControl::kWritableBits);
    hardware_->Write(Register32::kIsoXmitIntMaskClear,
                     (1U << contextIndex_));
    hardware_->Write(Register32::kIsoXmitIntEventClear,
                     (1U << contextIndex_));

    // The finite skip program begins with one descriptor, not the four
    // physical slots reserved for a normal FF800 packet program.
    const uint32_t expectedCmd =
        static_cast<uint32_t>(descriptorIOVA) | 1U;
    hardware_->Write(cmdPtrReg, expectedCmd);
    const uint32_t commandBeforeRun = hardware_->Read(cmdPtrReg);
    const uint32_t controlBeforeRun = hardware_->Read(ctrlReg);
    if (commandBeforeRun != expectedCmd ||
        (controlBeforeRun & Driver::ContextControl::kRun) != 0U ||
        (controlBeforeRun & Driver::ContextControl::kActive) != 0U ||
        (controlBeforeRun & Driver::ContextControl::kDead) != 0U) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5E finite pre-RUN validation failed expectedCmd=0x%08x actualCmd=0x%08x control=0x%08x",
                 expectedCmd,
                 commandBeforeRun,
                 controlBeforeRun);
        return kIOReturnInternalError;
    }

    hardware_->Write(ctrlSetReg, Driver::ContextControl::kRun);

    const uint32_t maxPolls = durationMs * 100U; // 10 us per poll.
    bool activeObserved = false;
    uint32_t completionPolls = 0U;
    uint32_t controlAfterRun = hardware_->Read(ctrlReg);
    auto completion = ring_.FetchAllSkipCompletionForRunPreflight();
    for (;
         completionPolls < maxPolls &&
         completion.completedDescriptors < Tx::Layout::kNumPackets &&
         (controlAfterRun & Driver::ContextControl::kDead) == 0U;
         ++completionPolls) {
        activeObserved = activeObserved ||
            ((controlAfterRun & Driver::ContextControl::kActive) != 0U);
        IODelay(10U);
        controlAfterRun = hardware_->Read(ctrlReg);
        completion = ring_.FetchAllSkipCompletionForRunPreflight();
    }
    activeObserved = activeObserved ||
        ((controlAfterRun & Driver::ContextControl::kActive) != 0U);

    const bool deadSet =
        (controlAfterRun & Driver::ContextControl::kDead) != 0U;
    const bool completionProved =
        completion.completedDescriptors == Tx::Layout::kNumPackets &&
        completion.lastTransferStatus != 0U;
    if (deadSet || !completionProved) {
        hardware_->Write(ctrlClrReg, Driver::ContextControl::kRun);
        (void)hardware_->Read(ctrlReg);
        ASFW_LOG(Isoch,
                 "IT: Stage 5E finite all-skip completion failed control=0x%08x dead=%u completed=%u/48 lastStatus=0x%04x lastTimestamp=0x%04x activeObserved=%u polls=%u",
                 controlAfterRun,
                 deadSet ? 1U : 0U,
                 completion.completedDescriptors,
                 completion.lastTransferStatus,
                 completion.lastTimestamp,
                 activeObserved ? 1U : 0U,
                 completionPolls);
        return deadSet ? kIOReturnNotPermitted : kIOReturnTimeout;
    }

    ASFW_LOG(Isoch,
             "IT: ✅ Stage 5E finite all-skip DMA completion proved cmd=0x%08x control=0x%08x completed=48/48 lastStatus=0x%04x lastTimestamp=0x%04x activeObserved=%u dormantAccepted=%u polls=%u noInterrupt=1 noPacketDescriptor=1",
             commandBeforeRun,
             controlAfterRun,
             completion.lastTransferStatus,
             completion.lastTimestamp,
             activeObserved ? 1U : 0U,
             (controlAfterRun & Driver::ContextControl::kActive) == 0U ? 1U : 0U,
             completionPolls);

    hardware_->Write(ctrlClrReg, Driver::ContextControl::kRun);
    uint32_t controlStopped = hardware_->Read(ctrlReg);
    for (uint32_t poll = 0;
         poll < 1000U &&
         (controlStopped & Driver::ContextControl::kActive) != 0U;
         ++poll) {
        IODelay(10U);
        controlStopped = hardware_->Read(ctrlReg);
    }

    hardware_->Write(Register32::kIsoXmitIntMaskClear,
                     (1U << contextIndex_));
    hardware_->Write(Register32::kIsoXmitIntEventClear,
                     (1U << contextIndex_));

    const bool runClear =
        (controlStopped & Driver::ContextControl::kRun) == 0U;
    const bool activeClear =
        (controlStopped & Driver::ContextControl::kActive) == 0U;
    const bool deadClear =
        (controlStopped & Driver::ContextControl::kDead) == 0U;
    if (!runClear || !activeClear || !deadClear) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5E finite all-skip stop validation failed control=0x%08x runClear=%u activeClear=%u deadClear=%u",
                 controlStopped,
                 runClear ? 1U : 0U,
                 activeClear ? 1U : 0U,
                 deadClear ? 1U : 0U);
        return kIOReturnTimeout;
    }

    state_ = State::Stopped;
    ASFW_LOG(Isoch,
             "IT: ✅ Stage 5E finite all-skip context stopped cleanly control=0x%08x runClear=1 activeClear=1 deadClear=1 completed=48 busPacketDescriptors=0",
             controlStopped);
    return kIOReturnSuccess;
}

kern_return_t IsochTransmitContext::RunSingleSilencePacketForPreflight(
    const uint32_t timeoutMs) noexcept {
    if (state_ != State::Configured && state_ != State::Stopped) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5F finite silent packet rejected - state=%{public}s",
                 TxStateName(state_));
        return kIOReturnNotReady;
    }
    if (!hardware_ || !ring_.HasRings() || !payloadBase_ ||
        !metadataRing_ || !controlBlock_) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5F finite silent packet missing hardware/ring/shared memory");
        return kIOReturnNoResources;
    }
    if (timeoutMs == 0U || timeoutMs > 20U) {
        return kIOReturnBadArgument;
    }

    Tx::IsochTxDmaRing::SingleSilencePacketProgram program{};
    if (!ring_.ProgramSingleSilencePacketForRunPreflight(
            payloadBase_,
            controlBlock_->numSlots,
            controlBlock_->slotStrideBytes,
            metadataRing_,
            program)) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5F could not construct verified finite silent packet program");
        return kIOReturnInternalError;
    }

    ASFW_LOG(Isoch,
             "IT: ✅ Stage 5F finite silent packet published packet=%u producerSlot=%u cmd=0x%08x channel=%u payloadBytes=%u tag=0 noCIP=1 allZero=1 terminalBranch=0 dmaPublish=1",
             program.packetSlot,
             program.producerSlot,
             program.commandPtr,
             channel_,
             program.payloadLength);

    const Register32 cmdPtrReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitCommandPtr(contextIndex_));
    const Register32 ctrlReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControl(contextIndex_));
    const Register32 ctrlSetReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControlSet(contextIndex_));
    const Register32 ctrlClrReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControlClear(contextIndex_));

    hardware_->Write(ctrlClrReg, Driver::ContextControl::kWritableBits);
    hardware_->Write(Register32::kIsoXmitIntMaskClear,
                     (1U << contextIndex_));
    hardware_->Write(Register32::kIsoXmitIntEventClear,
                     (1U << contextIndex_));
    hardware_->Write(cmdPtrReg, program.commandPtr);

    const uint32_t commandBeforeRun = hardware_->Read(cmdPtrReg);
    const uint32_t controlBeforeRun = hardware_->Read(ctrlReg);
    if (commandBeforeRun != program.commandPtr ||
        (controlBeforeRun & Driver::ContextControl::kRun) != 0U ||
        (controlBeforeRun & Driver::ContextControl::kActive) != 0U ||
        (controlBeforeRun & Driver::ContextControl::kDead) != 0U) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5F finite silent packet pre-RUN validation failed expectedCmd=0x%08x actualCmd=0x%08x control=0x%08x",
                 program.commandPtr,
                 commandBeforeRun,
                 controlBeforeRun);
        return kIOReturnInternalError;
    }

    hardware_->Write(ctrlSetReg, Driver::ContextControl::kRun);

    const uint32_t maxPolls = timeoutMs * 100U; // 10 us per poll.
    uint32_t polls = 0U;
    uint32_t controlAfterRun = hardware_->Read(ctrlReg);
    auto completion =
        ring_.FetchSingleSilencePacketCompletionForRunPreflight(
            program.packetSlot);
    while (polls < maxPolls && completion.transferStatus == 0U &&
           (controlAfterRun & Driver::ContextControl::kDead) == 0U) {
        IODelay(10U);
        ++polls;
        controlAfterRun = hardware_->Read(ctrlReg);
        completion =
            ring_.FetchSingleSilencePacketCompletionForRunPreflight(
                program.packetSlot);
    }

    const bool dead =
        (controlAfterRun & Driver::ContextControl::kDead) != 0U;
    const uint8_t eventCode =
        static_cast<uint8_t>(completion.transferStatus & 0x1FU);
    const bool ackComplete = eventCode == 0x11U;
    if (dead || completion.transferStatus == 0U || !ackComplete) {
        hardware_->Write(ctrlClrReg, Driver::ContextControl::kRun);
        (void)hardware_->Read(ctrlReg);
        ASFW_LOG(Isoch,
                 "IT: Stage 5F finite silent packet completion failed control=0x%08x dead=%u xferStatus=0x%04x event=0x%02x timestamp=0x%04x polls=%u",
                 controlAfterRun,
                 dead ? 1U : 0U,
                 completion.transferStatus,
                 eventCode,
                 completion.timestamp,
                 polls);
        return dead ? kIOReturnNotPermitted : kIOReturnTimeout;
    }

    ASFW_LOG(Isoch,
             "IT: ✅ Stage 5F finite silent packet DMA/transmit completed cmd=0x%08x control=0x%08x xferStatus=0x%04x event=ack_complete timestamp=0x%04x packet=%u channel=%u payloadBytes=%u actualBusPackets=1 payload=silence deviceEngineStopped=1 interrupts=0",
             commandBeforeRun,
             controlAfterRun,
             completion.transferStatus,
             completion.timestamp,
             program.packetSlot,
             channel_,
             program.payloadLength);

    hardware_->Write(ctrlClrReg, Driver::ContextControl::kRun);
    uint32_t controlStopped = hardware_->Read(ctrlReg);
    for (uint32_t poll = 0U;
         poll < 1000U &&
         (controlStopped & Driver::ContextControl::kActive) != 0U;
         ++poll) {
        IODelay(10U);
        controlStopped = hardware_->Read(ctrlReg);
    }
    hardware_->Write(Register32::kIsoXmitIntMaskClear,
                     (1U << contextIndex_));
    hardware_->Write(Register32::kIsoXmitIntEventClear,
                     (1U << contextIndex_));

    const bool runClear =
        (controlStopped & Driver::ContextControl::kRun) == 0U;
    const bool activeClear =
        (controlStopped & Driver::ContextControl::kActive) == 0U;
    const bool deadClear =
        (controlStopped & Driver::ContextControl::kDead) == 0U;
    if (!runClear || !activeClear || !deadClear) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5F finite silent packet stop validation failed control=0x%08x runClear=%u activeClear=%u deadClear=%u",
                 controlStopped,
                 runClear ? 1U : 0U,
                 activeClear ? 1U : 0U,
                 deadClear ? 1U : 0U);
        return kIOReturnTimeout;
    }

    state_ = State::Stopped;
    ASFW_LOG(Isoch,
             "IT: ✅ Stage 5F finite silent packet context stopped cleanly control=0x%08x runClear=1 activeClear=1 deadClear=1 actualBusPackets=1",
             controlStopped);
    return kIOReturnSuccess;
}

kern_return_t IsochTransmitContext::PrepareFiniteSilenceCadenceForPreflight()
    noexcept {
    finiteCadencePrepared_ = false;
    finiteCadenceProgram_ = {};

    if (state_ != State::Configured && state_ != State::Stopped) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5G finite cadence preparation rejected - state=%{public}s",
                 TxStateName(state_));
        return kIOReturnNotReady;
    }
    if (!hardware_ || !ring_.HasRings()) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5G finite cadence preparation missing hardware/ring");
        return kIOReturnNoResources;
    }
    if (!payloadBase_ || !metadataRing_ || !controlBlock_ ||
        controlBlock_->numSlots == 0U ||
        controlBlock_->slotStrideBytes == 0U) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5G finite cadence preparation shared TX contract incomplete");
        return kIOReturnNotReady;
    }

    Tx::IsochTxDmaRing::FiniteSilenceCadenceProgram program{};
    if (!ring_.ProgramFiniteSilenceCadenceForRunPreflight(
            payloadBase_,
            controlBlock_->numSlots,
            controlBlock_->slotStrideBytes,
            metadataRing_,
            program)) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5G could not construct verified finite D-D-D-S silence cadence");
        return kIOReturnInternalError;
    }

    ASFW_LOG(Isoch,
             "IT: ✅ Stage 5G descriptor chain published descriptors=%u dataPackets=%u skip=%u payloadBytes=%u pattern=DDD-S terminalBranch=0 dmaPublish=1",
             program.descriptorCount,
             program.dataPacketCount,
             program.skipPacketCount,
             program.payloadLength);

    const Register32 cmdPtrReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitCommandPtr(contextIndex_));
    const Register32 ctrlReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControl(contextIndex_));
    const Register32 ctrlClrReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControlClear(contextIndex_));

    hardware_->Write(ctrlClrReg, Driver::ContextControl::kWritableBits);
    hardware_->Write(Register32::kIsoXmitIntMaskClear,
                     (1U << contextIndex_));
    hardware_->Write(Register32::kIsoXmitIntEventClear,
                     (1U << contextIndex_));
    hardware_->Write(cmdPtrReg, program.commandPtr);

    const uint32_t commandReadback = hardware_->Read(cmdPtrReg);
    const uint32_t controlReadback = hardware_->Read(ctrlReg);
    const bool runClear =
        (controlReadback & Driver::ContextControl::kRun) == 0U;
    const bool activeClear =
        (controlReadback & Driver::ContextControl::kActive) == 0U;
    const bool deadClear =
        (controlReadback & Driver::ContextControl::kDead) == 0U;
    if (commandReadback != program.commandPtr || !runClear ||
        !activeClear || !deadClear) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5G finite cadence pre-RUN validation failed expectedCmd=0x%08x actualCmd=0x%08x control=0x%08x runClear=%u activeClear=%u deadClear=%u",
                 program.commandPtr,
                 commandReadback,
                 controlReadback,
                 runClear ? 1U : 0U,
                 activeClear ? 1U : 0U,
                 deadClear ? 1U : 0U);
        return kIOReturnInternalError;
    }

    finiteCadenceProgram_ = program;
    finiteCadencePrepared_ = true;
    return kIOReturnSuccess;
}

kern_return_t
IsochTransmitContext::RunPreparedFiniteSilenceCadenceForPreflight(
    const uint32_t timeoutMs) noexcept {
    if (!finiteCadencePrepared_ ||
        (state_ != State::Configured && state_ != State::Stopped)) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5G finite cadence run rejected prepared=%u state=%{public}s",
                 finiteCadencePrepared_ ? 1U : 0U,
                 TxStateName(state_));
        return kIOReturnNotReady;
    }
    if (!hardware_ || timeoutMs == 0U || timeoutMs > 50U) {
        finiteCadencePrepared_ = false;
        finiteCadenceProgram_ = {};
        return hardware_ ? kIOReturnBadArgument : kIOReturnNoResources;
    }

    const auto program = finiteCadenceProgram_;
    const Register32 cmdPtrReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitCommandPtr(contextIndex_));
    const Register32 ctrlReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControl(contextIndex_));
    const Register32 ctrlSetReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControlSet(contextIndex_));

    const uint32_t commandBeforeRun = hardware_->Read(cmdPtrReg);
    const uint32_t controlBeforeRun = hardware_->Read(ctrlReg);
    if (commandBeforeRun != program.commandPtr ||
        (controlBeforeRun & Driver::ContextControl::kRun) != 0U ||
        (controlBeforeRun & Driver::ContextControl::kActive) != 0U ||
        (controlBeforeRun & Driver::ContextControl::kDead) != 0U) {
        finiteCadencePrepared_ = false;
        finiteCadenceProgram_ = {};
        ASFW_LOG(Isoch,
                 "IT: Stage 5G finite cadence RUN gate failed expectedCmd=0x%08x actualCmd=0x%08x control=0x%08x",
                 program.commandPtr,
                 commandBeforeRun,
                 controlBeforeRun);
        return kIOReturnInternalError;
    }

    hardware_->Write(ctrlSetReg, Driver::ContextControl::kRun);

    const uint32_t maxPolls = timeoutMs * 100U; // 10 us per poll.
    uint32_t polls = 0U;
    bool deadObserved = false;
    bool activeObserved = false;
    uint32_t controlAfterRun = hardware_->Read(ctrlReg);
    auto completion =
        ring_.FetchFiniteSilenceCadenceCompletionForRunPreflight(
            program.startPacketSlot);
    while (polls < maxPolls &&
           completion.completedDescriptors < program.descriptorCount) {
        deadObserved = deadObserved ||
            (controlAfterRun & Driver::ContextControl::kDead) != 0U;
        activeObserved = activeObserved ||
            (controlAfterRun & Driver::ContextControl::kActive) != 0U;
        if (deadObserved) {
            break;
        }
        IODelay(10U);
        ++polls;
        controlAfterRun = hardware_->Read(ctrlReg);
        completion =
            ring_.FetchFiniteSilenceCadenceCompletionForRunPreflight(
                program.startPacketSlot);
    }
    deadObserved = deadObserved ||
        (controlAfterRun & Driver::ContextControl::kDead) != 0U;
    activeObserved = activeObserved ||
        (controlAfterRun & Driver::ContextControl::kActive) != 0U;

    const bool completionValid =
        !deadObserved &&
        completion.completedDescriptors == program.descriptorCount &&
        completion.completedDataDescriptors == program.dataPacketCount &&
        completion.actualBusPackets == program.dataPacketCount &&
        completion.eventErrors == 0U &&
        completion.cadenceValid;

    if (completionValid) {
        ASFW_LOG(Isoch,
                 "IT: ✅ Stage 5G finite cadence burst completed descriptors=%u/%u dataCompleted=%u/%u actualBusPackets=%u eventErrors=0 deadObserved=0 activeObserved=%u polls=%u",
                 completion.completedDescriptors,
                 program.descriptorCount,
                 completion.completedDataDescriptors,
                 program.dataPacketCount,
                 completion.actualBusPackets,
                 activeObserved ? 1U : 0U,
                 polls);
        ASFW_LOG(Isoch,
                 "IT: ✅ Stage 5G cadence verification passed timestampPattern=1,1,2");
    } else {
        ASFW_LOG(Isoch,
                 "IT: Stage 5G finite cadence burst failed control=0x%08x descriptors=%u/%u dataCompleted=%u/%u actualBusPackets=%u eventErrors=%u deadObserved=%u cadenceValid=%u lastStatus=0x%04x lastTimestamp=0x%04x polls=%u",
                 controlAfterRun,
                 completion.completedDescriptors,
                 program.descriptorCount,
                 completion.completedDataDescriptors,
                 program.dataPacketCount,
                 completion.actualBusPackets,
                 completion.eventErrors,
                 deadObserved ? 1U : 0U,
                 completion.cadenceValid ? 1U : 0U,
                 completion.lastTransferStatus,
                 completion.lastTimestamp,
                 polls);
    }

    const kern_return_t cleanupKr =
        CleanupFiniteSilenceCadenceForPreflight();
    if (cleanupKr != kIOReturnSuccess) {
        return cleanupKr;
    }
    if (!completionValid) {
        return deadObserved ? kIOReturnNotPermitted : kIOReturnTimeout;
    }
    return kIOReturnSuccess;
}

kern_return_t
IsochTransmitContext::CleanupFiniteSilenceCadenceForPreflight() noexcept {
    finiteCadencePrepared_ = false;
    finiteCadenceProgram_ = {};

    if (!hardware_) {
        if (state_ != State::Unconfigured) {
            state_ = State::Stopped;
        }
        return kIOReturnNoResources;
    }

    const Register32 ctrlReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControl(contextIndex_));
    const Register32 ctrlClrReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControlClear(contextIndex_));

    hardware_->Write(ctrlClrReg, Driver::ContextControl::kRun);
    uint32_t controlStopped = hardware_->Read(ctrlReg);
    for (uint32_t poll = 0U;
         poll < 1000U &&
         (controlStopped & Driver::ContextControl::kActive) != 0U;
         ++poll) {
        IODelay(10U);
        controlStopped = hardware_->Read(ctrlReg);
    }
    hardware_->Write(Register32::kIsoXmitIntMaskClear,
                     (1U << contextIndex_));
    hardware_->Write(Register32::kIsoXmitIntEventClear,
                     (1U << contextIndex_));

    const bool runClear =
        (controlStopped & Driver::ContextControl::kRun) == 0U;
    const bool activeClear =
        (controlStopped & Driver::ContextControl::kActive) == 0U;
    const bool deadClear =
        (controlStopped & Driver::ContextControl::kDead) == 0U;
    if (state_ != State::Unconfigured) {
        state_ = State::Stopped;
    }
    refillInProgress_.clear(std::memory_order_release);

    if (!runClear || !activeClear || !deadClear) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5G finite cadence stop validation failed control=0x%08x runClear=%u activeClear=%u deadClear=%u",
                 controlStopped,
                 runClear ? 1U : 0U,
                 activeClear ? 1U : 0U,
                 deadClear ? 1U : 0U);
        return deadClear ? kIOReturnTimeout : kIOReturnNotPermitted;
    }

    ASFW_LOG(Isoch,
             "IT: ✅ Stage 5G transmit context stopped cleanly control=0x%08x runClear=1 activeClear=1 deadClear=1",
             controlStopped);
    return kIOReturnSuccess;
}

kern_return_t
IsochTransmitContext::PrepareBoundedCircularSilenceCadenceForPreflight()
    noexcept {
    boundedCircularCadencePrepared_ = false;
    boundedCircularCadenceProgram_ = {};
    finiteCadencePrepared_ = false;
    finiteCadenceProgram_ = {};

    if (state_ != State::Configured && state_ != State::Stopped) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5H circular cadence preparation rejected state=%{public}s",
                 TxStateName(state_));
        return kIOReturnNotReady;
    }
    if (!hardware_ || !ring_.HasRings()) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5H circular cadence preparation missing hardware/ring");
        return kIOReturnNoResources;
    }
    if (!payloadBase_ || !metadataRing_ || !controlBlock_ ||
        controlBlock_->numSlots == 0U ||
        controlBlock_->slotStrideBytes == 0U) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5H circular cadence shared TX contract incomplete");
        return kIOReturnNotReady;
    }

    Tx::IsochTxDmaRing::BoundedCircularSilenceCadenceProgram program{};
    if (!ring_.ProgramBoundedCircularSilenceCadenceForPreflight(
            payloadBase_,
            controlBlock_->numSlots,
            controlBlock_->slotStrideBytes,
            metadataRing_,
            program)) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5H could not construct verified circular D-D-D-S silence cadence");
        return kIOReturnInternalError;
    }

    const Register32 cmdPtrReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitCommandPtr(contextIndex_));
    const Register32 ctrlReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControl(contextIndex_));
    const Register32 ctrlClrReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControlClear(contextIndex_));

    hardware_->Write(ctrlClrReg, Driver::ContextControl::kWritableBits);
    hardware_->Write(Register32::kIsoXmitIntMaskClear,
                     (1U << contextIndex_));
    hardware_->Write(Register32::kIsoXmitIntEventClear,
                     (1U << contextIndex_));
    hardware_->Write(cmdPtrReg, program.commandPtr);

    const uint32_t commandReadback = hardware_->Read(cmdPtrReg);
    const uint32_t controlReadback = hardware_->Read(ctrlReg);
    const bool runClear =
        (controlReadback & Driver::ContextControl::kRun) == 0U;
    const bool activeClear =
        (controlReadback & Driver::ContextControl::kActive) == 0U;
    const bool deadClear =
        (controlReadback & Driver::ContextControl::kDead) == 0U;
    if (commandReadback != program.commandPtr || !runClear ||
        !activeClear || !deadClear) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5H circular cadence pre-RUN validation failed expectedCmd=0x%08x actualCmd=0x%08x control=0x%08x runClear=%u activeClear=%u deadClear=%u",
                 program.commandPtr,
                 commandReadback,
                 controlReadback,
                 runClear ? 1U : 0U,
                 activeClear ? 1U : 0U,
                 deadClear ? 1U : 0U);
        return kIOReturnInternalError;
    }

    boundedCircularCadenceProgram_ = program;
    boundedCircularCadencePrepared_ = true;
    ASFW_LOG(Isoch,
             "IT: ✅ Stage 5H circular cadence armed with RUN clear cmd=0x%08x startSlot=%u durationPending=1",
             program.commandPtr,
             program.startPacketSlot);
    return kIOReturnSuccess;
}

kern_return_t
IsochTransmitContext::RunPreparedBoundedCircularSilenceCadenceForPreflight(
    const uint32_t durationMs) noexcept {
    if (!boundedCircularCadencePrepared_ ||
        (state_ != State::Configured && state_ != State::Stopped)) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5H circular cadence run rejected prepared=%u state=%{public}s",
                 boundedCircularCadencePrepared_ ? 1U : 0U,
                 TxStateName(state_));
        return kIOReturnNotReady;
    }
    if (!hardware_ || durationMs < 50U || durationMs > 250U) {
        return hardware_ ? kIOReturnBadArgument : kIOReturnNoResources;
    }

    const auto program = boundedCircularCadenceProgram_;
    const Register32 cmdPtrReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitCommandPtr(contextIndex_));
    const Register32 ctrlReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControl(contextIndex_));
    const Register32 ctrlSetReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControlSet(contextIndex_));

    const uint32_t commandBeforeRun = hardware_->Read(cmdPtrReg);
    const uint32_t controlBeforeRun = hardware_->Read(ctrlReg);
    if (commandBeforeRun != program.commandPtr ||
        (controlBeforeRun & Driver::ContextControl::kRun) != 0U ||
        (controlBeforeRun & Driver::ContextControl::kActive) != 0U ||
        (controlBeforeRun & Driver::ContextControl::kDead) != 0U) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5H circular cadence RUN gate failed expectedCmd=0x%08x actualCmd=0x%08x control=0x%08x",
                 program.commandPtr,
                 commandBeforeRun,
                 controlBeforeRun);
        return kIOReturnInternalError;
    }

    hardware_->Write(ctrlSetReg, Driver::ContextControl::kRun);

    constexpr uint32_t kPollDelayUs = 250U;
    const uint32_t maxPolls = (durationMs * 1000U) / kPollDelayUs;
    uint32_t polls = 0U;
    bool deadObserved = false;
    bool activeObserved = false;
    uint32_t consecutiveInactiveAfterActive = 0U;
    uint32_t maxInactiveAfterActive = 0U;
    uint32_t anchorExecutions = 0U;
    uint32_t anchorEventErrors = 0U;
    uint16_t previousAnchorTimestamp = 0U;
    bool anchorSeen = false;

    for (; polls < maxPolls; ++polls) {
        const uint32_t control = hardware_->Read(ctrlReg);
        const bool active =
            (control & Driver::ContextControl::kActive) != 0U;
        const bool dead =
            (control & Driver::ContextControl::kDead) != 0U;
        deadObserved = deadObserved || dead;
        if (active) {
            activeObserved = true;
            consecutiveInactiveAfterActive = 0U;
        } else if (activeObserved) {
            ++consecutiveInactiveAfterActive;
            maxInactiveAfterActive = std::max(
                maxInactiveAfterActive, consecutiveInactiveAfterActive);
        }

        const auto anchor =
            ring_.FetchCadenceAnchorCompletionForPreflight(
                program.startPacketSlot);
        if (anchor.transferStatus != 0U) {
            const uint8_t eventCode = static_cast<uint8_t>(
                anchor.transferStatus & 0x1FU);
            if (eventCode != 0x11U) {
                ++anchorEventErrors;
            } else if (!anchorSeen) {
                anchorSeen = true;
                previousAnchorTimestamp = anchor.timestamp;
                anchorExecutions = 1U;
            } else if (anchor.timestamp != previousAnchorTimestamp) {
                previousAnchorTimestamp = anchor.timestamp;
                ++anchorExecutions;
            }
        }

        if (dead) {
            break;
        }
        IODelay(kPollDelayUs);
    }

    // Stop the immutable ring synchronously before the protocol sends the
    // FF800 stop command. The cleanup callback remains idempotent and will
    // repeat this gate on every partial-failure path.
    const kern_return_t stopStatus =
        CleanupBoundedCircularSilenceCadenceForPreflight();
    const auto completion =
        ring_.FetchFiniteSilenceCadenceCompletionForRunPreflight(
            program.startPacketSlot);

    const uint32_t completedSweeps =
        anchorExecutions > 0U ? anchorExecutions - 1U : 0U;
    const uint32_t minimumBusPackets =
        completedSweeps * program.dataPacketCount;
    const uint32_t nominalSweeps = durationMs / 6U;
    const uint32_t minimumRequiredSweeps =
        std::max(4U, nominalSweeps / 2U);

    // Tolerate at most three consecutive inactive samples (750 us) to avoid
    // treating a single asynchronous register read as a lost circular run.
    const bool activeContinuous = maxInactiveAfterActive < 4U;
    const bool repeatedRunValid =
        stopStatus == kIOReturnSuccess &&
        !deadObserved &&
        activeObserved &&
        activeContinuous &&
        anchorEventErrors == 0U &&
        completedSweeps >= minimumRequiredSweeps &&
        completion.completedDescriptors == program.descriptorCount &&
        completion.completedDataDescriptors == program.dataPacketCount &&
        completion.actualBusPackets == program.dataPacketCount &&
        completion.eventErrors == 0U;

    if (!repeatedRunValid) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5H bounded circular run failed durationMs=%u controlPolls=%u activeObserved=%u maxInactivePolls=%u deadObserved=%u anchorExecutions=%u completedSweeps=%u/%u minimumBusPackets=%u anchorEventErrors=%u descriptors=%u/%u dataCompleted=%u/%u eventErrors=%u stopKr=0x%x",
                 durationMs,
                 polls,
                 activeObserved ? 1U : 0U,
                 maxInactiveAfterActive,
                 deadObserved ? 1U : 0U,
                 anchorExecutions,
                 completedSweeps,
                 minimumRequiredSweeps,
                 minimumBusPackets,
                 anchorEventErrors,
                 completion.completedDescriptors,
                 program.descriptorCount,
                 completion.completedDataDescriptors,
                 program.dataPacketCount,
                 completion.eventErrors,
                 stopStatus);
        if (stopStatus != kIOReturnSuccess) {
            return stopStatus;
        }
        return deadObserved ? kIOReturnNotPermitted : kIOReturnTimeout;
    }

    ASFW_LOG(Isoch,
             "IT: ✅ Stage 5H bounded circular silence sustained durationMs=%u anchorExecutions=%u completedSweeps=%u minimumBusPackets=%u activeContinuous=1 eventErrors=0 deadObserved=0 interrupts=0 refill=0",
             durationMs,
             anchorExecutions,
             completedSweeps,
             minimumBusPackets);
    return kIOReturnSuccess;
}

kern_return_t
IsochTransmitContext::CleanupBoundedCircularSilenceCadenceForPreflight()
    noexcept {
    boundedCircularCadencePrepared_ = false;
    boundedCircularCadenceProgram_ = {};

    if (!hardware_) {
        if (state_ != State::Unconfigured) {
            state_ = State::Stopped;
        }
        return kIOReturnNoResources;
    }

    const Register32 ctrlReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControl(contextIndex_));
    const Register32 ctrlClrReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControlClear(contextIndex_));

    hardware_->Write(ctrlClrReg, Driver::ContextControl::kRun);
    uint32_t controlStopped = hardware_->Read(ctrlReg);
    for (uint32_t poll = 0U;
         poll < 2000U &&
         (controlStopped & Driver::ContextControl::kActive) != 0U;
         ++poll) {
        IODelay(10U);
        controlStopped = hardware_->Read(ctrlReg);
    }
    hardware_->Write(Register32::kIsoXmitIntMaskClear,
                     (1U << contextIndex_));
    hardware_->Write(Register32::kIsoXmitIntEventClear,
                     (1U << contextIndex_));

    const bool runClear =
        (controlStopped & Driver::ContextControl::kRun) == 0U;
    const bool activeClear =
        (controlStopped & Driver::ContextControl::kActive) == 0U;
    const bool deadClear =
        (controlStopped & Driver::ContextControl::kDead) == 0U;
    if (state_ != State::Unconfigured) {
        state_ = State::Stopped;
    }
    refillInProgress_.clear(std::memory_order_release);

    if (!runClear || !activeClear || !deadClear) {
        ASFW_LOG(Isoch,
                 "IT: Stage 5H circular context stop validation failed control=0x%08x runClear=%u activeClear=%u deadClear=%u",
                 controlStopped,
                 runClear ? 1U : 0U,
                 activeClear ? 1U : 0U,
                 deadClear ? 1U : 0U);
        return deadClear ? kIOReturnTimeout : kIOReturnNotPermitted;
    }

    ASFW_LOG(Isoch,
             "IT: ✅ Stage 5H circular transmit context stopped cleanly control=0x%08x runClear=1 activeClear=1 deadClear=1",
             controlStopped);
    return kIOReturnSuccess;
}

kern_return_t
IsochTransmitContext::StartContinuousCircularSilenceCadence() noexcept {
    if (!boundedCircularCadencePrepared_ ||
        (state_ != State::Configured && state_ != State::Stopped)) {
        ASFW_LOG(Isoch,
                 "IT: Continuous cadence start rejected prepared=%u state=%{public}s",
                 boundedCircularCadencePrepared_ ? 1U : 0U,
                 TxStateName(state_));
        return kIOReturnNotReady;
    }
    if (!hardware_) {
        return kIOReturnNoResources;
    }

    const auto program = boundedCircularCadenceProgram_;
    const Register32 cmdPtrReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitCommandPtr(contextIndex_));
    const Register32 ctrlReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControl(contextIndex_));
    const Register32 ctrlSetReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControlSet(contextIndex_));

    const uint32_t commandBeforeRun = hardware_->Read(cmdPtrReg);
    const uint32_t controlBeforeRun = hardware_->Read(ctrlReg);
    if (commandBeforeRun != program.commandPtr ||
        (controlBeforeRun & Driver::ContextControl::kRun) != 0U ||
        (controlBeforeRun & Driver::ContextControl::kActive) != 0U ||
        (controlBeforeRun & Driver::ContextControl::kDead) != 0U) {
        ASFW_LOG(Isoch,
                 "IT: Continuous cadence RUN gate failed expectedCmd=0x%08x actualCmd=0x%08x control=0x%08x",
                 program.commandPtr,
                 commandBeforeRun,
                 controlBeforeRun);
        return kIOReturnInternalError;
    }

    continuousAnchorSeen_ = false;
    continuousAnchorExecutions_ = 0U;
    continuousPreviousAnchorTimestamp_ = 0U;

    hardware_->Write(ctrlSetReg, Driver::ContextControl::kRun);

    // One settle delay + single readback/anchor-fetch to confirm the ring
    // actually started -- not a sustain-and-verify loop like Stage 5H's run
    // method. The hardware keeps looping on its own from here.
    IODelay(250U);
    const uint32_t controlAfterRun = hardware_->Read(ctrlReg);
    const bool active =
        (controlAfterRun & Driver::ContextControl::kActive) != 0U;
    const bool dead =
        (controlAfterRun & Driver::ContextControl::kDead) != 0U;
    const auto anchor = ring_.FetchCadenceAnchorCompletionForPreflight(
        program.startPacketSlot);
    if (anchor.transferStatus != 0U) {
        const uint8_t eventCode =
            static_cast<uint8_t>(anchor.transferStatus & 0x1FU);
        if (eventCode == 0x11U) {
            continuousAnchorSeen_ = true;
            continuousAnchorExecutions_ = 1U;
            continuousPreviousAnchorTimestamp_ = anchor.timestamp;
        }
    }

    if (dead || !active) {
        ASFW_LOG(Isoch,
                 "IT: Continuous cadence start failed to sustain control=0x%08x active=%u dead=%u",
                 controlAfterRun,
                 active ? 1U : 0U,
                 dead ? 1U : 0U);
        (void)CleanupBoundedCircularSilenceCadenceForPreflight();
        return dead ? kIOReturnNotPermitted : kIOReturnTimeout;
    }

    continuousCadenceRunning_ = true;
    ASFW_LOG(Isoch,
             "IT: ✅ Continuous circular silence cadence started channel/descriptors=%u dataPackets=%u skip=%u anchorConfirmed=%u",
             program.descriptorCount,
             program.dataPacketCount,
             program.skipPacketCount,
             continuousAnchorSeen_ ? 1U : 0U);
    return kIOReturnSuccess;
}

IsochTransmitContext::ContinuousCadenceHealth
IsochTransmitContext::PollContinuousCircularSilenceCadenceHealth() noexcept {
    ContinuousCadenceHealth health{};
    if (!continuousCadenceRunning_ || !hardware_) {
        return health;
    }
    health.running = true;

    const auto program = boundedCircularCadenceProgram_;
    const Register32 ctrlReg = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControl(contextIndex_));
    const uint32_t control = hardware_->Read(ctrlReg);
    health.dead = (control & Driver::ContextControl::kDead) != 0U;

    const auto anchor = ring_.FetchCadenceAnchorCompletionForPreflight(
        program.startPacketSlot);
    if (anchor.transferStatus != 0U) {
        const uint8_t eventCode =
            static_cast<uint8_t>(anchor.transferStatus & 0x1FU);
        if (eventCode != 0x11U) {
            health.eventError = true;
        } else if (!continuousAnchorSeen_) {
            continuousAnchorSeen_ = true;
            continuousAnchorExecutions_ = 1U;
            continuousPreviousAnchorTimestamp_ = anchor.timestamp;
        } else if (anchor.timestamp != continuousPreviousAnchorTimestamp_) {
            continuousPreviousAnchorTimestamp_ = anchor.timestamp;
            ++continuousAnchorExecutions_;
        }
    }

    health.anchorExecutions = continuousAnchorExecutions_;
    health.lastAnchorTimestamp = continuousPreviousAnchorTimestamp_;
    return health;
}

kern_return_t
IsochTransmitContext::StopContinuousCircularSilenceCadence() noexcept {
    const bool wasRunning = continuousCadenceRunning_;
    continuousCadenceRunning_ = false;
    const kern_return_t status =
        CleanupBoundedCircularSilenceCadenceForPreflight();
    if (wasRunning) {
        ASFW_LOG(Isoch,
                 "IT: Continuous circular silence cadence stopped anchorExecutions=%u stopKr=0x%x",
                 continuousAnchorExecutions_,
                 status);
    }
    return status;
}

kern_return_t IsochTransmitContext::Start() noexcept {
    if (state_ != State::Configured && state_ != State::Stopped) {
        ASFW_LOG(Isoch, "IT: Start rejected - state=%{public}s", TxStateName(state_));
        return kIOReturnNotReady;
    }

    if (!hardware_) {
        ASFW_LOG(Isoch, "IT: Cannot start - no hardware");
        return kIOReturnNotReady;
    }

    if (!ring_.HasRings()) {
        ASFW_LOG(Isoch, "IT: Cannot start - no DMA ring");
        return kIOReturnNoResources;
    }
    if (!payloadBase_ || !payloadDmaMap_.IsValid() || !metadataRing_ || !controlBlock_ ||
        controlBlock_->numSlots == 0 || controlBlock_->slotStrideBytes == 0 ||
        controlBlock_->maxPacketBytes == 0) {
        ASFW_LOG(Isoch, "IT: Cannot start - shared TX payload contract is incomplete");
        return kIOReturnNotReady;
    }

    packetsAssembled_ = 0;
    tickCount_ = 0;
    interruptCount_.store(0, std::memory_order_relaxed);
    lastInterruptCountSeen_ = 0;
    irqStallTicks_ = 0;
    refillInProgress_.clear(std::memory_order_release);

    latencyBucket0_.store(0, std::memory_order_relaxed);
    latencyBucket1_.store(0, std::memory_order_relaxed);
    latencyBucket2_.store(0, std::memory_order_relaxed);
    latencyBucket3_.store(0, std::memory_order_relaxed);
    maxRefillLatencyUs_.store(0, std::memory_order_relaxed);
    irqWatchdogKicks_.store(0, std::memory_order_relaxed);

    ring_.ResetForStart();
    ring_.SeedCycleTracking(*hardware_);

    ASFW_LOG(Isoch, "IT: Starting transmit context (Stage 3 - ADK Phase 2)");

    const uint64_t preFillCount = controlBlock_->exposeCursor.load(std::memory_order_relaxed);
    const auto primeStats =
        ring_.Prime(payloadDmaMap_, controlBlock_->numSlots, controlBlock_->slotStrideBytes, metadataRing_, preFillCount);
    if (primeStats.packetsAssembled != Tx::Layout::kNumPackets) {
        ASFW_LOG(Isoch, "IT: Failed to prime descriptor ring against shared payload slab");
        return kIOReturnInternalError;
    }
    packetsAssembled_ = primeStats.packetsAssembled;
    controlBlock_->statusWord.store(
        ASFW::IsochTransport::TxStreamStatus::kRunning,
        std::memory_order_release);
    
    Register32 cmdPtrReg = static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(contextIndex_));
    Register32 ctrlReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(contextIndex_));
    Register32 ctrlSetReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControlSet(contextIndex_));
    Register32 ctrlClrReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControlClear(contextIndex_));

    const uint64_t descIOVA = ring_.Slab().DescriptorRegion().deviceBase;
    if (descIOVA == 0 || descIOVA > 0xFFFFFFFFULL) {
        ASFW_LOG(Isoch, "IT: Invalid descriptor IOVA 0x%llx", descIOVA);
        return kIOReturnInternalError;
    }
    const uint32_t cmdPtr = static_cast<uint32_t>(descIOVA) | Tx::Layout::kBlocksPerPacket;

    ASFW_LOG(Isoch, "IT: Writing CommandPtr=0x%08x (Z=%u)", cmdPtr, Tx::Layout::kBlocksPerPacket);
    hardware_->Write(cmdPtrReg, cmdPtr);

    hardware_->Write(ctrlClrReg, Driver::ContextControl::kWritableBits);

    hardware_->Write(Register32::kIsoXmitIntEventClear, 0xFFFFFFFF);
    hardware_->Write(Register32::kIsoXmitIntMaskSet, (1u << contextIndex_));
    hardware_->Write(Register32::kIntMaskSet, IntEventBits::kIsochTx);
    ASFW_LOG(Isoch, "IT: Enabled IT interrupt for context %u", contextIndex_);

    hardware_->Write(ctrlSetReg, Driver::ContextControl::kRun);

    const uint32_t readCmd = hardware_->Read(cmdPtrReg);
    const uint32_t readCtl = hardware_->Read(ctrlReg);

    const bool runSet = (readCtl & Driver::ContextControl::kRun) != 0;
    const bool activeSet = (readCtl & Driver::ContextControl::kActive) != 0;
    const bool deadSet = (readCtl & Driver::ContextControl::kDead) != 0;
    const uint32_t eventCode = (readCtl & Driver::ContextControl::kEventCodeMask) >> Driver::ContextControl::kEventCodeShift;

    ASFW_LOG(Isoch, "IT: Readback Cmd=0x%08x Ctl=0x%08x (run=%d active=%d dead=%d evt=0x%02x)",
             readCmd, readCtl, runSet, activeSet, deadSet, eventCode);

    if (deadSet) {
        ASFW_LOG(Isoch, "❌ IT: Context is DEAD immediately! Check descriptor program.");
        return kIOReturnNotPermitted;
    }

    state_ = State::Running;
    ASFW_LOG(Isoch, "IT: Started successfully");
    return kIOReturnSuccess;
}

void IsochTransmitContext::Stop() noexcept {
    // Safety net: a Stage 6 continuous cadence deliberately never enters
    // State::Running (see StartContinuousCircularSilenceCadence), so the
    // RUN-clear block below would otherwise never fire for it. Without this,
    // a driver teardown/unload while a continuous stream is active would
    // leave the OHCI IT context spinning in RUN state.
    if (continuousCadenceRunning_) {
        (void)StopContinuousCircularSilenceCadence();
    }

    finiteCadencePrepared_ = false;
    finiteCadenceProgram_ = {};
    boundedCircularCadencePrepared_ = false;
    boundedCircularCadenceProgram_ = {};
    if (state_ == State::Running && hardware_) {
        Register32 ctrlClrReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControlClear(contextIndex_));
        hardware_->Write(ctrlClrReg, Driver::ContextControl::kRun);

        hardware_->Write(Register32::kIsoXmitIntMaskClear, (1u << contextIndex_));

        if (controlBlock_) {
            controlBlock_->statusWord.store(ASFW::IsochTransport::TxStreamStatus::kStopped, std::memory_order_release);
        }

        state_ = State::Stopped;
        refillInProgress_.clear(std::memory_order_release);
        ASFW_LOG(Isoch, "IT: Stopped. Stats: %llu pkts IRQs=%llu",
                 packetsAssembled_, interruptCount_.load(std::memory_order_relaxed));
        const auto& rc = ring_.RTCounters();
        ASFW_LOG(Isoch,
                 "IT WIRE final data=%llu zeroPcm=%llu infoQuads=%llu dropouts=%llu maxAbs24=%u lastQuad=0x%08x firstInfoAbsIdx=%llu",
                 rc.wireDataPackets.load(std::memory_order_relaxed),
                 rc.wireZeroPcmPackets.load(std::memory_order_relaxed),
                 rc.wireInfoQuads.load(std::memory_order_relaxed),
                 rc.wirePcmDropouts.load(std::memory_order_relaxed),
                 rc.wireMaxAbs24.load(std::memory_order_relaxed),
                 rc.wireLastInfoQuad.load(std::memory_order_relaxed),
                 rc.wireFirstInfoAbsIdx.load(std::memory_order_relaxed));
    }

    if (state_ == State::Configured) {
        state_ = State::Stopped;
        refillInProgress_.clear(std::memory_order_release);
        ASFW_LOG(Isoch, "IT: Stopped from configured state before hardware run");
    }
}

void IsochTransmitContext::DoRefillOnce(uint64_t eventHostTicks,
                                        bool publishTimingEvent) noexcept {
    if (!hardware_ || state_ != State::Running) {
        return;
    }

    if (!metadataRing_ || !controlBlock_) {
        return;
    }

    const uint32_t numSlots = controlBlock_->numSlots;

    auto outcome = ring_.Refill(
        *hardware_,
        contextIndex_,
        metadataRing_,
        controlBlock_,
        numSlots,
        payloadBase_,
        payloadDmaMap_);
    if (!outcome.ok) {
        const auto& counters = ring_.RTCounters();
        if (outcome.failureReason ==
                Tx::IsochTxDmaRing::RefillFailureReason::
                    ProducerFatalStatus &&
            outcome.producerFailureAvailable) {
            const auto& failure = outcome.producerFailure;
            ASFW_LOG(
                Isoch,
                "IT: Producer fatal stage=%{public}s reason=%{public}s "
                "generation=%llu packet=%llu range=[%llu,%llu) "
                "prepared=%u completion=%llu expose=%llu "
                "replayProducer=%llu replayEpoch=%u",
                ASFW::IsochTransport::TxProducerStageName(
                    failure.stage),
                ASFW::IsochTransport::TxProducerFailureReasonName(
                    failure.reason),
                failure.generation,
                failure.packetIndex,
                failure.rangeStart,
                failure.rangeTarget,
                failure.preparedCount,
                failure.completionCursor,
                failure.exposeCursor,
                failure.replayProducerCursor,
                failure.replayEpoch);
        }
        ASFW_LOG(
            Isoch,
            "IT: Refill failed reason=%{public}s ctrl=0x%08x streamStatus=%u "
            "cmdPtr=0x%08x cmdAddr=0x%08x hwPacket=%u "
            "fatalAbs=%llu fatalSlot=%u payloadLen=%u "
            "exitDead=%llu exitDecode=%llu fatalSize=%llu fatalMap=%llu "
            "underruns=%llu - stopping immediately",
            Tx::IsochTxDmaRing::RefillFailureReasonName(
                outcome.failureReason),
            outcome.contextControl,
            outcome.streamStatus,
            outcome.cmdPtr,
            outcome.cmdAddr,
            outcome.hwPacketIndex,
            outcome.failurePacketAbs,
            outcome.failureSlot,
            outcome.failurePayloadLength,
            counters.exitDead.load(std::memory_order_relaxed),
            counters.exitDecodeFail.load(std::memory_order_relaxed),
            counters.fatalPacketSize.load(std::memory_order_relaxed),
            counters.fatalPayloadMapping.load(std::memory_order_relaxed),
            counters.txUnderruns.load(std::memory_order_relaxed));
        StopImmediatelyForTxFault();
    } else {
        packetsAssembled_ += outcome.packetsFilled;
        if (outcome.packetsFilled > 0) {
            ring_.WakeHardwareIfIdle(*hardware_, contextIndex_);
        }
        if (outcome.preparationRequestGeneration != 0 &&
            txPreparationCallback_) {
            txPreparationCallback_(outcome.preparationRequestGeneration);
        }
    }
}

void IsochTransmitContext::SetTxPreparationCallback(
    TxPreparationCallback callback) noexcept {
    txPreparationCallback_ = std::move(callback);
}

void IsochTransmitContext::StopImmediatelyForTxFault() noexcept {
    if (state_ == State::Stopped) {
        return;
    }
    if (hardware_) {
        const Register32 ctrlClrReg =
            static_cast<Register32>(
                DMAContextHelpers::IsoXmitContextControlClear(contextIndex_));
        hardware_->Write(ctrlClrReg, Driver::ContextControl::kRun);
        hardware_->Write(Register32::kIsoXmitIntMaskClear, (1u << contextIndex_));
    }
    if (controlBlock_) {
        const auto currentStatus = controlBlock_->statusWord.load(std::memory_order_acquire);
        if (currentStatus != ASFW::IsochTransport::TxStreamStatus::kUnderrunFatal) {
            controlBlock_->statusWord.store(ASFW::IsochTransport::TxStreamStatus::kDeadContext, std::memory_order_release);
        }
    }
    state_ = State::Stopped;
    ASFW_LOG(Isoch, "IT FATAL STOP: RUN cleared and interrupt masked");
}

void IsochTransmitContext::Poll() noexcept {
    if (state_ != State::Running) return;
    ++tickCount_;

    const uint64_t currentInterrupts = interruptCount_.load(std::memory_order_relaxed);
    if (currentInterrupts == lastInterruptCountSeen_) {
        irqStallTicks_++;
        if (irqStallTicks_ >= 5) {
            irqStallTicks_ = 0;
            irqWatchdogKicks_.fetch_add(1, std::memory_order_relaxed);
            DoRefillOnce(mach_absolute_time(), /*publishTimingEvent=*/false);
        }
    } else {
        lastInterruptCountSeen_ = currentInterrupts;
        irqStallTicks_ = 0;
    }
}

void IsochTransmitContext::HandleInterrupt() noexcept {
    if (state_ != State::Running) return;
    interruptCount_.fetch_add(1, std::memory_order_relaxed);

    if (refillInProgress_.test_and_set(std::memory_order_acq_rel)) {
        return;
    }

    const uint64_t refillStart = mach_absolute_time();
    DoRefillOnce(refillStart, /*publishTimingEvent=*/true);
    const uint64_t refillEnd = mach_absolute_time();

    const uint64_t deltaNs = ASFW::Timing::hostTicksToNanos(refillEnd - refillStart);
    const uint32_t deltaUs = static_cast<uint32_t>(deltaNs / 1000);
    if (deltaUs < 50) {
        latencyBucket0_.fetch_add(1, std::memory_order_relaxed);
    } else if (deltaUs < 200) {
        latencyBucket1_.fetch_add(1, std::memory_order_relaxed);
    } else if (deltaUs < 500) {
        latencyBucket2_.fetch_add(1, std::memory_order_relaxed);
    } else {
        latencyBucket3_.fetch_add(1, std::memory_order_relaxed);
    }

    uint32_t prevMax = maxRefillLatencyUs_.load(std::memory_order_relaxed);
    while (deltaUs > prevMax && !maxRefillLatencyUs_.compare_exchange_weak(
               prevMax, deltaUs, std::memory_order_relaxed, std::memory_order_relaxed)) {}

    refillInProgress_.clear(std::memory_order_release);
}

void IsochTransmitContext::WakeHardware() noexcept {
    if (!hardware_) return;
    ring_.WakeHardwareIfIdle(*hardware_, contextIndex_);
}

void IsochTransmitContext::LogStatistics() const noexcept {
}

void IsochTransmitContext::DumpDescriptorRing(uint32_t startPacket, uint32_t numPackets) const noexcept {
    ring_.DumpDescriptorRing(startPacket, numPackets);
}

} // namespace ASFW::Isoch
