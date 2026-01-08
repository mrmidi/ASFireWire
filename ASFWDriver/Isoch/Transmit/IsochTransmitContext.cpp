// IsochTransmitContext.cpp
// ASFW - Isochronous Transmit Context
// !!!!WARNING: DON'T RELY ON OHCI 1.1 SPECS HERE!
// IT SEEMS THAT OHCI 1.2 USED
// ALLWAYS CROSS-VALIDATE WITH LINUX DRIVER
// OR AppleFWOHCI.kext decomp!

#include "IsochTransmitContext.hpp"
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

    return ctx;
}

void IsochTransmitContext::SetSharedTxQueue(void* base, uint64_t bytes) noexcept {
    if (!base || bytes == 0) {
        ASFW_LOG(Isoch, "IT: SetSharedTxQueue - invalid parameters");
        return;
    }

    if (sharedTxQueue_.Attach(base, bytes)) {
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

kern_return_t IsochTransmitContext::Configure(uint8_t channel, uint8_t sid) noexcept {
    if (state_ != State::Unconfigured && state_ != State::Stopped) {
        return kIOReturnBusy;
    }
    
    channel_ = channel;
    assembler_.setSID(sid);
    
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
    
    if (!descriptorRing_.Storage().data()) {
        ASFW_LOG(Isoch, "IT: Cannot start - no DMA ring");
        return kIOReturnNoResources;
    }

    assembler_.reset();
    packetsAssembled_ = 0;
    dataPackets_ = 0;
    noDataPackets_ = 0;
    tickCount_ = 0;
    interruptCount_ = 0;

    if (sharedTxQueue_.IsValid()) {
        uint32_t fillBefore = sharedTxQueue_.FillLevelFrames();
        ASFW_LOG(Isoch, "IT: Pre-prime transfer - shared queue has %u frames", fillBefore);
        
        constexpr uint32_t kTransferChunk = 256;
        int32_t transferBuf[kTransferChunk * 2];
        uint32_t totalTransferred = 0;
        uint32_t chunkCount = 0;
        
        while (sharedTxQueue_.FillLevelFrames() > 0) {
            uint32_t toRead = sharedTxQueue_.FillLevelFrames();
            if (toRead > kTransferChunk) toRead = kTransferChunk;
            
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
            
            if (written < read) break;
        }
        
        ASFW_LOG(Isoch, "IT: Pre-prime transferred %u frames to assembler (fill=%u)",
                 totalTransferred, assembler_.bufferFillLevel());
    }

    memset(descRegion_.virtualBase, 0xDE, kNumDescriptors * sizeof(OHCIDescriptor));
    ASFW_LOG(Isoch, "IT: Pre-filled descriptor ring with 0xDE pattern");

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
        ASFW_LOG(Isoch, "IT: Stopped. Stats: %llu pkts (%lluD/%lluN) IRQs=%llu",
                 packetsAssembled_, dataPackets_, noDataPackets_, interruptCount_);
    }
}

void IsochTransmitContext::Poll() noexcept {
    if (state_ != State::Running) return;
    ++tickCount_;

    if (sharedTxQueue_.IsValid()) {
        constexpr uint32_t kTransferChunk = 256;
        int32_t transferBuf[kTransferChunk * 2];

        uint32_t totalTransferred = 0;
        while (sharedTxQueue_.FillLevelFrames() >= kTransferChunk) {
            uint32_t read = sharedTxQueue_.Read(transferBuf, kTransferChunk);
            if (read == 0) break;

            uint32_t written = assembler_.ringBuffer().write(transferBuf, read);
            totalTransferred += written;

            if (written < read) {
                break;
            }
        }

        uint32_t remaining = sharedTxQueue_.FillLevelFrames();
        if (remaining > 0 && remaining < kTransferChunk) {
            uint32_t read = sharedTxQueue_.Read(transferBuf, remaining);
            if (read > 0) {
                totalTransferred += assembler_.ringBuffer().write(transferBuf, read);
            }
        }

        static uint64_t lastLogTick = 0;
        if (tickCount_ == 1 || (tickCount_ - lastLogTick) >= 1000) {
            ASFW_LOG(Isoch, "IT: Poll tick=%llu txQ=%u asmBuf=%u transferred=%u",
                     tickCount_, sharedTxQueue_.FillLevelFrames(),
                     assembler_.bufferFillLevel(), totalTransferred);
            lastLogTick = tickCount_;
        }
    } else {
        if (tickCount_ == 1 || (tickCount_ % 1000) == 0) {
            ASFW_LOG(Isoch, "IT: Poll tick=%llu fillLevel=%u (no shared queue)",
                     tickCount_, assembler_.bufferFillLevel());
        }
    }
}

void IsochTransmitContext::HandleInterrupt() noexcept {
    if (state_ != State::Running) return;
    ++interruptCount_;

    RefillRing();
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
    if ((descRegion_.deviceBase & 0xFULL) != 0) {
        ASFW_LOG(Isoch, "IT: SetupRings - descriptor base not 16B aligned: 0x%llx",
                 descRegion_.deviceBase);
        return kIOReturnNoResources;
    }

    std::span<OHCIDescriptor> storage(
        reinterpret_cast<OHCIDescriptor*>(descRegion_.virtualBase),
        kRingBlocks
    );
    if (!descriptorRing_.Initialize(storage)) return kIOReturnError;

    memset(descRegion_.virtualBase, 0, kDescriptorRingSize);

    ASFW_LOG(Isoch, "IT: Rings Ready. DescIOVA=0x%llx BufIOVA=0x%llx (blocks=%u packets=%u bpp=%u)",
              descRegion_.deviceBase, bufRegion_.deviceBase, kRingBlocks, kNumPackets, kBlocksPerPacket);

    return kIOReturnSuccess;
}

void IsochTransmitContext::PrimeRing() noexcept {
    const size_t capacity = descriptorRing_.Capacity();
    const size_t numPackets = capacity / kBlocksPerPacket;
    
    auto* descriptors = descriptorRing_.Storage().data();
    
    ASFW_LOG(Isoch, "IT: PrimeRing - capacity=%zu numPackets=%zu", capacity, numPackets);
    
    for (size_t pktIdx = 0; pktIdx < numPackets; ++pktIdx) {
        auto pkt = assembler_.assembleNext(0xFFFF);

        if (pkt.size > kMaxPacketSize) {
            ASFW_LOG(Isoch, "IT: FATAL pkt.size=%u > kMaxPacketSize=%u pktIdx=%zu",
                     pkt.size, kMaxPacketSize, pktIdx);
            return;
        }
        if (pkt.size > 0xFFFFu) {
            ASFW_LOG(Isoch, "IT: FATAL pkt.size=%u exceeds 16-bit dataLength pktIdx=%zu",
                     pkt.size, pktIdx);
            return;
        }

        size_t descBase = pktIdx * kBlocksPerPacket;
        size_t nextPktBase = ((pktIdx + 1) % numPackets) * kBlocksPerPacket;
        
        if (descBase >= kRingBlocks || (descBase + kBlocksPerPacket - 1) >= kRingBlocks) {
            ASFW_LOG(Isoch, "IT: ❌ FATAL: descBase=%zu OUT OF BOUNDS (max=%u) pktIdx=%zu",
                     descBase, kRingBlocks - 1, pktIdx);
            return;
        }
        
        uint32_t nextBlockIOVA = static_cast<uint32_t>(descRegion_.deviceBase) + 
                                 static_cast<uint32_t>(nextPktBase * sizeof(OHCIDescriptor));
        
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
        
        auto* immDesc = reinterpret_cast<OHCIDescriptorImmediate*>(&descriptors[descBase]);

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

        // No need to zero descriptors[descBase + 1] anymore - immediateData is part of immDesc

        auto* lastDesc = &descriptors[descBase + 2];

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
        if (pkt.isData) ++dataPackets_; else ++noDataPackets_;
    }
    
    std::atomic_thread_fence(std::memory_order_release);
    ASFW::Driver::WriteBarrier();
}

void IsochTransmitContext::RefillRing() noexcept {
    if (!hardware_ || state_ != State::Running) return;

    Register32 ctrlReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(contextIndex_));
    uint32_t ctrl = hardware_->Read(ctrlReg);
    bool dead = (ctrl & ContextControl::kDead) != 0;
    if (dead) {
        static bool logged = false;
        if (!logged) {
            ASFW_LOG(Isoch, "IT: RefillRing - context is DEAD, skipping refill");
            logged = true;
        }
        return;
    }

    Register32 cmdPtrReg = static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(contextIndex_));
    uint32_t cmdPtr = hardware_->Read(cmdPtrReg);
    uint32_t cmdAddr = cmdPtr & 0xFFFFFFF0u;

    const uint32_t baseAddr = static_cast<uint32_t>(descRegion_.deviceBase);
    const uint32_t numPackets = kRingBlocks / kBlocksPerPacket;

    if (cmdAddr < baseAddr) return;

    uint32_t descOffset = cmdAddr - baseAddr;
    uint32_t descIndex = descOffset / sizeof(OHCIDescriptor);
    uint32_t hwPacketIndex = descIndex / kBlocksPerPacket;

    if (hwPacketIndex >= numPackets) return;

    static uint32_t refillCallCount = 0;
    if (refillCallCount == 0) {
        ASFW_LOG(Isoch, "IT: RefillRing FIRST CALL - hwPkt=%u swFill=%u cmdPtr=0x%08x",
                 hwPacketIndex, softwareFillIndex_, cmdPtr);
    }
    refillCallCount++;

    uint32_t packetsToRefill = 0;
    if (hwPacketIndex >= softwareFillIndex_) {
        packetsToRefill = hwPacketIndex - softwareFillIndex_;
    } else {
        packetsToRefill = (numPackets - softwareFillIndex_) + hwPacketIndex;
    }

    const uint32_t maxRefill = 16;
    const uint32_t safetyGap = 8;

    if (packetsToRefill > safetyGap) {
        packetsToRefill -= safetyGap;
    } else {
        packetsToRefill = 0;
    }

    if (packetsToRefill > maxRefill) {
        packetsToRefill = maxRefill;
    }

    if (packetsToRefill == 0) return;

    static uint32_t actualRefillCount = 0;
    if (actualRefillCount == 0) {
        ASFW_LOG(Isoch, "IT: RefillRing REFILLING %u packets (hw=%u sw=%u)",
                 packetsToRefill, hwPacketIndex, softwareFillIndex_);
    }
    actualRefillCount++;

    auto* descriptors = descriptorRing_.Storage().data();

    for (uint32_t i = 0; i < packetsToRefill; ++i) {
        uint32_t pktIdx = (softwareFillIndex_ + i) % numPackets;

        auto pkt = assembler_.assembleNext(0xFFFF);

        if (pkt.size > kMaxPacketSize || pkt.size > 0xFFFFu) {
            ASFW_LOG(Isoch, "IT: RefillRing FATAL pkt.size=%u pktIdx=%u", pkt.size, pktIdx);
            return;
        }

        size_t descBase = pktIdx * kBlocksPerPacket;

        if (descBase >= kRingBlocks || (descBase + kBlocksPerPacket - 1) >= kRingBlocks) {
            ASFW_LOG(Isoch, "IT: ❌ RefillRing FATAL: descBase=%zu OUT OF BOUNDS pktIdx=%u",
                     descBase, pktIdx);
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

        auto* lastDesc = &descriptors[descBase + 2];
        uint16_t lastReqCount = static_cast<uint16_t>(pkt.size);

        uint32_t existingControl = lastDesc->control & 0xFFFF0000u;
        lastDesc->control = existingControl | lastReqCount;
        lastDesc->dataAddress = payloadIOVA;
        lastDesc->statusWord = 0;

        auto* immDesc = reinterpret_cast<OHCIDescriptorImmediate*>(&descriptors[descBase]);
        uint32_t isochHeaderQ1 = (static_cast<uint32_t>(pkt.size) & 0xFFFFu) << 16;
        immDesc->immediateData[1] = isochHeaderQ1;
        // TODO: On bus reset recovery, rebuild Q0/Q1 and control fields if tag/channel/speed can change.

        ++packetsAssembled_;
        if (pkt.isData) ++dataPackets_; else ++noDataPackets_;
    }

    std::atomic_thread_fence(std::memory_order_release);
    ASFW::Driver::WriteBarrier();

    softwareFillIndex_ = (softwareFillIndex_ + packetsToRefill) % numPackets;
    lastHwPacketIndex_ = hwPacketIndex;
}
    
