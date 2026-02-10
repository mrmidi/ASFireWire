// IsochTransmitContext.cpp
// ASFW - Isochronous Transmit Context
// !!!!WARNING: DON'T RELY ON OHCI 1.1 SPECS HERE!
// IT SEEMS THAT OHCI 1.2 USED
// ALLWAYS CROSS-VALIDATE WITH LINUX DRIVER
// OR AppleFWOHCI.kext decomp!

#include "IsochTransmitContext.hpp"
#include "../Config/TxBufferProfiles.hpp"
#include "../../Hardware/HardwareInterface.hpp"
#include "../../Hardware/OHCIDescriptors.hpp"
#include "../../Hardware/OHCIConstants.hpp"
#include <DriverKit/IOLib.h>
#include <atomic>

namespace ASFW::Isoch {

using namespace ASFW::Async::HW;
using namespace ASFW::Driver;

std::unique_ptr<IsochTransmitContext> IsochTransmitContext::Create(
    Driver::HardwareInterface* hw,
    std::shared_ptr<Memory::IIsochDMAMemory> dmaMemory) noexcept {

    auto ctx = std::make_unique<IsochTransmitContext>();
    if (!ctx) return nullptr;

    ctx->hardware_ = hw;
    ctx->dmaMemory_ = std::move(dmaMemory);
    
    // Create SYT generator (cycle-based, no HW dependency)
    ctx->sytGenerator_ = std::make_unique<Encoding::SYTGenerator>();

    return ctx;
}

void IsochTransmitContext::SetSharedTxQueue(void* base, uint64_t bytes) noexcept {
    if (!base || bytes == 0) {
        ASFW_LOG(Isoch, "IT: SetSharedTxQueue - invalid parameters");
        return;
    }

    if (sharedTxQueue_.Attach(base, bytes)) {
        // Consumer-owned flush only: drop stale backlog on (re)attach.
        sharedTxQueue_.ConsumerDropQueuedFrames();
        ASFW_LOG(Isoch, "IT: Shared TX queue attached - capacity=%u frames",
                 sharedTxQueue_.CapacityFrames());
    } else {
        ASFW_LOG(Isoch, "IT: Failed to attach shared TX queue - invalid header?");
    }
}

uint32_t IsochTransmitContext::SharedTxFillLevelFrames() const noexcept {
    if (!sharedTxQueue_.IsValid()) return 0;
    return sharedTxQueue_.FillLevelFrames();
}

uint32_t IsochTransmitContext::SharedTxCapacityFrames() const noexcept {
    if (!sharedTxQueue_.IsValid()) return 0;
    return sharedTxQueue_.CapacityFrames();
}

// ZERO-COPY: Set direct pointer to CoreAudio output buffer
void IsochTransmitContext::SetZeroCopyOutputBuffer(void* base, uint64_t bytes, uint32_t frameCapacity) noexcept {
    if (!base || bytes == 0 || frameCapacity == 0) {
        zeroCopyAudioBase_ = nullptr;
        zeroCopyAudioBytes_ = 0;
        zeroCopyFrameCapacity_ = 0;
        zeroCopyReadFrame_ = 0;
        zeroCopyEnabled_ = false;
        assembler_.setZeroCopySource(nullptr, 0);

        if (base || bytes || frameCapacity) {
            ASFW_LOG(Isoch, "IT: SetZeroCopyOutputBuffer - invalid parameters");
        } else {
            ASFW_LOG(Isoch, "IT: ZERO-COPY disabled; using shared TX queue");
        }
        return;
    }

    zeroCopyAudioBase_ = base;
    zeroCopyAudioBytes_ = bytes;
    zeroCopyFrameCapacity_ = frameCapacity;
    zeroCopyReadFrame_ = 0;
    zeroCopyEnabled_ = true;

    // Wire PacketAssembler to read directly from this buffer
    assembler_.setZeroCopySource(reinterpret_cast<const int32_t*>(base), frameCapacity);

    ASFW_LOG(Isoch, "IT: ✅ ZERO-COPY enabled! AudioBuffer base=%p bytes=%llu frames=%u assembler=%s",
             base, bytes, frameCapacity,
             assembler_.isZeroCopyEnabled() ? "ENABLED" : "fallback");
}

kern_return_t IsochTransmitContext::Configure(uint8_t channel, uint8_t sid, uint32_t streamModeRaw) noexcept {
    if (state_ != State::Unconfigured && state_ != State::Stopped) {
        return kIOReturnBusy;
    }
    
    channel_ = channel;
    assembler_.setSID(sid);

    requestedStreamMode_ = (streamModeRaw == 1u)
        ? Encoding::StreamMode::kBlocking
        : Encoding::StreamMode::kNonBlocking;
    effectiveStreamMode_ = requestedStreamMode_;
    assembler_.setStreamMode(effectiveStreamMode_);

    ASFW_LOG(Isoch, "IT: Stream mode=%{public}s",
             effectiveStreamMode_ == Encoding::StreamMode::kBlocking ? "blocking" : "non-blocking");
    
    if (dmaMemory_) {
        auto result = SetupRings();
        if (result != kIOReturnSuccess) {
            ASFW_LOG(Isoch, "IT: SetupRings failed");
            return result;
        }
    }
    
    state_ = State::Configured;
    ASFW_LOG(Isoch, "IT: Configured ch=%u sid=%u", channel, sid);
    return kIOReturnSuccess;
}

kern_return_t IsochTransmitContext::Start() noexcept {
    if (state_ != State::Configured && state_ != State::Stopped) {
        return kIOReturnNotReady;
    }
    
    if (!descRegion_.virtualBase) {
        ASFW_LOG(Isoch, "IT: Cannot start - no DMA ring");
        return kIOReturnNoResources;
    }

    assembler_.reset();
    packetsAssembled_ = 0;
    dataPackets_ = 0;
    noDataPackets_ = 0;
    tickCount_ = 0;
    interruptCount_ = 0;
    lastInterruptCountSeen_ = 0;
    irqStallTicks_ = 0;
    refillInProgress_.clear(std::memory_order_release);
    samplesSinceStart_ = 0;
    
    // Initialize SYT generator (cycle-based, Linux approach)
    if (sytGenerator_) {
        sytGenerator_->initialize(48000.0);  // TODO: Get from actual sample rate
        ASFW_LOG(Isoch, "IT: SYTGenerator initialized, cycle-based mode");
    }

    // Seed transmit cycle estimate from current bus time
    if (hardware_) {
        uint32_t cycleTime = hardware_->ReadCycleTime();
        uint32_t currentCycle = (cycleTime >> 12) & 0x1FFF;  // 13-bit cycle count
        // Add startup offset: a few cycles for DMA to begin after Run bit set
        nextTransmitCycle_ = (currentCycle + 4) % 8000;
        cycleTrackingValid_ = true;
        lastHwTimestamp_ = 0;
        ASFW_LOG(Isoch, "IT: Cycle tracking seeded: currentCycle=%u nextTxCycle=%u",
                 currentCycle, nextTransmitCycle_);
    }

    if (sharedTxQueue_.IsValid() && !zeroCopyEnabled_) {
        uint32_t fillBefore = sharedTxQueue_.FillLevelFrames();
        const uint32_t startupPrimeLimitFrames = Config::kTxBufferProfile.startupPrimeLimitFrames;
        uint32_t remainingPrimeFrames = startupPrimeLimitFrames;
        ASFW_LOG(Isoch, "IT: Pre-prime transfer - shared queue has %u frames (limit=%u)",
                 fillBefore, startupPrimeLimitFrames);
        
        constexpr uint32_t kTransferChunk = Config::kTransferChunkFrames;
        int32_t transferBuf[kTransferChunk * Encoding::kMaxSupportedChannels];
        uint32_t totalTransferred = 0;
        uint32_t chunkCount = 0;
        bool primeLimitHit = false;
        
        while (sharedTxQueue_.FillLevelFrames() > 0) {
            if (startupPrimeLimitFrames != 0 && remainingPrimeFrames == 0) {
                primeLimitHit = true;
                break;
            }

            uint32_t toRead = sharedTxQueue_.FillLevelFrames();
            if (toRead > kTransferChunk) toRead = kTransferChunk;
            if (startupPrimeLimitFrames != 0 && toRead > remainingPrimeFrames) {
                toRead = remainingPrimeFrames;
            }
            
            uint32_t read = sharedTxQueue_.Read(transferBuf, toRead);
            if (read == 0) break;
            
            if (chunkCount < 3) {
                ASFW_LOG(Isoch, "IT: SharedQ chunk[%u] read=%u samples=[%08x,%08x,%08x,%08x]",
                         chunkCount, read,
                         static_cast<uint32_t>(transferBuf[0]),
                         static_cast<uint32_t>(transferBuf[1]),
                         static_cast<uint32_t>(transferBuf[2]),
                         static_cast<uint32_t>(transferBuf[3]));
            }
            chunkCount++;
            
            uint32_t written = assembler_.ringBuffer().write(transferBuf, read);
            totalTransferred += written;
            if (startupPrimeLimitFrames != 0) {
                if (written >= remainingPrimeFrames) {
                    remainingPrimeFrames = 0;
                } else {
                    remainingPrimeFrames -= written;
                }
            }
            
            if (written < read) break;
        }
        
        ASFW_LOG(Isoch, "IT: Pre-prime transferred %u frames to assembler (fill=%u limit=%u hit=%s)",
                 totalTransferred,
                 assembler_.bufferFillLevel(),
                 startupPrimeLimitFrames,
                 primeLimitHit ? "YES" : "NO");
    } else if (zeroCopyEnabled_) {
        ASFW_LOG(Isoch, "IT: Pre-prime skipped (ZERO-COPY mode)");
    }

    // Fill entire descriptor slab with 0xDE pattern (helps debug page gaps)
    memset(descRegion_.virtualBase, 0xDE, kDescriptorRingSize);
    ASFW_LOG(Isoch, "IT: Pre-filled descriptor slab (%zu bytes) with 0xDE pattern", kDescriptorRingSize);

    PrimeRing();
    ASFW_LOG(Isoch, "IT: Ring primed with %llu packets", packetsAssembled_);

    DumpDescriptorRing(0, 4);
    DumpDescriptorRing(7, 1);
    
    Register32 cmdPtrReg = static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(contextIndex_));
    Register32 ctrlReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(contextIndex_));
    Register32 ctrlSetReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControlSet(contextIndex_));
    Register32 ctrlClrReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControlClear(contextIndex_));
    
    uint64_t descIOVA = descRegion_.deviceBase;
    if (descIOVA == 0 || descIOVA > 0xFFFFFFFFULL) {
        ASFW_LOG(Isoch, "IT: Invalid descriptor IOVA 0x%llx", descIOVA);
        return kIOReturnInternalError;
    }
    uint32_t cmdPtr = static_cast<uint32_t>(descIOVA) | kBlocksPerPacket;

    ASFW_LOG(Isoch, "IT: Writing CommandPtr=0x%08x (Z=%u)", cmdPtr, kBlocksPerPacket);
    hardware_->Write(cmdPtrReg, cmdPtr);
    
    hardware_->Write(ctrlClrReg, ContextControl::kWritableBits);
    
    hardware_->Write(Register32::kIsoXmitIntEventClear, 0xFFFFFFFF);
    hardware_->Write(Register32::kIsoXmitIntMaskSet, (1u << contextIndex_));
    hardware_->Write(Register32::kIntMaskSet, IntEventBits::kIsochTx);
    ASFW_LOG(Isoch, "IT: Enabled IT interrupt for context %u", contextIndex_);
    
    uint32_t ctrlSet = ContextControl::kRun;
    hardware_->Write(ctrlSetReg, ctrlSet);
    
    uint32_t readCmd = hardware_->Read(cmdPtrReg);
    uint32_t readCtl = hardware_->Read(ctrlReg);

    uint32_t isoXmitIntMask = hardware_->Read(Register32::kIsoXmitIntMaskSet);
    uint32_t intMask = hardware_->Read(Register32::kIntMaskSet);

    bool runSet = (readCtl & ContextControl::kRun) != 0;
    bool activeSet = (readCtl & ContextControl::kActive) != 0;
    bool deadSet = (readCtl & ContextControl::kDead) != 0;
    uint32_t eventCode = (readCtl & ContextControl::kEventCodeMask) >> ContextControl::kEventCodeShift;

    ASFW_LOG(Isoch, "IT: Readback Cmd=0x%08x Ctl=0x%08x (run=%d active=%d dead=%d evt=0x%02x)",
             readCmd, readCtl, runSet, activeSet, deadSet, eventCode);
    ASFW_LOG(Isoch, "IT: IntMasks - IsoXmit=0x%08x Global=0x%08x (IsochTx bit=%d)",
             isoXmitIntMask, intMask, (intMask & IntEventBits::kIsochTx) != 0);
    
    if (deadSet) {
        ASFW_LOG(Isoch, "❌ IT: Context is DEAD immediately! Check descriptor program.");
        return kIOReturnNotPermitted;
    }
    
    state_ = State::Running;
    ASFW_LOG(Isoch, "IT: Started successfully");
    return kIOReturnSuccess;
}

