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
    uint32_t interruptInterval) noexcept {

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
    metadataRing_ = reinterpret_cast<IsochTxPacketMeta*>(metadataMap_->GetAddress());

    // 3. Map Control Block
    IOMemoryMap* cMap = nullptr;
    kr = controlBlock->CreateMapping(0, 0, 0, 0, 0, &cMap);
    if (kr != kIOReturnSuccess || !cMap) {
        ASFW_LOG(Isoch, "IT: Failed to map control block: 0x%08x", kr);
        return kr;
    }
    controlMap_ = OSSharedPtr<IOMemoryMap>(cMap, OSNoRetain);
    controlBlock_ = reinterpret_cast<IsochTxQueueControl*>(controlMap_->GetAddress());

    // Populate structural fields
    uint64_t metadataLen = 0;
    metadataRing->GetLength(&metadataLen);
    uint64_t payloadLen = 0;
    payloadSlab->GetLength(&payloadLen);
    uint64_t controlLen = 0;
    controlBlock->GetLength(&controlLen);

    if (metadataLen == 0 ||
        (metadataLen % sizeof(IsochTxPacketMeta)) != 0 ||
        controlLen < sizeof(IsochTxQueueControl)) {
        ASFW_LOG(Isoch,
                 "IT: Invalid TX queue mapping metadataBytes=%llu controlBytes=%llu",
                 metadataLen, controlLen);
        return kIOReturnBadArgument;
    }

    const uint32_t numSlots = static_cast<uint32_t>(metadataLen / sizeof(IsochTxPacketMeta));
    if (numSlots == 0 || payloadLen == 0 || payloadLen % numSlots != 0) {
        ASFW_LOG(Isoch,
                 "IT: Invalid TX queue geometry payloadBytes=%llu slots=%u",
                 payloadLen, numSlots);
        return kIOReturnBadArgument;
    }
    const uint32_t maxPacketBytes = static_cast<uint32_t>(payloadLen / numSlots);

    controlBlock_->abiVersion = kTxQueueAbiVersion;
    controlBlock_->numSlots = numSlots;
    controlBlock_->slotStrideBytes = maxPacketBytes;
    controlBlock_->maxPacketBytes = maxPacketBytes;
    controlBlock_->interruptInterval = interruptInterval;
    controlBlock_->ResetConsumerForArm();
    if (controlBlock_->abiVersion != kTxQueueAbiVersion) {
        ASFW_LOG(Isoch, "IT: TX queue ABI write validation failed abi=%u",
                 controlBlock_->abiVersion);
        return kIOReturnInternalError;
    }

    ASFW_LOG(Isoch, "IT: Mapped shared memory. payloadSegments=%zu metadataRing=%p controlBlock=%p slots=%u maxBytes=%u",
             payloadDmaMap_.SegmentCount(), metadataRing_, controlBlock_, numSlots, maxPacketBytes);
    return kIOReturnSuccess;
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
    irqSilentKickStreak_ = 0;
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

    const uint64_t preFillCount = controlBlock_->committedEnd.load(std::memory_order_relaxed);
    const auto primeStats =
        ring_.Prime(payloadDmaMap_, controlBlock_->numSlots, controlBlock_->slotStrideBytes, metadataRing_, preFillCount);
    if (primeStats.packetsAssembled != Tx::Layout::kNumPackets) {
        ASFW_LOG(Isoch, "IT: Failed to prime descriptor ring against shared payload slab");
        return kIOReturnInternalError;
    }
    packetsAssembled_ = primeStats.packetsAssembled;
    controlBlock_->statusWord.store(
        IsochTxQueueStatus::kRunning,
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

kern_return_t IsochTransmitContext::Stop() noexcept {
    if (state_ == State::Running && hardware_) {
        // This gate also covers watchdog Poll().  Acquire it before clearing
        // RUN so an already-dispatched refill cannot retain a direct-audio
        // mapping past the point this function reports quiesced.
        while (refillInProgress_.test_and_set(std::memory_order_acq_rel)) {
            IODelay(5);
        }

        Register32 ctrlClrReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControlClear(contextIndex_));
        const Register32 ctrlSetReg =
            static_cast<Register32>(DMAContextHelpers::IsoXmitContextControlSet(contextIndex_));

        hardware_->Write(Register32::kIsoXmitIntMaskClear, (1u << contextIndex_));
        // A posted CLEAR must reach OHCI before ACTIVE is meaningful.  Linux
        // firewire/ohci.c:1361-1378 follows this same RUN-clear/ACTIVE-clear
        // barrier before it lets DMA resources go away.
        hardware_->WriteAndFlush(ctrlClrReg, Driver::ContextControl::kRun);

        if ((hardware_->Read(ctrlSetReg) & Driver::ContextControl::kActive) != 0) {
            IODelay(5);
            constexpr uint32_t kMaxIterations = 250;
            constexpr uint32_t kBaseDelayMicros = 6;
            for (uint32_t iteration = 0; iteration < kMaxIterations; ++iteration) {
                if ((hardware_->Read(ctrlSetReg) & Driver::ContextControl::kActive) == 0) {
                    break;
                }
                IODelay(kBaseDelayMicros + iteration);
            }
        }

        const uint32_t control = hardware_->Read(ctrlSetReg);
        if ((control & Driver::ContextControl::kActive) != 0) {
            const kern_return_t failure = (control & Driver::ContextControl::kDead) != 0
                ? kIOReturnDMAError
                : kIOReturnTimeout;
            ASFW_LOG_ERROR(Isoch,
                           "IT: stop did not quiesce context=%u control=0x%08x kr=0x%08x; retaining DMA bindings",
                           contextIndex_, control, failure);
            refillInProgress_.clear(std::memory_order_release);
            return failure;
        }

        if (controlBlock_) {
            controlBlock_->statusWord.store(IsochTxQueueStatus::kStopped, std::memory_order_release);
        }

        state_ = State::Stopped;
        refillInProgress_.clear(std::memory_order_release);
        ASFW_LOG(Isoch, "IT: Stopped. Stats: %llu pkts IRQs=%llu",
                 packetsAssembled_, interruptCount_.load(std::memory_order_relaxed));
        return kIOReturnSuccess;
    }

    if (state_ == State::Configured) {
        state_ = State::Stopped;
        refillInProgress_.clear(std::memory_order_release);
        ASFW_LOG(Isoch, "IT: Stopped from configured state before hardware run");
    }
    return kIOReturnSuccess;
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
        if (outcome.refillRequestGeneration != 0 &&
            txPreparationCallback_) {
            txPreparationCallback_(outcome.refillRequestGeneration);
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
        if (currentStatus != IsochTxQueueStatus::kProducerFault) {
            controlBlock_->statusWord.store(IsochTxQueueStatus::kDeadContext, std::memory_order_release);
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
            ++irqSilentKickStreak_;
            // Snapshot context + latched interrupt state on the anomaly path
            // only. kIntEvent latches raised events even when masked, so a
            // cycleInconsistent/cycleTooLong the dispatcher never serviced is
            // still visible here (first-fault evidence for the 2026-07-19
            // Saffire dual-context freeze, whose trigger left no interrupt
            // record).
            if (irqSilentKickStreak_ == 1 && hardware_) {
                const uint32_t ctrl = hardware_->Read(static_cast<Register32>(
                    DMAContextHelpers::IsoXmitContextControl(contextIndex_)));
                const uint32_t latchedIntEvents =
                    hardware_->Read(Register32::kIntEvent);
                ASFW_LOG(Isoch,
                         "IT: refill watchdog engaged (no IT interrupts "
                         "observed; kicks=%llu ctrl=0x%08x intEvent=0x%08x)",
                         irqWatchdogKicks_.load(std::memory_order_relaxed),
                         ctrl,
                         latchedIntEvents);
            }
            if (irqSilentKickStreak_ >= kIrqSilentKickFatalThreshold) {
                // Watchdog-carried streaming re-transmits stale descriptor
                // laps between kicks (observed Duet zombie, 2026-07-19: the
                // interrupt path died mid-session and the watchdog fed the
                // wire for 35 minutes of corrupt audio). Sustained interrupt
                // silence is a transport fault, not jitter.
                const uint32_t ctrl = hardware_
                    ? hardware_->Read(static_cast<Register32>(
                          DMAContextHelpers::IsoXmitContextControl(
                              contextIndex_)))
                    : 0;
                const uint32_t latchedIntEvents =
                    hardware_ ? hardware_->Read(Register32::kIntEvent) : 0;
                ASFW_LOG(Isoch,
                         "IT FATAL: interrupt path silent across %u "
                         "consecutive watchdog kicks; stopping context "
                         "(ctrl=0x%08x intEvent=0x%08x)",
                         irqSilentKickStreak_,
                         ctrl,
                         latchedIntEvents);
                StopImmediatelyForTxFault();
                return;
            }
            if (!refillInProgress_.test_and_set(std::memory_order_acq_rel)) {
                // Stop() may have acquired the gate after the first state
                // check. Re-check while holding it before touching the slab.
                if (state_ == State::Running) {
                    DoRefillOnce(mach_absolute_time(), /*publishTimingEvent=*/false);
                }
                refillInProgress_.clear(std::memory_order_release);
            }
        }
    } else {
        lastInterruptCountSeen_ = currentInterrupts;
        irqStallTicks_ = 0;
        irqSilentKickStreak_ = 0;
    }
}

void IsochTransmitContext::HandleInterrupt() noexcept {
    if (state_ != State::Running) return;
    interruptCount_.fetch_add(1, std::memory_order_relaxed);

    if (refillInProgress_.test_and_set(std::memory_order_acq_rel)) {
        return;
    }

    // Stop() can acquire the gate between the initial state check and this
    // point. Do not start a refill after it has fenced the stream.
    if (state_ != State::Running) {
        refillInProgress_.clear(std::memory_order_release);
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
