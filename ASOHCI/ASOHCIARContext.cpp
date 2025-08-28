//
// ASOHCIARContext.cpp
// ASOHCI
//
// AR context wrapper on ASOHCIContextBase with RAII and ARDescriptorRing.
//

#include "ASOHCIARContext.hpp"
#include "ASOHCIARDescriptorRing.hpp"
#include "Shared/ASOHCIContextBase.hpp"
#include "OHCIConstants.hpp"
#include "LogHelper.hpp"

#include <DriverKit/IOLib.h>

ASOHCIARContext::ASOHCIARContext() = default;
ASOHCIARContext::~ASOHCIARContext() = default;

kern_return_t ASOHCIARContext::Initialize(IOPCIDevice* pci,
                                          uint8_t barIndex,
                                          ARContextRole role,
                                          const ASContextOffsets& offsets,
                                          ARBufferFillMode fillMode)
{
    if (!pci) return kIOReturnBadArgument;
    _role = role;
    _fill = fillMode;

    ASContextKind kind = (role == ARContextRole::kRequest)
                            ? ASContextKind::kAR_Request
                            : ASContextKind::kAR_Response;
    return ASOHCIContextBase::Initialize(pci, barIndex, kind, offsets);
}

kern_return_t ASOHCIARContext::AttachRing(ASOHCIARDescriptorRing* ring)
{
    _ring = ring;
    return (_ring) ? kIOReturnSuccess : kIOReturnBadArgument;
}

kern_return_t ASOHCIARContext::Start()
{
    if (!_ring) return kIOReturnNotReady;
    uint32_t addr = 0; uint8_t z = 0;
    kern_return_t kr = _ring->GetCommandPtrSeed(&addr, &z);
    if (kr != kIOReturnSuccess) return kr;
    kr = WriteCommandPtr(addr, z);
    if (kr != kIOReturnSuccess) return kr;
    WriteContextSet(kOHCI_ContextControl_run);
    os_log(ASLog(), "ARContext: started (addr=0x%x Z=%u)", addr, z);
    return kIOReturnSuccess;
}

kern_return_t ASOHCIARContext::Stop()
{
    return ASOHCIContextBase::Stop();
}

void ASOHCIARContext::OnPacketArrived()
{
    // Lightweight nudge; consumers should pull via TryDequeue
    WriteContextSet(kOHCI_ContextControl_wake);
}

void ASOHCIARContext::OnBufferComplete()
{
    // Same as packet arrived for now; policy can diverge later
    WriteContextSet(kOHCI_ContextControl_wake);
}

bool ASOHCIARContext::TryDequeue(ARPacketView* outView, uint32_t* outRingIndex)
{
    if (!_ring) return false;
    return _ring->TryPopCompleted(outView, outRingIndex);
}

kern_return_t ASOHCIARContext::Recycle(uint32_t ringIndex)
{
    if (!_ring) return kIOReturnNotReady;
    kern_return_t kr = _ring->Recycle(ringIndex);
    if (kr == kIOReturnSuccess) WriteContextSet(kOHCI_ContextControl_wake);
    return kr;
}

void ASOHCIARContext::SetStatusHelper(ASOHCIARStatus* status)
{
    _stat = status;
}

// Reset lifecycle: defer to base; ring is continuous and will be re-armed on Start().
// No overrides needed here.
        os_log(ASLog(), "ASOHCIARContext: ERROR: Failed to clear run bit: 0x%x", result);
        return result;
    }
    
    // Wait for context to become inactive (OHCI 1.1 §3.1.1.1)
    while (retries-- > 0) {
        result = ReadContextControl(&contextControl);
        if (result != kIOReturnSuccess) {
            os_log(ASLog(), "ASOHCIARContext: ERROR: Failed to read context control: 0x%x", result);
            return result;
        }
        
        if (!(contextControl & kOHCI_ContextControl_active)) {
            break;  // Context is now inactive
        }
        
        IOSleep(1);  // Wait 1ms
    }
    
    if (contextControl & kOHCI_ContextControl_active) {
        os_log(ASLog(), "ASOHCIARContext: Context failed to stop (still active): 0x%x", contextControl);
        return kIOReturnTimeout;
    }
    
    fRunning = false;
    
    os_log(ASLog(), "ASOHCIARContext: Stopped %s context", 
                (fContextType == AR_REQUEST_CONTEXT) ? "Request" : "Response");
    
    return kIOReturnSuccess;
}