void IsochTransmitContext::Stop() noexcept {
    if (state_ == State::Running) {
        Register32 ctrlClrReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControlClear(contextIndex_));
        hardware_->Write(ctrlClrReg, ContextControl::kRun);
        
        hardware_->Write(Register32::kIsoXmitIntMaskClear, (1u << contextIndex_));
        
        state_ = State::Stopped;
        refillInProgress_.clear(std::memory_order_release);
        ASFW_LOG(Isoch, "IT: Stopped. Stats: %llu pkts (%lluD/%lluN) IRQs=%llu",
                 packetsAssembled_, dataPackets_, noDataPackets_,
                 interruptCount_.load(std::memory_order_relaxed));
    }
}

void IsochTransmitContext::Poll() noexcept {
    if (state_ != State::Running) return;
    ++tickCount_;

    // IRQ-stall watchdog:
    // If IT interrupts stop arriving while context is still running, kick refill/wake
    // from the 1ms poll path to keep DMA fed and recover from missed IRQ windows.
    uint64_t irqNow = interruptCount_.load(std::memory_order_relaxed);
    if (irqNow != lastInterruptCountSeen_) {
        lastInterruptCountSeen_ = irqNow;
        irqStallTicks_ = 0;
    } else {
        ++irqStallTicks_;
    }

    constexpr uint32_t kIrqStallThresholdTicks = 2;  // ~2ms @ 1ms watchdog tick
    if (irqStallTicks_ >= kIrqStallThresholdTicks) {
        if (!refillInProgress_.test_and_set(std::memory_order_acq_rel)) {
            RefillRing();
            refillInProgress_.clear(std::memory_order_release);
        }
        WakeHardware();
        rtRefill_.irqWatchdogKicks.fetch_add(1, std::memory_order_relaxed);
        irqStallTicks_ = 0;
    }

    // Periodic non-RT diagnostics.
    if (tickCount_ == 1 || (tickCount_ % 1000) == 0) {
        const uint32_t rbFill = assembler_.bufferFillLevel();
        const uint32_t txFill = sharedTxQueue_.IsValid() ? sharedTxQueue_.FillLevelFrames() : 0;

        const auto load = [](const std::atomic<uint64_t>& c) noexcept {
            return c.load(std::memory_order_relaxed);
        };

        RTRefillSnapshot cur{};
        cur.calls = load(rtRefill_.calls);
        cur.exitNotRunning = load(rtRefill_.exitNotRunning);
        cur.exitDead = load(rtRefill_.exitDead);
        cur.exitDecodeFail = load(rtRefill_.exitDecodeFail);
        cur.exitHwOOB = load(rtRefill_.exitHwOOB);
        cur.exitZeroRefill = load(rtRefill_.exitZeroRefill);
        cur.resyncApplied = load(rtRefill_.resyncApplied);
        cur.staleFramesDropped = load(rtRefill_.staleFramesDropped);
        cur.refills = load(rtRefill_.refills);
        cur.packetsRefilled = load(rtRefill_.packetsRefilled);
        cur.legacyPumpMovedFrames = load(rtRefill_.legacyPumpMovedFrames);
        cur.legacyPumpSkipped = load(rtRefill_.legacyPumpSkipped);
        cur.fatalPacketSize = load(rtRefill_.fatalPacketSize);
        cur.fatalDescriptorBounds = load(rtRefill_.fatalDescriptorBounds);
        cur.irqWatchdogKicks = load(rtRefill_.irqWatchdogKicks);

        RTRefillSnapshot delta{};
        delta.calls = cur.calls - rtRefillLast_.calls;
        delta.exitNotRunning = cur.exitNotRunning - rtRefillLast_.exitNotRunning;
        delta.exitDead = cur.exitDead - rtRefillLast_.exitDead;
        delta.exitDecodeFail = cur.exitDecodeFail - rtRefillLast_.exitDecodeFail;
        delta.exitHwOOB = cur.exitHwOOB - rtRefillLast_.exitHwOOB;
        delta.exitZeroRefill = cur.exitZeroRefill - rtRefillLast_.exitZeroRefill;
        delta.resyncApplied = cur.resyncApplied - rtRefillLast_.resyncApplied;
        delta.staleFramesDropped = cur.staleFramesDropped - rtRefillLast_.staleFramesDropped;
        delta.refills = cur.refills - rtRefillLast_.refills;
        delta.packetsRefilled = cur.packetsRefilled - rtRefillLast_.packetsRefilled;
        delta.legacyPumpMovedFrames = cur.legacyPumpMovedFrames - rtRefillLast_.legacyPumpMovedFrames;
        delta.legacyPumpSkipped = cur.legacyPumpSkipped - rtRefillLast_.legacyPumpSkipped;
        delta.fatalPacketSize = cur.fatalPacketSize - rtRefillLast_.fatalPacketSize;
        delta.fatalDescriptorBounds = cur.fatalDescriptorBounds - rtRefillLast_.fatalDescriptorBounds;
        delta.irqWatchdogKicks = cur.irqWatchdogKicks - rtRefillLast_.irqWatchdogKicks;
        rtRefillLast_ = cur;

        ASFW_LOG(Isoch, "IT: Poll tick=%llu zeroCopy=%{public}s rbFill=%u txFill=%u readPos=%u/%u | RefillΔ calls=%llu refills=%llu pkts=%llu legacy(moved=%llu skip=%llu) exits(nr=%llu dead=%llu dec=%llu oob=%llu zero=%llu) sync(resync=%llu drop=%llu) watchdog=%llu fatal(sz=%llu,bounds=%llu)",
                 tickCount_,
                 zeroCopyEnabled_ ? "YES" : "NO",
                 rbFill,
                 txFill,
                 assembler_.zeroCopyReadPosition(),
                 zeroCopyFrameCapacity_,
                 delta.calls,
                 delta.refills,
                 delta.packetsRefilled,
                 delta.legacyPumpMovedFrames,
                 delta.legacyPumpSkipped,
                 delta.exitNotRunning,
                 delta.exitDead,
                 delta.exitDecodeFail,
                 delta.exitHwOOB,
                 delta.exitZeroRefill,
                 delta.resyncApplied,
                 delta.staleFramesDropped,
                 delta.irqWatchdogKicks,
                 delta.fatalPacketSize,
                 delta.fatalDescriptorBounds);
    }
}

