#include "IsochReceiveContext.hpp"
#include <new>
#include "../Hardware/OHCIConstants.hpp"
#include "../Hardware/RegisterMap.hpp"
#include "../Shared/Rings/RingHelpers.hpp"
#include "../Diagnostics/Signposts.hpp"

#include "../Hardware/OHCIDescriptors.hpp"
#include "../Hardware/HWNamespaceAlias.hpp"

// Verify Descriptor Layout (Expert Advice: Item C)
static_assert(sizeof(::ASFW::Async::HW::OHCIDescriptor) == 16, "OHCIDescriptor must be 16 bytes");
static_assert(alignof(::ASFW::Async::HW::OHCIDescriptor) >= 16, "OHCIDescriptor alignment must be >= 16");

namespace ASFW::Isoch {

namespace HW = ::ASFW::Async::HW;

// ============================================================================
// Factory
// ============================================================================

OSSharedPtr<IsochReceiveContext> IsochReceiveContext::Create(::ASFW::Driver::HardwareInterface* hw, 
                                                            std::shared_ptr<::ASFW::Isoch::Memory::IIsochDMAMemory> dmaMemory) {
    IsochReceiveContext* rawCtx = new (std::nothrow) IsochReceiveContext();
    if (!rawCtx) return nullptr; // Handle allocation failure

    rawCtx->hardware_ = hw;
    rawCtx->dmaMemory_ = std::move(dmaMemory);
    
    if (!rawCtx->init()) {
        rawCtx->release(); // Release on init failure
        return nullptr;
    }
    
    return OSSharedPtr<IsochReceiveContext>(rawCtx, OSNoRetain);
}

// ============================================================================
// Lifecycle
// ============================================================================

bool IsochReceiveContext::init() {
    if (!OSObject::init()) {
        return false;
    }
    
    // Initialize base DmaContextManagerBase state
    return true;
}

void IsochReceiveContext::free() {
    Stop();
    OSObject::free();
}

// ============================================================================
// Configuration
// ============================================================================

IsochReceiveContext::Registers IsochReceiveContext::GetRegisters(uint8_t index) const {
    return Registers{
        .CommandPtr         = static_cast<::ASFW::Driver::Register32>(::DMAContextHelpers::IsoRcvCommandPtr(index)),
        .ContextControlSet  = static_cast<::ASFW::Driver::Register32>(::DMAContextHelpers::IsoRcvContextControlSet(index)),
        .ContextControlClear= static_cast<::ASFW::Driver::Register32>(::DMAContextHelpers::IsoRcvContextControlClear(index)),
        .ContextMatch       = static_cast<::ASFW::Driver::Register32>(::DMAContextHelpers::IsoRcvContextMatch(index))
    };
}

kern_return_t IsochReceiveContext::Configure(uint8_t channel, uint8_t contextIndex) {
    if (!hardware_ || !dmaMemory_) {
        return kIOReturnNotReady;
    }
    
    if (contextIndex >= 4) { // Typical OHCI has 4 IR contexts
        return kIOReturnBadArgument;
    }
    
    contextIndex_ = contextIndex;
    channel_ = channel;
    registers_ = GetRegisters(contextIndex_);
    
    return SetupRings();
}

kern_return_t IsochReceiveContext::SetupRings() {
    // 1. Allocate Rings
    size_t descriptorsSize = kNumDescriptors * sizeof(::ASFW::Async::HW::OHCIDescriptor);
    size_t buffersSize = kNumDescriptors * kMaxPacketSize;
    
    using namespace ASFW::Async;
    
    auto descRegion = dmaMemory_->AllocateDescriptor(descriptorsSize);
    if (!descRegion) {
        return kIOReturnNoMemory;
    }
    
    auto bufRegion = dmaMemory_->AllocatePayloadBuffer(buffersSize);
    if (!bufRegion) {
        return kIOReturnNoMemory;
    }
    
    auto descSpan = std::span<HW::OHCIDescriptor>(reinterpret_cast<HW::OHCIDescriptor*>(descRegion->virtualBase), kNumDescriptors);
    auto bufSpan = std::span<uint8_t>(bufRegion->virtualBase, buffersSize);
    
    bool ok = bufferRing_.Initialize(descSpan, bufSpan, kNumDescriptors, kMaxPacketSize);
    if (!ok) {
        return kIOReturnInternalError;
    }

    bufferRing_.BindDma(dmaMemory_.get());
    
    if (!bufferRing_.Finalize(descRegion->deviceBase, bufRegion->deviceBase)) {
        return kIOReturnInternalError;
    }

    // 2. Program Initial Descriptors
    auto count = bufferRing_.Capacity();
    for (uint32_t i = 0; i < count; ++i) {
        auto* desc = bufferRing_.GetDescriptor(i);
        
        constexpr uint16_t kReqCount = static_cast<uint16_t>(kMaxPacketSize);
        const uint8_t interruptBits = (i % 8 == 7) ? HW::OHCIDescriptor::kIntAlways
                                                   : HW::OHCIDescriptor::kIntNever;
        uint32_t control = HW::OHCIDescriptor::BuildControl(
            kReqCount,
            HW::OHCIDescriptor::kCmdInputLast,
            HW::OHCIDescriptor::kKeyStandard,
            interruptBits,
            HW::OHCIDescriptor::kBranchAlways);
        control |= (1u << (HW::OHCIDescriptor::kStatusShift + HW::OHCIDescriptor::kControlHighShift));
        desc->control = control;
        
        const uint64_t dataIOVA = bufferRing_.GetElementIOVA(i);
        if (dataIOVA == 0 || dataIOVA > 0xFFFFFFFFULL) {
            return kIOReturnInternalError;
        }
        desc->dataAddress = static_cast<uint32_t>(dataIOVA);
        
        const uint64_t nextIOVA = bufferRing_.GetDescriptorIOVA((i + 1) % count);
        if (nextIOVA == 0 || nextIOVA > 0xFFFFFFFFULL || (nextIOVA & 0xF) != 0) {
            return kIOReturnInternalError;
        }
        uint32_t nextDescIOVA = static_cast<uint32_t>(nextIOVA);
        
        desc->branchWord = HW::MakeBranchWordAR(nextDescIOVA, 1);
        
        HW::AR_init_status(*desc, kReqCount);
    }
    
    bufferRing_.PublishAllDescriptorsOnce();
    
    return kIOReturnSuccess;
}


// ============================================================================
// Runtime
// ============================================================================

kern_return_t IsochReceiveContext::Start() {
    if (GetState() != IRPolicy::State::Stopped) {
        return kIOReturnInvalid;
    }
    
    if (!hardware_) {
        ASFW_LOG(Isoch, "❌ Start: hardware_ is null!");
        return kIOReturnNotReady;
    }
    
    uint32_t contextMatch = 0xF0000000 | (channel_ & 0x3F);
    hardware_->Write(registers_.ContextMatch, contextMatch);
    
    uint64_t descIOVA = bufferRing_.GetDescriptorIOVA(0);
    if (descIOVA == 0 || descIOVA > 0xFFFFFFFFULL) {
        ASFW_LOG(Isoch, "❌ Start: Invalid descriptor IOVA 0x%llx", descIOVA);
        return kIOReturnInternalError;
    }
    uint32_t cmdPtr = static_cast<uint32_t>(descIOVA) | 1; // Z=1 (fetch 1 descriptor)
    hardware_->Write(registers_.CommandPtr, cmdPtr);
    
    hardware_->Write(registers_.ContextControlClear, 0xFFFFFFFFu);
    uint32_t ctlValue = ContextControl::kRun | ContextControl::kIsochHeader;
    hardware_->Write(registers_.ContextControlSet, ctlValue);
    
    uint32_t contextMask = 1u << contextIndex_;
    hardware_->Write(ASFW::Driver::Register32::kIsoRecvIntMaskSet, contextMask);
    ASFW_LOG(Isoch, "Start: Enabled IR interrupt for context %u (mask=0x%08x)", contextIndex_, contextMask);
    
    uint32_t readMatch = hardware_->Read(registers_.ContextMatch);
    uint32_t readCmd = hardware_->Read(registers_.CommandPtr);
    uint32_t readCtl = hardware_->Read(registers_.ContextControlSet);
    
    ASFW_LOG(Isoch, "Start: Wrote Match=0x%08x Cmd=0x%08x Ctl=0x%08x", contextMatch, cmdPtr, ctlValue);
    ASFW_LOG(Isoch, "Start: Readback Match=0x%08x Cmd=0x%08x Ctl=0x%08x", readMatch, readCmd, readCtl);
    
    bool runSet = (readCtl & ContextControl::kRun) != 0;
    bool activeSet = (readCtl & ContextControl::kActive) != 0;
    bool deadSet = (readCtl & ContextControl::kDead) != 0;
    
    ASFW_LOG(Isoch, "Start: Context state: run=%d active=%d dead=%d", runSet, activeSet, deadSet);
    
    if (deadSet) {
        ASFW_LOG(Isoch, "❌ Start: Context is DEAD! Check descriptor program.");
        return kIOReturnNotPermitted;
    }
    
    while (lock_.test_and_set(std::memory_order_acquire)) {
    }
    
    Transition(IRPolicy::State::Running, 0, "Start");
    
    streamProcessor_.Reset();
    
    lock_.clear(std::memory_order_release);
    
    return kIOReturnSuccess;
}


void IsochReceiveContext::Stop() {
    while (lock_.test_and_set(std::memory_order_acquire)) {
    }

    if (GetState() == IRPolicy::State::Stopped) {
        lock_.clear(std::memory_order_release);
        return;
    }
    
    hardware_->Write(registers_.ContextControlClear, ContextControl::kRun);
    
    uint32_t contextMask = 1u << contextIndex_;
    hardware_->Write(ASFW::Driver::Register32::kIsoRecvIntMaskClear, contextMask);
    ASFW_LOG(Isoch, "Stop: Disabled IR interrupt for context %u", contextIndex_);
    
    Transition(IRPolicy::State::Stopped, 0, "Stop");
    
    streamProcessor_.LogStatistics();
    
    lock_.clear(std::memory_order_release);
}

uint32_t IsochReceiveContext::Poll() {
    if (lock_.test_and_set(std::memory_order_acquire)) {
        return 0;
    }

    if (GetState() != IRPolicy::State::Running) {
        lock_.clear(std::memory_order_release);
        return 0;
    }

    uint32_t processed = 0;
    
    uint64_t start = mach_absolute_time();
    
    constexpr bool kNullProcessing = true;
    auto capacity = bufferRing_.Capacity();
    uint32_t idx = lastProcessedIndex_;

    for (uint32_t scanned = 0; scanned < capacity; ++scanned) {
        auto* desc = bufferRing_.GetDescriptor(idx);
        
        dmaMemory_->FetchFromDevice(desc, sizeof(*desc));
        
        uint16_t xferStatus = HW::AR_xferStatus(*desc);
        uint16_t resCount = HW::AR_resCount(*desc);
        uint32_t rawStatus = desc->statusWord;
        
        if (rawStatus != 0 || xferStatus != 0 || resCount != kMaxPacketSize) {
        }

        constexpr uint16_t reqCount = static_cast<uint16_t>(kMaxPacketSize);
        bool done = (xferStatus != 0) || (resCount != reqCount);

        if (done) {
             uint16_t actualLength = (resCount <= reqCount) ? (reqCount - resCount) : 0;
             
            if constexpr (kNullProcessing) {
                streamProcessor_.RecordRawPacket(actualLength);
            } else {
                auto* va = bufferRing_.GetElementVA(idx);

                dmaMemory_->FetchFromDevice(va, actualLength);

                streamProcessor_.ProcessPacket(reinterpret_cast<const uint8_t*>(va), actualLength);

                if (callback_) {
                    auto span = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(va), actualLength);
                    callback_(span, static_cast<uint32_t>(xferStatus), 0);
                }
            }
            
             HW::AR_init_status(*desc, reqCount);
             dmaMemory_->PublishToDevice(desc, sizeof(*desc));
             
             idx = (idx + 1) % capacity;
             lastProcessedIndex_ = idx;
             processed++;
        } else {
            break; 
        }
    }
    