// Handle context interrupt
kern_return_t ASOHCIARContext::HandleInterrupt()
{
    if (!fInitialized || !fRunning || !fDescriptors) {
        return kIOReturnError;
    }

    bool any = false;
    // Iterate all descriptors, look for completed fills (resCount < reqCount)
    for (uint32_t i = 0; i < fDescriptorCount; ++i) {
        OHCI_ARInputMoreDescriptor* desc = &fDescriptors[i];
        uint32_t req = desc->reqCount;
        uint32_t res = desc->resCount;
        if (req == 0 || req > fBufferSize) req = fBufferSize;
        // If hardware wrote anything (residual decreased), peek header and recycle
        if (res < req) {
            uint32_t received = req - res;
            // Header peek: dump first up to 16 bytes of packet
            if (fBufferMaps && fBufferMaps[i]) {
                uint8_t* buf = (uint8_t*)(uintptr_t)fBufferMaps[i]->GetAddress();
                size_t   blen = (size_t)fBufferMaps[i]->GetLength();
                if (buf && blen >= fBufferSize) {
                    // Build a small hex string without sprintf
                    char line[80]; size_t pos = 0; uint32_t dump = (received < 16) ? received : 16;
                    for (uint32_t b = 0; b < dump && pos + 3 < sizeof(line); ++b) {
                        uint8_t v = buf[b];
                        line[pos++] = ' ';
                        static const char hexd[17] = "0123456789abcdef";
                        line[pos++] = hexd[(v >> 4) & 0xF];
                        line[pos++] = hexd[v & 0xF];
                    }
                    line[pos] = '\0';
                    os_log(ASLog(), "ASOHCIARContext: %s RX[%u] len=%u peek:%{public}s",
                           (fContextType == AR_REQUEST_CONTEXT) ? "ARRQ" : "ARRS",
                           i, received, line);
                }
            }
            // Recycle: reset residual count and status; leave reqCount/dataAddress/branch as-is
            desc->resCount = req;
            desc->xferStatus = 0;
            any = true;
        }
    }
    if (any) {
        // If the context ended (e.g., last descriptor Z=0), re-arm CommandPtr
        uint32_t st = 0;
        if (GetStatus(&st) == kIOReturnSuccess) {
            if ((st & kOHCI_ContextControl_active) == 0) {
                (void)WriteCommandPtr((uint32_t)(fDescriptorSeg.address >> 4), 1);
                (void)WriteContextControl(kOHCI_ContextControl_run, true);
            }
        }
        // Wake the context to continue DMA after recycling
        (void)Wake();
    }
    return kIOReturnSuccess;
}

// Wake the context
kern_return_t ASOHCIARContext::Wake()
{
    if (!fRunning) {
        return kIOReturnError;
    }
    
    return WriteContextControl(kOHCI_ContextControl_wake, true);
}

// Get current context status
kern_return_t ASOHCIARContext::GetStatus(uint32_t* outStatus)
{
    if (!outStatus) {
        return kIOReturnBadArgument;
    }
    
    return ReadContextControl(outStatus);
}

// Check if context is active
bool ASOHCIARContext::IsActive()
{
    uint32_t status = 0;
    if (ReadContextControl(&status) == kIOReturnSuccess) {
        return (status & kOHCI_ContextControl_active) != 0;
    }
    return false;
}

#pragma mark - Private Methods

// Set context register offsets based on type
void ASOHCIARContext::SetContextOffsets(ContextType contextType)
{
    if (contextType == AR_REQUEST_CONTEXT) {
        fContextBaseOffset = kOHCI_AsReqRcvContextBase;
        fContextControlSetOffset = kOHCI_AsReqRcvContextControlS;
        fContextControlClearOffset = kOHCI_AsReqRcvContextControlC;
        fCommandPtrOffset = kOHCI_AsReqRcvCommandPtr;
    } else {
        fContextBaseOffset = kOHCI_AsRspRcvContextBase;
        fContextControlSetOffset = kOHCI_AsRspRcvContextControlS;
        fContextControlClearOffset = kOHCI_AsRspRcvContextControlC;
        fCommandPtrOffset = kOHCI_AsRspRcvCommandPtr;
    }
}