void IsochTransmitContext::HandleInterrupt() noexcept {
    if (state_ != State::Running) return;
    interruptCount_.fetch_add(1, std::memory_order_relaxed);

    // Avoid concurrent refill from ISR + watchdog poll path.
    if (refillInProgress_.test_and_set(std::memory_order_acq_rel)) {
        return;
    }
    RefillRing();
    refillInProgress_.clear(std::memory_order_release);
}

void IsochTransmitContext::WakeHardware() noexcept {
    Register32 ctrlReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(contextIndex_));
    const uint32_t ctrl = hardware_->Read(ctrlReg);

    const bool run = (ctrl & ContextControl::kRun) != 0;
    const bool dead = (ctrl & ContextControl::kDead) != 0;
    const bool active = (ctrl & ContextControl::kActive) != 0;

    if (run && !dead && !active) {
        Register32 ctrlSetReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControlSet(contextIndex_));
        hardware_->Write(ctrlSetReg, ContextControl::kWake);
    }
}

kern_return_t IsochTransmitContext::SetupRings() noexcept {
    if (!dmaMemory_) return kIOReturnNoMemory;

    // Allocate descriptor ring - request 4K alignment for page gap calculation
    auto descR = dmaMemory_->AllocateDescriptor(kDescriptorRingSize);
    if (!descR) return kIOReturnNoMemory;
    descRegion_ = *descR;

    auto bufR = dmaMemory_->AllocatePayloadBuffer(kPayloadBufferSize);
    if (!bufR) return kIOReturnNoMemory;
    bufRegion_ = *bufR;

    if (descRegion_.deviceBase > 0xFFFFFFFFULL || bufRegion_.deviceBase > 0xFFFFFFFFULL) {
        ASFW_LOG(Isoch, "IT: SetupRings - IOVA out of 32-bit range: desc=0x%llx buf=0x%llx",
                 descRegion_.deviceBase, bufRegion_.deviceBase);
        return kIOReturnNoResources;
    }
    
    // Check 16-byte alignment (minimum for OHCI descriptors)
    if ((descRegion_.deviceBase & 0xFULL) != 0) {
        ASFW_LOG(Isoch, "IT: SetupRings - descriptor base not 16B aligned: 0x%llx",
                 descRegion_.deviceBase);
        return kIOReturnNoResources;
    }
    
    // CRITICAL: Check 4K alignment for page gap calculation
    // Our GetDescriptorIOVA() assumes base is 4K-aligned so page offsets line up
    uint64_t pageOffset = descRegion_.deviceBase & (kOHCIPageSize - 1);
    if (pageOffset != 0) {
        ASFW_LOG(Isoch, "❌ IT: SetupRings - descriptor base NOT 4K aligned! "
                 "IOVA=0x%llx pageOffset=0x%llx - page gap calculation WILL BE WRONG, failing",
                 descRegion_.deviceBase, pageOffset);
        return kIOReturnNoResources;
    }

    // Zero the entire slab (will be filled with 0xDE in Start())
    memset(descRegion_.virtualBase, 0, kDescriptorRingSize);

    ASFW_LOG(Isoch, "IT: Rings Ready. DescIOVA=0x%llx (pageOff=0x%llx) BufIOVA=0x%llx",
             descRegion_.deviceBase, pageOffset, bufRegion_.deviceBase);
    ASFW_LOG(Isoch, "IT: Layout: %u packets, %u blocks, %u pages, %zu bytes/page usable",
             kNumPackets, kRingBlocks, kTotalPages, static_cast<size_t>(kDescriptorsPerPage * kDescriptorStride));

    return kIOReturnSuccess;
}