void IsochTransmitContext::LogStatistics() const noexcept {
#ifndef ASFW_HOST_TEST
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
            auto* descs = descriptorRing_.Storage().data();
            ASFW_LOG(Isoch, "  Block0[0]: ctl=0x%08x dat=0x%08x br=0x%08x st=0x%08x",
                     descs[0].control, descs[0].dataAddress, descs[0].branchWord, descs[0].statusWord);
            ASFW_LOG(Isoch, "  Block0[1]: ctl=0x%08x dat=0x%08x br=0x%08x st=0x%08x",
                     descs[1].control, descs[1].dataAddress, descs[1].branchWord, descs[1].statusWord);
            ASFW_LOG(Isoch, "  Block0[2]: ctl=0x%08x dat=0x%08x br=0x%08x st=0x%08x",
                     descs[2].control, descs[2].dataAddress, descs[2].branchWord, descs[2].statusWord);
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

    const uint32_t base = static_cast<uint32_t>(descRegion_.deviceBase);
    const uint32_t end = base + static_cast<uint32_t>(kRingBlocks * sizeof(OHCIDescriptor));

    Register32 cmdPtrReg = static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(contextIndex_));
    const uint32_t cmdPtr = hardware_->Read(cmdPtrReg);
    const uint32_t addr = cmdPtr & 0xFFFFFFF0u;
    const uint32_t z = cmdPtr & 0xF;

    ASFW_LOG(Isoch, "IT: DumpAtCmdPtr: cmdPtr=0x%08x addr=0x%08x Z=%u (base=0x%08x end=0x%08x)",
             cmdPtr, addr, z, base, end);

    if (addr < base || addr >= end) {
        ASFW_LOG(Isoch, "IT: CmdPtr OUT OF RING RANGE!");
        return;
    }

    const size_t idx = (addr - base) / sizeof(OHCIDescriptor);
    auto* d = reinterpret_cast<OHCIDescriptor*>(descRegion_.virtualBase);

    for (size_t k = 0; k < 4 && (idx + k) < kRingBlocks; ++k) {
        const auto& b = d[idx + k];
        ASFW_LOG(Isoch, "IT: @%zu ctl=0x%08x dat=0x%08x br=0x%08x st=0x%08x",
                 idx + k, b.control, b.dataAddress, b.branchWord, b.statusWord);
    }