// Allocate receive buffers
kern_return_t ASOHCIARContext::AllocateBuffers()
{
    kern_return_t result;
    
    fBufferDescriptors = new IOBufferMemoryDescriptor*[fBufferCount];
    fBufferMaps = new IOMemoryMap*[fBufferCount];
    fBufferDMA = new IODMACommand*[fBufferCount];
    fBufferSegs = new IOAddressSegment[fBufferCount];
    
    if (!fBufferDescriptors || !fBufferMaps || !fBufferDMA || !fBufferSegs) {
        return kIOReturnNoMemory;
    }
    
    // Initialize arrays
    for (uint32_t i = 0; i < fBufferCount; i++) {
        fBufferDescriptors[i] = nullptr;
        fBufferMaps[i] = nullptr;
        fBufferDMA[i] = nullptr;
        fBufferSegs[i] = {};
    }
    
    // Allocate each buffer
    for (uint32_t i = 0; i < fBufferCount; i++) {
        result = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                                 fBufferSize,
                                                 4, // quadlet alignment
                                                 &fBufferDescriptors[i]);
        if (result != kIOReturnSuccess) {
            os_log(ASLog(), "ASOHCIARContext: ERROR: Failed to create buffer descriptor %u: 0x%x", i, result);
            return result;
        }
        
        result = fBufferDescriptors[i]->CreateMapping(0, 0, 0, 0, 0, &fBufferMaps[i]);
        if (result != kIOReturnSuccess) {
            os_log(ASLog(), "ASOHCIARContext: ERROR: Failed to map buffer %u: 0x%x", i, result);
            return result;
        }

        // Create 32-bit DMA mapping for each buffer
        IODMACommandSpecification spec{};
        spec.options = kIODMACommandSpecificationNoOptions;
        spec.maxAddressBits = 32;
        result = IODMACommand::Create(fPCIDevice, kIODMACommandCreateNoOptions, &spec, &fBufferDMA[i]);
        if (result != kIOReturnSuccess || !fBufferDMA[i]) {
            os_log(ASLog(), "ASOHCIARContext: ERROR: DMA cmd create failed for buffer %u: 0x%x", i, result);
            return (result != kIOReturnSuccess) ? result : kIOReturnNoMemory;
        }
        uint64_t flags = 0; uint32_t segCount = 1; IOAddressSegment segs[1] = {};
        result = fBufferDMA[i]->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                                              fBufferDescriptors[i], 0, fBufferSize,
                                              &flags, &segCount, segs);
        if (result != kIOReturnSuccess || segCount < 1 || segs[0].address == 0) {
            os_log(ASLog(), "ASOHCIARContext: ERROR: DMA map failed for buffer %u: 0x%x segs=%u", i, result, segCount);
            return (result != kIOReturnSuccess) ? result : kIOReturnNoResources;
        }
        fBufferSegs[i] = segs[0];
    }
    
    return kIOReturnSuccess;
}

// Allocate descriptor chain
kern_return_t ASOHCIARContext::AllocateDescriptorChain()
{
    kern_return_t result;
    size_t chainSize = fDescriptorCount * sizeof(OHCI_ARInputMoreDescriptor);
    
    result = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                             chainSize,
                                             kOHCI_DescriptorAlign,
                                             &fDescriptorChain);
    if (result != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCIARContext: ERROR: Failed to create descriptor chain: 0x%x", result);
        return result;
    }
    
    result = fDescriptorChain->CreateMapping(0, 0, 0, 0, 0, &fDescriptorMap);
    if (result != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCIARContext: ERROR: Failed to map descriptor chain: 0x%x", result);
        return result;
    }
    
    fDescriptors = (OHCI_ARInputMoreDescriptor*)fDescriptorMap->GetAddress();
    if (!fDescriptors) {
        os_log(ASLog(), "ASOHCIARContext: ERROR: Failed to get descriptor chain address");
        return kIOReturnError;
    }
    
    // DMA map the descriptor chain
    IODMACommandSpecification spec{};
    spec.options = kIODMACommandSpecificationNoOptions;
    spec.maxAddressBits = 32;
    result = IODMACommand::Create(fPCIDevice, kIODMACommandCreateNoOptions, &spec, &fDescriptorDMA);
    if (result != kIOReturnSuccess || !fDescriptorDMA) {
        os_log(ASLog(), "ASOHCIARContext: ERROR: Failed to create desc DMA cmd: 0x%x", result);
        return (result != kIOReturnSuccess) ? result : kIOReturnNoMemory;
    }
    uint64_t flags = 0; uint32_t segCount = 1; IOAddressSegment segs[1] = {};
    result = fDescriptorDMA->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                                           fDescriptorChain,
                                           0,
                                           chainSize,
                                           &flags,
                                           &segCount,
                                           segs);
    if (result != kIOReturnSuccess || segCount < 1 || segs[0].address == 0) {
        os_log(ASLog(), "ASOHCIARContext: ERROR: Desc DMA map failed: 0x%x segs=%u", result, segCount);
        return (result != kIOReturnSuccess) ? result : kIOReturnNoResources;
    }
    fDescriptorSeg = segs[0];
    return kIOReturnSuccess;
}