// =============================================================================
// Page-aware descriptor access helpers (Linux-style padding)
// =============================================================================

OHCIDescriptor* IsochTransmitContext::GetDescriptorPtr(uint32_t logicalIndex) noexcept {
    // Calculate which 4K page this descriptor is on
    uint32_t page = logicalIndex / kDescriptorsPerPage;
    uint32_t offsetInPage = (logicalIndex % kDescriptorsPerPage) * kDescriptorStride;
    
    uint8_t* base = reinterpret_cast<uint8_t*>(descRegion_.virtualBase);
    return reinterpret_cast<OHCIDescriptor*>(base + (page * kOHCIPageSize) + offsetInPage);
}

const OHCIDescriptor* IsochTransmitContext::GetDescriptorPtr(uint32_t logicalIndex) const noexcept {
    uint32_t page = logicalIndex / kDescriptorsPerPage;
    uint32_t offsetInPage = (logicalIndex % kDescriptorsPerPage) * kDescriptorStride;
    
    const uint8_t* base = reinterpret_cast<const uint8_t*>(descRegion_.virtualBase);
    return reinterpret_cast<const OHCIDescriptor*>(base + (page * kOHCIPageSize) + offsetInPage);
}

uint32_t IsochTransmitContext::GetDescriptorIOVA(uint32_t logicalIndex) const noexcept {
    uint32_t page = logicalIndex / kDescriptorsPerPage;
    uint32_t offsetInPage = (logicalIndex % kDescriptorsPerPage) * kDescriptorStride;
    
    return static_cast<uint32_t>(descRegion_.deviceBase) + 
           (page * static_cast<uint32_t>(kOHCIPageSize)) + offsetInPage;
}

bool IsochTransmitContext::DecodeCmdAddrToLogicalIndex(uint32_t cmdAddr, uint32_t& outLogicalIndex) const noexcept {
    uint32_t baseAddr = static_cast<uint32_t>(descRegion_.deviceBase);
    
    // Sanity checks
    if (cmdAddr < baseAddr) return false;
    if ((cmdAddr & 0xFu) != 0) return false;  // Must be 16-byte aligned
    
    uint32_t offset = cmdAddr - baseAddr;
    uint32_t page = offset / static_cast<uint32_t>(kOHCIPageSize);
    uint32_t offsetInPage = offset % static_cast<uint32_t>(kOHCIPageSize);
    
    // Check if page is valid
    if (page >= kTotalPages) return false;
    
    // Check if in padding zone (last 64 bytes of page are unused with 252 descs/page)
    uint32_t usableBytes = kDescriptorsPerPage * kDescriptorStride;
    if (offsetInPage >= usableBytes) return false;
    
    // Check alignment
    if ((offsetInPage % kDescriptorStride) != 0) return false;
    
    uint32_t indexInPage = offsetInPage / kDescriptorStride;
    outLogicalIndex = (page * kDescriptorsPerPage) + indexInPage;
    
    return outLogicalIndex < kRingBlocks;
}

void IsochTransmitContext::ValidateDescriptorLayout() const noexcept {
#ifndef NDEBUG
    // Validate all descriptors are in safe zone (offset < 0xFE0)
    bool allSafe = true;
    for (uint32_t i = 0; i < kRingBlocks; ++i) {
        uint32_t iova = GetDescriptorIOVA(i);
        uint32_t pageOffset = iova & (kOHCIPageSize - 1);
        if (pageOffset >= (kOHCIPageSize - kOHCIPrefetchSize)) {
            ASFW_LOG(Isoch, "❌ IT: Descriptor %u in DANGER ZONE! IOVA=0x%08x pageOff=0x%03x",
                     i, iova, pageOffset);
            allSafe = false;
        }
    }
    
    // Validate packet alignment: each packet's 3 blocks on same page
    for (uint32_t pkt = 0; pkt < kNumPackets; ++pkt) {
        uint32_t base = pkt * kBlocksPerPacket;
        uint32_t page0 = GetDescriptorIOVA(base) / kOHCIPageSize;
        uint32_t page1 = GetDescriptorIOVA(base + 1) / kOHCIPageSize;
        uint32_t page2 = GetDescriptorIOVA(base + 2) / kOHCIPageSize;
        if (page0 != page1 || page1 != page2) {
            ASFW_LOG(Isoch, "❌ IT: Packet %u straddles pages! pages=[%u,%u,%u]",
                     pkt, page0, page1, page2);
            allSafe = false;
        }
    }
    
    if (allSafe) {
        ASFW_LOG(Isoch, "✅ IT: Layout validation PASSED (%u descriptors, %u packets, %u pages)",
                 kRingBlocks, kNumPackets, kTotalPages);
    }
#endif
}