    if (processed > 0) {
        ::ASFW::Driver::WriteBarrier();
    
        uint64_t end = mach_absolute_time();
        uint64_t deltaTicks = end - start;
        uint64_t deltaUs = ASFW::Diagnostics::MachTicksToMicroseconds(deltaTicks);
        streamProcessor_.RecordPollLatency(deltaUs, processed);
    }
    
    lock_.clear(std::memory_order_release);
    return processed;
}

void IsochReceiveContext::SetCallback(IsochReceiveCallback callback) {
    callback_ = callback;
}

void IsochReceiveContext::LogHardwareState() {
    if (!hardware_) {
        ASFW_LOG(Isoch, "LogHardwareState: hardware_=null, skipping");
        return;
    }
    if (GetState() != IRPolicy::State::Running) {
        return;
    }
    
    uint32_t cmdPtr = hardware_->Read(registers_.CommandPtr);
    uint32_t ctl = hardware_->Read(registers_.ContextControlSet);
    uint32_t match = hardware_->Read(registers_.ContextMatch);
    
    bool runSet = (ctl & ContextControl::kRun) != 0;
    bool activeSet = (ctl & ContextControl::kActive) != 0;
    bool deadSet = (ctl & ContextControl::kDead) != 0;
    uint32_t eventCode = (ctl & ContextControl::kEventCodeMask) >> ContextControl::kEventCodeShift;
    
    ASFW_LOG(Isoch, "IR: run=%d active=%d dead=%d evt=0x%02x lastIdx=%u cap=%u",
             runSet, activeSet, deadSet, eventCode,
             lastProcessedIndex_, static_cast<uint32_t>(bufferRing_.Capacity()));
    
    ASFW_LOG_V3(Isoch, "=== IR HW State ===");
    ASFW_LOG_V3(Isoch, "Registers: CmdPtr=0x%08x Ctl=0x%08x Match=0x%08x", cmdPtr, ctl, match);
    
    constexpr uint32_t kDumpCount = 8;
    uint32_t capacity = static_cast<uint32_t>(bufferRing_.Capacity());
    uint32_t dumpCount = std::min(kDumpCount, capacity);
    
    ASFW_LOG_V3(Isoch, "Descriptor Ring (first %u):", dumpCount);
    for (uint32_t i = 0; i < dumpCount; ++i) {
        auto* desc = bufferRing_.GetDescriptor(i);
        dmaMemory_->FetchFromDevice(desc, sizeof(*desc));
        
        uint16_t xferStatus = HW::AR_xferStatus(*desc);
        uint16_t resCount = HW::AR_resCount(*desc);
        uint16_t reqCount = static_cast<uint16_t>(kMaxPacketSize);
        bool done = (xferStatus != 0) || (resCount != reqCount);
        uint16_t bytesReceived = (resCount <= reqCount) ? (reqCount - resCount) : 0;
        
        ASFW_LOG_V3(Isoch, "  [%u] ctl=0x%08x data=0x%08x br=0x%08x stat=0x%08x | xfer=0x%04x res=%u %{public}s recv=%u",
                 i, desc->control, desc->dataAddress, desc->branchWord, desc->statusWord,
                 xferStatus, resCount, done ? "DONE" : "PEND", bytesReceived);
        
        if (done && bytesReceived > 0) {
            auto* payloadVA = reinterpret_cast<uint8_t*>(bufferRing_.GetElementVA(i));
            dmaMemory_->FetchFromDevice(payloadVA, std::min(bytesReceived, static_cast<uint16_t>(32)));
            
            ASFW_LOG_V3(Isoch, "      Payload[0-15]: %02x %02x %02x %02x  %02x %02x %02x %02x  %02x %02x %02x %02x  %02x %02x %02x %02x",
                     payloadVA[0], payloadVA[1], payloadVA[2], payloadVA[3],
                     payloadVA[4], payloadVA[5], payloadVA[6], payloadVA[7],
                     payloadVA[8], payloadVA[9], payloadVA[10], payloadVA[11],
                     payloadVA[12], payloadVA[13], payloadVA[14], payloadVA[15]);
            
            if (bytesReceived > 16) {
                ASFW_LOG_V3(Isoch, "      Payload[16-31]: %02x %02x %02x %02x  %02x %02x %02x %02x  %02x %02x %02x %02x  %02x %02x %02x %02x",
                         payloadVA[16], payloadVA[17], payloadVA[18], payloadVA[19],
                         payloadVA[20], payloadVA[21], payloadVA[22], payloadVA[23],
                         payloadVA[24], payloadVA[25], payloadVA[26], payloadVA[27],
                         payloadVA[28], payloadVA[29], payloadVA[30], payloadVA[31]);
            }
        }
    }
    
    ASFW_LOG_V3(Isoch, "===================");
}

} // namespace ASFW::Isoch