// Setup descriptor chain
kern_return_t ASOHCIARContext::SetupDescriptorChain()
{
    // Initialize each descriptor in the chain
    for (uint32_t i = 0; i < fDescriptorCount; i++) {
        OHCI_ARInputMoreDescriptor* desc = &fDescriptors[i];
        
        // Clear descriptor
        memset(desc, 0, sizeof(*desc));
        
        // Set command fields per OHCI 1.1 §8.1.1
        desc->cmd = 0x2;        // INPUT_MORE command
        desc->key = 0x0;        // Must be 0 for AR
        desc->i = 0x3;          // Generate interrupt on completion
        desc->b = 0x3;          // Branch control (must be 0x3)
        desc->reqCount = fBufferSize;
        
        // Use DMA address for data
        if (fBufferSegs == nullptr || fBufferSegs[i].address == 0) {
            os_log(ASLog(), "ASOHCIARContext: ERROR: Missing DMA address for buffer %u", i);
            return kIOReturnError;
        }
        desc->dataAddress = (uint32_t)fBufferSegs[i].address;
        
        // Set branch address and Z value
        if (i < fDescriptorCount - 1) {
            // Point to next descriptor using DMA base of descriptor chain
            uint64_t nextDescDMA = fDescriptorSeg.address + (uint64_t)(i + 1) * sizeof(*desc);
            desc->branchAddress = (uint32_t)(nextDescDMA >> 4); // upper 28 bits
            desc->Z = 1;  // Next block has 1 descriptor
        } else {
            // Last descriptor - end of chain
            desc->branchAddress = 0;
            desc->Z = 0;  // End of program
        }
        
        // Initialize status fields (will be updated by hardware)
        desc->resCount = fBufferSize;  // Initially no data received
        desc->xferStatus = 0;
    }
    
    return kIOReturnSuccess;
}

// Free buffers
kern_return_t ASOHCIARContext::FreeBuffers()
{
    if (fBufferDMA) {
        for (uint32_t i = 0; i < fBufferCount; i++) {
            if (fBufferDMA[i]) {
                fBufferDMA[i]->CompleteDMA(kIODMACommandCompleteDMANoOptions);
                fBufferDMA[i]->release();
            }
        }
        delete[] fBufferDMA;
        fBufferDMA = nullptr;
    }
    if (fBufferMaps) {
        for (uint32_t i = 0; i < fBufferCount; i++) {
            if (fBufferMaps[i]) {
                fBufferMaps[i]->release();
            }
        }
        delete[] fBufferMaps;
        fBufferMaps = nullptr;
    }
    
    if (fBufferDescriptors) {
        for (uint32_t i = 0; i < fBufferCount; i++) {
            if (fBufferDescriptors[i]) {
                fBufferDescriptors[i]->release();
            }
        }
        delete[] fBufferDescriptors;
        fBufferDescriptors = nullptr;
    }
    if (fBufferSegs) {
        delete[] fBufferSegs;
        fBufferSegs = nullptr;
    }
    
    return kIOReturnSuccess;
}

// Free descriptor chain
kern_return_t ASOHCIARContext::FreeDescriptorChain()
{
    if (fDescriptorDMA) {
        fDescriptorDMA->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        fDescriptorDMA->release();
        fDescriptorDMA = nullptr;
        fDescriptorSeg = {};
    }
    if (fDescriptorMap) {
        fDescriptorMap->release();
        fDescriptorMap = nullptr;
    }
    
    if (fDescriptorChain) {
        fDescriptorChain->release();
        fDescriptorChain = nullptr;
    }
    
    fDescriptors = nullptr;
    return kIOReturnSuccess;
}

// Write to context control register
kern_return_t ASOHCIARContext::WriteContextControl(uint32_t value, bool setRegister)
{
    uint32_t offset = setRegister ? fContextControlSetOffset : fContextControlClearOffset;
    fPCIDevice->MemoryWrite32(fBARIndex, offset, value);
    return kIOReturnSuccess;
}

// Read context control register
kern_return_t ASOHCIARContext::ReadContextControl(uint32_t* value)
{
    if (!value) {
        return kIOReturnBadArgument;
    }
    fPCIDevice->MemoryRead32(fBARIndex, fContextControlSetOffset, value);
    return kIOReturnSuccess;
}

// Write command pointer register
kern_return_t ASOHCIARContext::WriteCommandPtr(uint32_t descriptorAddress, uint32_t zValue)
{
    uint32_t commandPtr = (descriptorAddress << 4) | (zValue & 0xF);
    fPCIDevice->MemoryWrite32(fBARIndex, fCommandPtrOffset, commandPtr);
    return kIOReturnSuccess;
}