void IsochTransmitContext::PrimeRing() noexcept {
    // Use kNumPackets - don't derive from capacity (incompatible with page gaps)
    constexpr uint32_t numPackets = kNumPackets;
    
    ASFW_LOG(Isoch, "IT: PrimeRing - packets=%u blocks=%u pages=%u descPerPage=%u",
             numPackets, kRingBlocks, kTotalPages, kDescriptorsPerPage);
    
    // Validate layout in debug builds (once, before any descriptor writes)
    ValidateDescriptorLayout();
    
    for (uint32_t pktIdx = 0; pktIdx < numPackets; ++pktIdx) {
        // Cycle-aware SYT: only for DATA packets
        uint16_t syt = Encoding::SYTGenerator::kNoInfo;
        bool willBeData = assembler_.nextIsData();
        if (willBeData && sytGenerator_ && sytGenerator_->isValid() && cycleTrackingValid_) {
            syt = sytGenerator_->computeDataSYT(nextTransmitCycle_);
        }

        auto pkt = assembler_.assembleNext(syt);
        nextTransmitCycle_ = (nextTransmitCycle_ + 1) % 8000;  // One packet per bus cycle

        if (pkt.size > kMaxPacketSize) {
            ASFW_LOG(Isoch, "IT: FATAL pkt.size=%u > kMaxPacketSize=%u pktIdx=%u",
                     pkt.size, kMaxPacketSize, pktIdx);
            return;
        }
        if (pkt.size > 0xFFFFu) {
            ASFW_LOG(Isoch, "IT: FATAL pkt.size=%u exceeds 16-bit dataLength pktIdx=%u",
                     pkt.size, pktIdx);
            return;
        }

        uint32_t descBase = pktIdx * kBlocksPerPacket;
        uint32_t nextPktBase = ((pktIdx + 1) % numPackets) * kBlocksPerPacket;
        
        if (descBase >= kRingBlocks || (descBase + kBlocksPerPacket - 1) >= kRingBlocks) {
            ASFW_LOG(Isoch, "IT: ❌ FATAL: descBase=%u OUT OF BOUNDS (max=%u) pktIdx=%u",
                     descBase, kRingBlocks - 1, pktIdx);
            return;
        }
        
        // Use page-aware IOVA calculation for branch address
        uint32_t nextBlockIOVA = GetDescriptorIOVA(nextPktBase);
        
        uint8_t* payloadVirt = reinterpret_cast<uint8_t*>(bufRegion_.virtualBase) + 
                               (pktIdx * kMaxPacketSize);
        uint32_t payloadIOVA = static_cast<uint32_t>(bufRegion_.deviceBase) + 
                               static_cast<uint32_t>(pktIdx * kMaxPacketSize);
        
        if (pkt.size > 0) {
            uint32_t* dst32 = reinterpret_cast<uint32_t*>(payloadVirt);
            const uint32_t* src32 = reinterpret_cast<const uint32_t*>(pkt.data);
            const size_t count32 = pkt.size / 4;
            
            for (size_t k = 0; k < count32; ++k) {
                dst32[k] = src32[k];
            }
        }
        
        uint32_t isochHeaderQ0 =
            ((2u & 0x7) << 16) |
            ((1u & 0x3) << 14) |
            ((channel_ & 0x3F) << 8) |
            ((0xAu & 0xF) << 4) |
            (0u & 0xF);
        
        const uint32_t isochHeaderQ1 =
            static_cast<uint32_t>(static_cast<uint16_t>(pkt.size)) << 16;
        
        // Use page-aware pointer for descriptor access
        auto* immDesc = reinterpret_cast<OHCIDescriptorImmediate*>(GetDescriptorPtr(descBase));

        immDesc->common.control = (0x0200u << 16) | 8;
        
        // Linux/Apple-style layout (per AppleFWOHCI.kext decompilation):
        // - dataAddress (offset 0x04): unused, set to 0
        // - branchWord (offset 0x08): skipAddress | Z
        // - statusWord (offset 0x0C): unused, set to 0
        // - immediateData[0] (offset 0x10): isoch header Q0
        // - immediateData[1] (offset 0x14): isoch header Q1
        immDesc->common.dataAddress = 0;  // Unused in Apple/Linux layout
        immDesc->common.branchWord = (nextBlockIOVA & 0xFFFFFFF0u) | kBlocksPerPacket;  // skipAddr|Z
        immDesc->common.statusWord = 0;  // Unused in Apple/Linux layout
        
        // Isoch header at offset 0x10 (immediateData[0-1])
        immDesc->immediateData[0] = isochHeaderQ0;
        immDesc->immediateData[1] = isochHeaderQ1;
        immDesc->immediateData[2] = 0;
        immDesc->immediateData[3] = 0;

        // Use page-aware pointer for OUTPUT_LAST descriptor
        auto* lastDesc = GetDescriptorPtr(descBase + 2);

        uint16_t lastReqCount = static_cast<uint16_t>(pkt.size);

        uint8_t intBits = ((pktIdx % 8) == 7) ? OHCIDescriptor::kIntAlways : OHCIDescriptor::kIntNever;

        // Build OUTPUT_LAST control word per OHCI spec (cross-validated with Linux ohci.c):
        // [31:28] cmd=1, [27] s=1, [26:24] key=0, [21:20] i, [19:18] b, [15:0] reqCount
        uint32_t lastControl =
            (0x1u << 28) |                          // cmd = OUTPUT_LAST
            (0x1u << 27) |                          // s = status update
            (0x0u << 24) |                          // key = standard
            (static_cast<uint32_t>(intBits) << 20) | // i at bits 21:20 (was wrong at 18)
            (0x3u << 18) |                          // b = BRANCH_ALWAYS at bits 19:18 (was wrong at 16)
            lastReqCount;

        lastDesc->control = lastControl;
        lastDesc->dataAddress = payloadIOVA;
        lastDesc->branchWord = (nextBlockIOVA & 0xFFFFFFF0u) | kBlocksPerPacket;
        lastDesc->statusWord = 0;
        
        ++packetsAssembled_;
        if (pkt.isData) {
            ++dataPackets_;
            samplesSinceStart_ += assembler_.samplesPerDataPacket();
        } else {
            ++noDataPackets_;
        }
    }
    
    std::atomic_thread_fence(std::memory_order_release);
    ASFW::Driver::WriteBarrier();
}

