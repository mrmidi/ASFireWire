//
// ASOHCIITManager.cpp
// Isochronous Transmit (IT) manager skeleton
//
// Spec refs (OHCI 1.1): Chapter 6 (IsoXmitIntEvent/Mask demux), §9.2 (context discovery),
//   §9.4 (appending constraints), §9.5 (interrupt causes / cycle inconsistent handling)
//

#include "ASOHCIITManager.hpp"
#include "LogHelper.hpp"

#include <DriverKit/IOReturn.h>

kern_return_t ASOHCIITManager::Initialize(IOPCIDevice* pci,
                                          uint8_t barIndex,
                                          uint32_t poolBytes,
                                          const ITPolicy& defaultPolicy)
{
    if (!pci) return kIOReturnBadArgument;
    _pci = pci;
    _bar = barIndex;
    _defaultPolicy = defaultPolicy;

    // Minimal pool init stub: keep uninitialized until used
    (void)poolBytes;
    _numCtx = 0; // will be probed later
    os_log(ASLog(), "ITManager: Initialize (bar=%u, pool=%u bytes) [stub]", _bar, poolBytes);
    return kIOReturnSuccess;
}

kern_return_t ASOHCIITManager::StartAll()
{
    // TODO: enable isoXmitIntMask per context and start contexts
    os_log(ASLog(), "ITManager: StartAll [stub]");
    return kIOReturnSuccess;
}

kern_return_t ASOHCIITManager::StopAll()
{
    // TODO: disable masks and stop contexts
    os_log(ASLog(), "ITManager: StopAll [stub]");
    return kIOReturnSuccess;
}

kern_return_t ASOHCIITManager::Queue(uint32_t ctxId,
                                     ITSpeed spd,
                                     uint8_t tag,
                                     uint8_t channel,
                                     uint8_t sy,
                                     const uint32_t* payloadPAs,
                                     const uint32_t* payloadSizes,
                                     uint32_t fragments,
                                     const ITQueueOptions& opts)
{
    (void)ctxId; (void)spd; (void)tag; (void)channel; (void)sy; (void)payloadPAs; (void)payloadSizes; (void)fragments; (void)opts;
    // TODO: build program via _builder and enqueue into context
    return kIOReturnUnsupported;
}

void ASOHCIITManager::OnInterrupt_TxEventMask(uint32_t mask)
{
    // TODO: demux per-context bits and call OnInterruptTx()
    (void)mask;
}

void ASOHCIITManager::OnInterrupt_CycleInconsistent()
{
    // TODO: notify cycle-matched contexts
}

uint32_t ASOHCIITManager::ProbeContextCount()
{
    // TODO: read isoXmitIntMaskSet/Clr to discover implemented contexts
    return 0;
}

