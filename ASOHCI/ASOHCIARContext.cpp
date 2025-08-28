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

// Back-compat: build and own a simple ring with shared policy
kern_return_t ASOHCIARContext::Initialize(IOPCIDevice* pci,
                                          ContextType contextType,
                                          uint8_t barIndex,
                                          uint32_t bufferCount,
                                          uint32_t bufferBytes)
{
    ARContextRole role = (contextType == AR_REQUEST_CONTEXT) ? ARContextRole::kRequest : ARContextRole::kResponse;
    ASContextOffsets offs = {};
    if (role == ARContextRole::kRequest) {
        offs.contextBase        = kOHCI_AsReqRcvContextBase;
        offs.contextControlSet  = kOHCI_AsReqRcvContextControlS;
        offs.contextControlClear= kOHCI_AsReqRcvContextControlC;
        offs.commandPtr         = kOHCI_AsReqRcvCommandPtr;
    } else {
        offs.contextBase        = kOHCI_AsRspRcvContextBase;
        offs.contextControlSet  = kOHCI_AsRspRcvContextControlS;
        offs.contextControlClear= kOHCI_AsRspRcvContextControlC;
        offs.commandPtr         = kOHCI_AsRspRcvCommandPtr;
    }
    kern_return_t kr = Initialize(pci, barIndex, role, offs, ARBufferFillMode::kImmediate);
    if (kr != kIOReturnSuccess) return kr;
    // Create a private ring if none attached yet
    if (!_ring) {
        ASOHCIARDescriptorRing* r = new ASOHCIARDescriptorRing();
        if (!r) return kIOReturnNoMemory;
        kr = r->Initialize(pci, bufferCount, bufferBytes, ARBufferFillMode::kImmediate);
        if (kr != kIOReturnSuccess) { delete r; return kr; }
        _ring = r;
    }
    return kIOReturnSuccess;
}

kern_return_t ASOHCIARContext::Initialize(IOPCIDevice* pci,
                                          ContextType contextType,
                                          uint8_t barIndex)
{
    // Default policy: modest ring suitable for bring-up
    const uint32_t kDefaultBufCount = 16;
    const uint32_t kDefaultBufBytes = 2048;
    return Initialize(pci, contextType, barIndex, kDefaultBufCount, kDefaultBufBytes);
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

kern_return_t ASOHCIARContext::HandleInterrupt()
{
    // For now, just wake the context; consumers will pull via TryDequeue
    WriteContextSet(kOHCI_ContextControl_wake);
    return kIOReturnSuccess;
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