void IsochTransmitContext::RefillRing() noexcept {
    rtRefill_.calls.fetch_add(1, std::memory_order_relaxed);
    
    if (!hardware_ || state_ != State::Running) {
        rtRefill_.exitNotRunning.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (sharedTxQueue_.IsValid() && sharedTxQueue_.ConsumerApplyPendingResync()) {
        rtRefill_.resyncApplied.fetch_add(1, std::memory_order_relaxed);
    }

    Register32 ctrlReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(contextIndex_));
    uint32_t ctrl = hardware_->Read(ctrlReg);
    bool dead = (ctrl & ContextControl::kDead) != 0;
    if (dead) {
        rtRefill_.exitDead.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    Register32 cmdPtrReg = static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(contextIndex_));
    uint32_t cmdPtr = hardware_->Read(cmdPtrReg);
    uint32_t cmdAddr = cmdPtr & 0xFFFFFFF0u;

    // Legacy (non-zero-copy) path: keep assembler ring near a target fill.
    // This makes sharedTxQueue the effective jitter buffer instead of draining ASAP.
    if (!zeroCopyEnabled_ && sharedTxQueue_.IsValid()) {
        constexpr uint32_t kTargetRbFillFrames = Config::kTxBufferProfile.legacyRbTargetFrames;
        constexpr uint32_t kMaxRbFillFrames = Config::kTxBufferProfile.legacyRbMaxFrames;
        constexpr uint32_t kTransferChunkFrames = Config::kTransferChunkFrames;
        constexpr uint32_t kMaxChunksPerRefill = Config::kTxBufferProfile.legacyMaxChunksPerRefill;

        uint32_t rbFill = assembler_.bufferFillLevel();
        uint32_t pumpedFrames = 0;
        bool skipped = true;

        if (rbFill < kTargetRbFillFrames) {
            skipped = false;
            uint32_t want = kTargetRbFillFrames - rbFill;
            int32_t transferBuf[kTransferChunkFrames * Encoding::kMaxSupportedChannels];
            uint32_t chunks = 0;

            while (want > 0 && chunks < kMaxChunksPerRefill) {
                uint32_t qFill = sharedTxQueue_.FillLevelFrames();
                if (qFill == 0) {
                    break;
                }

                uint32_t rbSpace = assembler_.ringBuffer().availableSpace();
                if (rbSpace == 0) {
                    break;
                }

                uint32_t toRead = want;
                if (toRead > qFill) toRead = qFill;
                if (toRead > rbSpace) toRead = rbSpace;
                if (toRead > kTransferChunkFrames) toRead = kTransferChunkFrames;

                uint32_t read = sharedTxQueue_.Read(transferBuf, toRead);
                if (read == 0) {
                    break;
                }

                uint32_t written = assembler_.ringBuffer().write(transferBuf, read);
                pumpedFrames += written;
                if (written < read) {
                    break;
                }

                want -= written;
                ++chunks;

                if (assembler_.bufferFillLevel() >= kMaxRbFillFrames) {
                    break;
                }
            }
        }

        if (skipped) {
            rtRefill_.legacyPumpSkipped.fetch_add(1, std::memory_order_relaxed);
        } else {
            rtRefill_.legacyPumpMovedFrames.fetch_add(pumpedFrames, std::memory_order_relaxed);
        }
    }

    // Use page-aware inverse mapping for cmdPtr decoding (critical fix!)
    uint32_t hwLogicalIndex;
    if (!DecodeCmdAddrToLogicalIndex(cmdAddr, hwLogicalIndex)) {
        rtRefill_.exitDecodeFail.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    
    uint32_t hwPacketIndex = hwLogicalIndex / kBlocksPerPacket;
    constexpr uint32_t numPackets = kNumPackets;

    if (hwPacketIndex >= numPackets) {
        rtRefill_.exitHwOOB.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // ==========================================================================
    // HARDWARE-DELTA REFILL (v8 fix)
    // Refill exactly as many packets as hardware consumed since last call.
    // This guarantees software refill rate == hardware consumption rate.
    // ==========================================================================
    
    const uint32_t prevHwPacketIndex = lastHwPacketIndex_;
    uint32_t deltaConsumed = 0;
    if (hwPacketIndex >= prevHwPacketIndex) {
        deltaConsumed = hwPacketIndex - prevHwPacketIndex;
    } else {
        // Wrapped around
        deltaConsumed = (numPackets - prevHwPacketIndex) + hwPacketIndex;
    }
    
    // Update tracker for next call
    lastHwPacketIndex_ = hwPacketIndex;
    
    if (deltaConsumed == 0) {
        // Nothing consumed since last call - nothing to refill
        return;
    }

    // === CYCLE RESYNC from hardware timestamp ===
    // Before overwriting consumed descriptors, read the timeStamp from one of them.
    // The OUTPUT_LAST descriptor (descBase + 2) has s=1, so OHCI writes timeStamp after TX.
    if (cycleTrackingValid_) {
        // Read timestamp from the packet just before current HW position
        uint32_t lastProcessedPkt = (hwPacketIndex + numPackets - 1) % numPackets;
        auto* processedOL = GetDescriptorPtr(lastProcessedPkt * kBlocksPerPacket + 2);

        uint16_t hwTimestamp = static_cast<uint16_t>(processedOL->statusWord & 0xFFFF);

        if (processedOL->statusWord != 0) {
            // Hardware wrote valid status — extract cycle
            uint32_t hwCycle = hwTimestamp & 0x1FFF;  // 13-bit cycle count
            lastHwTimestamp_ = hwTimestamp;

            // Recompute nextTransmitCycle_ based on actual hardware cycle.
            // lastProcessedPkt was transmitted at hwCycle.
            // softwareFillIndex_ is where we'll write next.
            // Distance from lastProcessedPkt to softwareFillIndex_ (in ring):
            uint32_t aheadCount = (softwareFillIndex_ + numPackets - lastProcessedPkt) % numPackets;
            nextTransmitCycle_ = (hwCycle + aheadCount) % 8000;
        }
    }

    // Cap per-call refill to avoid excessive time in interrupt context
    // But allow multi-pass to catch up if needed
    const uint32_t kMaxRefillPerPass = 16;
    const uint32_t kMaxPasses = 4;
    
    uint32_t packetsRemaining = deltaConsumed;
    uint32_t passes = 0;
    uint32_t totalRefilled = 0;
    rtRefill_.refills.fetch_add(1, std::memory_order_relaxed);

    const bool zeroCopySync = zeroCopyEnabled_ && sharedTxQueue_.IsValid() && zeroCopyFrameCapacity_ > 0;

    while (packetsRemaining > 0 && passes < kMaxPasses) {
        uint32_t packetsToRefill = (packetsRemaining > kMaxRefillPerPass) ? kMaxRefillPerPass : packetsRemaining;

        for (uint32_t i = 0; i < packetsToRefill; ++i) {
            uint32_t pktIdx = (softwareFillIndex_ + i) % numPackets;

            // Cycle-aware SYT: only for DATA packets
            uint16_t syt = Encoding::SYTGenerator::kNoInfo;
            bool willBeData = assembler_.nextIsData();
            if (willBeData && sytGenerator_ && sytGenerator_->isValid() && cycleTrackingValid_) {
                syt = sytGenerator_->computeDataSYT(nextTransmitCycle_);
            }

            uint32_t zeroCopyFillBefore = 0;
            if (zeroCopySync && willBeData) {
                // Acquire producer progress, then align assembler read pointer
                // to shared read index + producer-supplied phase.
                zeroCopyFillBefore = sharedTxQueue_.FillLevelFrames();

                // Zero-copy storage retains only zeroCopyFrameCapacity_ frames.
                // If queue lag exceeds this, drop stale backlog before reading.
                if (zeroCopyFillBefore > zeroCopyFrameCapacity_) {
                    const uint32_t drop = zeroCopyFillBefore - zeroCopyFrameCapacity_;
                    const uint32_t dropped = sharedTxQueue_.ConsumeFrames(drop);
                    rtRefill_.staleFramesDropped.fetch_add(dropped, std::memory_order_relaxed);
                    zeroCopyFillBefore -= dropped;
                }

                uint32_t readAbs = sharedTxQueue_.ReadIndexFrames();
                const uint32_t phase = sharedTxQueue_.ZeroCopyPhaseFrames() % zeroCopyFrameCapacity_;
                assembler_.setZeroCopyReadPosition((readAbs + phase) % zeroCopyFrameCapacity_);
            }

            auto pkt = assembler_.assembleNext(syt);
            nextTransmitCycle_ = (nextTransmitCycle_ + 1) % 8000;  // One packet per bus cycle

            if (zeroCopySync && pkt.isData) {
                const uint32_t framesPerPacket = assembler_.samplesPerDataPacket();
                uint32_t consumed = sharedTxQueue_.ConsumeFrames(framesPerPacket);
                if (consumed < framesPerPacket ||
                    zeroCopyFillBefore < framesPerPacket) {
                    rtRefill_.exitZeroRefill.fetch_add(1, std::memory_order_relaxed);
                    // Preserve DATA cadence/DBC while avoiding stale buffer data on underrun.
                    memset(pkt.data + Encoding::kCIPHeaderSize, 0,
                           assembler_.dataPacketSize() - Encoding::kCIPHeaderSize);
                }
            }

            if (pkt.size > kMaxPacketSize || pkt.size > 0xFFFFu) {
                rtRefill_.fatalPacketSize.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            uint32_t descBase = pktIdx * kBlocksPerPacket;

            if (descBase >= kRingBlocks || (descBase + kBlocksPerPacket - 1) >= kRingBlocks) {
                rtRefill_.fatalDescriptorBounds.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            uint8_t* payloadVirt = reinterpret_cast<uint8_t*>(bufRegion_.virtualBase) +
                                   (pktIdx * kMaxPacketSize);
            uint32_t payloadIOVA = static_cast<uint32_t>(bufRegion_.deviceBase) +
                                   static_cast<uint32_t>(pktIdx * kMaxPacketSize);

            if (pkt.size > 0) {
                uint32_t* dst32 = reinterpret_cast<uint32_t*>(payloadVirt);
                const uint32_t* src32 = reinterpret_cast<const uint32_t*>(pkt.data);
                const size_t count32 = pkt.size / 4;
                for (size_t k = 0; k < count32; ++k) {
                    dst32[k] = src32[k];
                }
            }

            // Use page-aware pointer for OUTPUT_LAST descriptor
            auto* lastDesc = GetDescriptorPtr(descBase + 2);
            uint16_t lastReqCount = static_cast<uint16_t>(pkt.size);

            uint32_t existingControl = lastDesc->control & 0xFFFF0000u;
            lastDesc->control = existingControl | lastReqCount;
            lastDesc->dataAddress = payloadIOVA;
            lastDesc->statusWord = 0;

            // Use page-aware pointer for OUTPUT_MORE-Immediate descriptor
            auto* immDesc = reinterpret_cast<OHCIDescriptorImmediate*>(GetDescriptorPtr(descBase));
            uint32_t isochHeaderQ1 = (static_cast<uint32_t>(pkt.size) & 0xFFFFu) << 16;
            immDesc->immediateData[1] = isochHeaderQ1;

            ++packetsAssembled_;
            if (pkt.isData) {
                ++dataPackets_;
                samplesSinceStart_ += assembler_.samplesPerDataPacket();
            } else {
                ++noDataPackets_;
            }
        }

        std::atomic_thread_fence(std::memory_order_release);
        ASFW::Driver::WriteBarrier();

        softwareFillIndex_ = (softwareFillIndex_ + packetsToRefill) % numPackets;
        totalRefilled += packetsToRefill;
        packetsRemaining -= packetsToRefill;
        passes++;
    }
    
    rtRefill_.packetsRefilled.fetch_add(totalRefilled, std::memory_order_relaxed);
}
    
void IsochTransmitContext::LogStatistics() const noexcept {
#if 0
// seems like it's working as expected, but keep this here for future diagnostics if needed
    if (hardware_) {
        uint32_t cmdPtr = hardware_->Read(static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(contextIndex_)));
        uint32_t ctrl = hardware_->Read(static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(contextIndex_)));

        bool runSet = (ctrl & ContextControl::kRun) != 0;
        bool activeSet = (ctrl & ContextControl::kActive) != 0;
        bool deadSet = (ctrl & ContextControl::kDead) != 0;
        uint32_t eventCode = (ctrl & ContextControl::kEventCodeMask) >> ContextControl::kEventCodeShift;
        
        ASFW_LOG(Isoch, "IT: run=%d active=%d dead=%d evt=0x%02x pkts=%llu IRQ=%llu | CmdPtr=0x%08x Ctrl=0x%08x",
                 runSet, activeSet, deadSet, eventCode,
                 packetsAssembled_,
                 interruptCount_,
                 cmdPtr, ctrl);
        
        static uint64_t dumpCounter = 0;
        if ((dumpCounter++ % 10) == 0) {
            // Use page-aware pointer access
            const auto* desc0 = GetDescriptorPtr(0);
            const auto* desc1 = GetDescriptorPtr(1);
            const auto* desc2 = GetDescriptorPtr(2);
            ASFW_LOG(Isoch, "  Block0[0]: ctl=0x%08x dat=0x%08x br=0x%08x st=0x%08x",
                     desc0->control, desc0->dataAddress, desc0->branchWord, desc0->statusWord);
            ASFW_LOG(Isoch, "  Block0[1]: ctl=0x%08x dat=0x%08x br=0x%08x st=0x%08x",
                     desc1->control, desc1->dataAddress, desc1->branchWord, desc1->statusWord);
            ASFW_LOG(Isoch, "  Block0[2]: ctl=0x%08x dat=0x%08x br=0x%08x st=0x%08x",
                     desc2->control, desc2->dataAddress, desc2->branchWord, desc2->statusWord);
        }
        
        if (deadSet) {
            DumpAtCmdPtr();
        }
    }
#endif
}

void IsochTransmitContext::DumpPayloadBuffers(uint32_t numPackets) const noexcept {
    if (!bufRegion_.virtualBase) {
        ASFW_LOG(Isoch, "IT: DumpPayloadBuffers - no buffer allocated");
        return;
    }

    const size_t numTotalPackets = kRingBlocks / kBlocksPerPacket;
    if (numPackets > numTotalPackets) numPackets = numTotalPackets;

    ASFW_LOG(Isoch, "IT: === DMA Payload Buffer Dump (first %u of %zu packets) ===", numPackets, numTotalPackets);

    for (uint32_t pktIdx = 0; pktIdx < numPackets; ++pktIdx) {
        uint8_t* payloadVirt = reinterpret_cast<uint8_t*>(bufRegion_.virtualBase) +
                               (pktIdx * kMaxPacketSize);
        uint32_t* payload32 = reinterpret_cast<uint32_t*>(payloadVirt);

        uint32_t cip0 = payload32[0];
        uint32_t cip1 = payload32[1];

        uint32_t aud0 = payload32[2];
        uint32_t aud1 = payload32[3];
        uint32_t aud2 = payload32[4];
        uint32_t aud3 = payload32[5];

        bool isNoData = (aud0 == 0 && aud1 == 0);
        bool isSilence = ((aud0 & 0xFFFFFF) == 0) && ((aud1 & 0xFFFFFF) == 0);

        ASFW_LOG(Isoch, "  Pkt[%u] CIP=[%08x %08x] Audio=[%08x %08x %08x %08x] %s%s",
                 pktIdx, cip0, cip1, aud0, aud1, aud2, aud3,
                 isNoData ? "NO-DATA" : "DATA",
                 (isSilence && !isNoData) ? " (SILENCE!)" : "");
    }

    ASFW_LOG(Isoch, "IT: === End DMA Buffer Dump ===");
}

void IsochTransmitContext::DumpAtCmdPtr() const noexcept {
#ifndef ASFW_HOST_TEST
    if (!hardware_) return;

    Register32 cmdPtrReg = static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(contextIndex_));
    const uint32_t cmdPtr = hardware_->Read(cmdPtrReg);
    const uint32_t addr = cmdPtr & 0xFFFFFFF0u;
    const uint32_t z = cmdPtr & 0xF;

    const uint32_t base = static_cast<uint32_t>(descRegion_.deviceBase);
    
    ASFW_LOG(Isoch, "IT: DumpAtCmdPtr: cmdPtr=0x%08x addr=0x%08x Z=%u (base=0x%08x)",
             cmdPtr, addr, z, base);

    // Use page-aware decoding
    uint32_t logicalIdx;
    if (!DecodeCmdAddrToLogicalIndex(addr, logicalIdx)) {
        ASFW_LOG(Isoch, "IT: CmdPtr decode FAILED - addr=0x%08x outside ring or in padding", addr);
        return;
    }

    ASFW_LOG(Isoch, "IT: CmdPtr decoded to logicalIdx=%u (packet=%u, block=%u)",
             logicalIdx, logicalIdx / kBlocksPerPacket, logicalIdx % kBlocksPerPacket);

    for (uint32_t k = 0; k < 4 && (logicalIdx + k) < kRingBlocks; ++k) {
        const auto* b = GetDescriptorPtr(logicalIdx + k);
        ASFW_LOG(Isoch, "IT: @%u ctl=0x%08x dat=0x%08x br=0x%08x st=0x%08x",
                 logicalIdx + k, b->control, b->dataAddress, b->branchWord, b->statusWord);
    }
#endif
}

void IsochTransmitContext::DumpDescriptorRing(uint32_t startPacket, uint32_t numPackets) const noexcept {
    if (!descRegion_.virtualBase) {
        ASFW_LOG(Isoch, "IT: DumpDescriptorRing - no descriptor ring allocated");
        return;
    }

    constexpr uint32_t totalPackets = kNumPackets;
    if (startPacket >= totalPackets) {
        ASFW_LOG(Isoch, "IT: DumpDescriptorRing - startPacket %u out of range (max=%u)",
                 startPacket, totalPackets - 1);
        return;
    }
    if (startPacket + numPackets > totalPackets) {
        numPackets = totalPackets - startPacket;
    }

    const uint32_t descBaseIOVA = static_cast<uint32_t>(descRegion_.deviceBase);
    const uint32_t bufBaseIOVA = static_cast<uint32_t>(bufRegion_.deviceBase);

    ASFW_LOG(Isoch, "IT: DescRing Dump pkts %u-%u (total=%u pages=%u) DescBase=0x%08x BufBase=0x%08x Z=%u",
             startPacket, startPacket + numPackets - 1, totalPackets, kTotalPages,
             descBaseIOVA, bufBaseIOVA, kBlocksPerPacket);

    for (uint32_t pktIdx = startPacket; pktIdx < startPacket + numPackets; ++pktIdx) {
        uint32_t descBase = pktIdx * kBlocksPerPacket;

        // Use page-aware pointer access
        auto* desc0 = GetDescriptorPtr(descBase);
        auto* immDesc = reinterpret_cast<const OHCIDescriptorImmediate*>(desc0);
        uint32_t ctl0 = desc0->control;
        uint32_t i0 = (ctl0 >> 18) & 0x3;
        uint32_t b0 = (ctl0 >> 16) & 0x3;

        uint32_t skipAddr = immDesc->common.branchWord & 0xFFFFFFF0u;
        uint32_t skipZ = immDesc->common.branchWord & 0xF;
        uint32_t itQ0 = immDesc->immediateData[0];
        uint32_t itQ1 = immDesc->immediateData[1];

        uint32_t spd = (itQ0 >> 16) & 0x7;
        uint32_t tag = (itQ0 >> 14) & 0x3;
        uint32_t chan = (itQ0 >> 8) & 0x3F;
        uint32_t tcode = (itQ0 >> 4) & 0xF;
        uint32_t sy = itQ0 & 0xF;
        uint32_t dataLen = (itQ1 >> 16) & 0xFFFF;

        auto* desc2 = GetDescriptorPtr(descBase + 2);
        uint32_t ctl1 = desc2->control;
        uint32_t i1 = (ctl1 >> 18) & 0x3;
        uint32_t b1 = (ctl1 >> 16) & 0x3;
        uint32_t reqCount1 = ctl1 & 0xFFFF;
        uint32_t branchAddr = desc2->branchWord & 0xFFFFFFF0u;
        uint32_t branchZ = desc2->branchWord & 0xF;
        uint16_t xferStatus = static_cast<uint16_t>(desc2->statusWord >> 16);

        // Also show the computed vs physical IOVA for debugging page gaps
        uint32_t computedIOVA = GetDescriptorIOVA(descBase);
        
        ASFW_LOG(Isoch, "  Pkt[%u] @desc%u IOVA=0x%08x OMI: ctl=0x%08x i=%u b=%u skip=0x%08x|%u Q0=0x%08x(spd=%u tag=%u ch=%u tcode=0x%x sy=%u) Q1=0x%08x(len=%u)",
                 pktIdx, descBase, computedIOVA, ctl0, i0, b0, skipAddr, skipZ,
                 itQ0, spd, tag, chan, tcode, sy,
                 itQ1, dataLen);
        ASFW_LOG(Isoch, "         OL:  ctl=0x%08x i=%u b=%u req=%u data=0x%08x br=0x%08x|%u st=0x%04x",
                 ctl1, i1, b1, reqCount1, desc2->dataAddress, branchAddr, branchZ, xferStatus);
    }
}

} // namespace ASFW::Isoch