#endif
}

void IsochTransmitContext::DumpDescriptorRing(uint32_t startPacket, uint32_t numPackets) const noexcept {
    if (!descRegion_.virtualBase) {
        ASFW_LOG(Isoch, "IT: DumpDescriptorRing - no descriptor ring allocated");
        return;
    }

    const uint32_t totalPackets = kRingBlocks / kBlocksPerPacket;
    if (startPacket >= totalPackets) {
        ASFW_LOG(Isoch, "IT: DumpDescriptorRing - startPacket %u out of range (max=%u)",
                 startPacket, totalPackets - 1);
        return;
    }
    if (startPacket + numPackets > totalPackets) {
        numPackets = totalPackets - startPacket;
    }

    auto* descriptors = reinterpret_cast<OHCIDescriptor*>(descRegion_.virtualBase);
    const uint32_t descBaseIOVA = static_cast<uint32_t>(descRegion_.deviceBase);
    const uint32_t bufBaseIOVA = static_cast<uint32_t>(bufRegion_.deviceBase);

    ASFW_LOG(Isoch, "IT: DescRing Dump pkts %u-%u (total=%u) DescBase=0x%08x BufBase=0x%08x Z=%u",
             startPacket, startPacket + numPackets - 1, totalPackets,
             descBaseIOVA, bufBaseIOVA, kBlocksPerPacket);

    for (uint32_t pktIdx = startPacket; pktIdx < startPacket + numPackets; ++pktIdx) {
        size_t descBase = pktIdx * kBlocksPerPacket;

        auto* desc0 = &descriptors[descBase];
        auto* immDesc = reinterpret_cast<OHCIDescriptorImmediate*>(desc0);
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

        auto* desc2 = &descriptors[descBase + 2];
        uint32_t ctl1 = desc2->control;
        uint32_t i1 = (ctl1 >> 18) & 0x3;
        uint32_t b1 = (ctl1 >> 16) & 0x3;
        uint32_t reqCount1 = ctl1 & 0xFFFF;
        uint32_t branchAddr = desc2->branchWord & 0xFFFFFFF0u;
        uint32_t branchZ = desc2->branchWord & 0xF;
        uint16_t xferStatus = static_cast<uint16_t>(desc2->statusWord >> 16);

        ASFW_LOG(Isoch, "  Pkt[%u] OMI: ctl=0x%08x i=%u b=%u skip=0x%08x|%u Q0=0x%08x(spd=%u tag=%u ch=%u tcode=0x%x sy=%u) Q1=0x%08x(len=%u)",
                 pktIdx, ctl0, i0, b0, skipAddr, skipZ,
                 itQ0, spd, tag, chan, tcode, sy,
                 itQ1, dataLen);
        ASFW_LOG(Isoch, "         OL:  ctl=0x%08x i=%u b=%u req=%u data=0x%08x br=0x%08x|%u st=0x%04x",
                 ctl1, i1, b1, reqCount1, desc2->dataAddress, branchAddr, branchZ, xferStatus);
    }
}

} // namespace ASFW::Isoch
