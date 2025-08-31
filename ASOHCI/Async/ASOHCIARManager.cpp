//
// ASOHCIARManager.cpp
// ASOHCI
//
// Asynchronous Receive (AR) Manager (RAII)
//
// Responsibilities:
//  - Own and orchestrate both AR contexts (Request and Response)
//  - Centralize buffer policy (sizes/count)
//  - Fan-out interrupts and bus-reset signals
//  - Optional: expose a packet callback or pull APIs
//
// Spec refs (OHCI 1.1): §8.1/§8.2/§8.4/§8.6
//

#include "ASOHCIARManager.hpp"
#include "../Shared/ASOHCIContextBase.hpp"
#include "../Shared/ASOHCITypes.hpp"
#include "ASOHCIARContext.hpp"
#include "ASOHCIARDescriptorRing.hpp"
#include "ASOHCIARParser.hpp"
#include "ASOHCIARStatus.hpp"
#include "LogHelper.hpp"
#include "OHCIConstants.hpp"

#include <DriverKit/IOLib.h>

namespace {
static inline ASContextOffsets OffsetsFor(ARContextRole role) {
  ASContextOffsets o{};
  if (role == ARContextRole::kRequest) {
    o.contextBase = kOHCI_AsReqRcvContextBase;
    o.contextControlSet = kOHCI_AsReqRcvContextControlS;
    o.contextControlClear = kOHCI_AsReqRcvContextControlC;
    o.commandPtr = kOHCI_AsReqRcvCommandPtr;
  } else {
    o.contextBase = kOHCI_AsRspRcvContextBase;
    o.contextControlSet = kOHCI_AsRspRcvContextControlS;
    o.contextControlClear = kOHCI_AsRspRcvContextControlC;
    o.commandPtr = kOHCI_AsRspRcvCommandPtr;
  }
  return o;
}
} // namespace

// RAII manager
// (use implicit defaults from header; ensure Stop on dtor if needed by owner)

kern_return_t
ASOHCIARManager::Initialize(OSSharedPtr<IOPCIDevice> pci, uint8_t barIndex,
                            uint32_t bufferCount, uint32_t bufferBytes,
                            ARBufferFillMode fillMode,
                            const ARFilterOptions & /*filterOpts*/) {
  if (!pci)
    return kIOReturnBadArgument;
  if (bufferCount < 2U || bufferCount > 64U)
    return kIOReturnBadArgument;
  if (bufferBytes < 512U || bufferBytes > (256U * 1024U) ||
      (bufferBytes & 0x3U))
    return kIOReturnBadArgument;

  _pci = pci;
  _bar = barIndex;

  // Create contexts with OSSharedPtr
  _arReq = OSSharedPtr<ASOHCIARContext>(new ASOHCIARContext(), OSNoRetain);
  _arRsp = OSSharedPtr<ASOHCIARContext>(new ASOHCIARContext(), OSNoRetain);
  if (!_arReq || !_arRsp)
    return kIOReturnNoMemory;

  // Create rings with OSSharedPtr
  _ringReq = OSSharedPtr<ASOHCIARDescriptorRing>(new ASOHCIARDescriptorRing(),
                                                 OSNoRetain);
  _ringRsp = OSSharedPtr<ASOHCIARDescriptorRing>(new ASOHCIARDescriptorRing(),
                                                 OSNoRetain);
  if (!_ringReq || !_ringRsp)
    return kIOReturnNoMemory;

  // Initialize rings (shared policy for now)
  kern_return_t kr =
      _ringReq->Initialize(_pci.get(), bufferCount, bufferBytes, fillMode);
  if (kr != kIOReturnSuccess)
    return kr;
  kr = _ringRsp->Initialize(_pci.get(), bufferCount, bufferBytes, fillMode);
  if (kr != kIOReturnSuccess)
    return kr;

  // Initialize contexts with role-specific offsets
  kr = _arReq->Initialize(_pci.get(), barIndex, ARContextRole::kRequest,
                          OffsetsFor(ARContextRole::kRequest), fillMode);
  if (kr != kIOReturnSuccess)
    return kr;
  kr = _arRsp->Initialize(_pci.get(), barIndex, ARContextRole::kResponse,
                          OffsetsFor(ARContextRole::kResponse), fillMode);
  if (kr != kIOReturnSuccess)
    return kr;

  // Attach rings
  (void)_arReq->AttachRing(_ringReq.get());
  (void)_arRsp->AttachRing(_ringRsp.get());

  os_log(ASLog(), "ARManager: initialized (%u buffers × %u bytes, BAR=%u)",
         bufferCount, bufferBytes, _bar);
  return kIOReturnSuccess;
}

kern_return_t ASOHCIARManager::Start() {
  if (!_arReq || !_arRsp)
    return kIOReturnNotReady;
  kern_return_t kr = _arReq->Start();
  if (kr != kIOReturnSuccess)
    return kr;
  kr = _arRsp->Start();
  if (kr != kIOReturnSuccess) {
    (void)_arReq->Stop();
    return kr;
  }
  os_log(ASLog(), "ARManager: both AR contexts started");
  return kIOReturnSuccess;
}

kern_return_t ASOHCIARManager::Stop() {
  kern_return_t r1 = kIOReturnSuccess, r2 = kIOReturnSuccess;
  if (_arReq)
    r1 = _arReq->Stop();
  if (_arRsp)
    r2 = _arRsp->Stop();
  return (r1 != kIOReturnSuccess) ? r1 : r2;
}

void ASOHCIARManager::SetPacketCallback(PacketCallback cb, void *refcon) {
  _cb = cb;
  _cbRefcon = refcon;
}

// IRQ fan-in (router would call these)
void ASOHCIARManager::OnRequestPacketIRQ() {
  if (_arReq)
    _arReq->OnPacketArrived();
}
void ASOHCIARManager::OnResponsePacketIRQ() {
  if (_arRsp)
    _arRsp->OnPacketArrived();
}
void ASOHCIARManager::OnRequestBufferIRQ() {
  if (_arReq)
    _arReq->OnBufferComplete();
}
void ASOHCIARManager::OnResponseBufferIRQ() {
  if (_arRsp)
    _arRsp->OnBufferComplete();
}

// Optional pull model
bool ASOHCIARManager::DequeueRequest(ARPacketView *outView,
                                     uint32_t *outIndex) {
  return _arReq ? _arReq->TryDequeue(outView, outIndex) : false;
}

bool ASOHCIARManager::DequeueResponse(ARPacketView *outView,
                                      uint32_t *outIndex) {
  return _arRsp ? _arRsp->TryDequeue(outView, outIndex) : false;
}

kern_return_t ASOHCIARManager::RecycleRequest(uint32_t index) {
  return _arReq ? _arReq->Recycle(index) : kIOReturnNotReady;
}

kern_return_t ASOHCIARManager::RecycleResponse(uint32_t index) {
  return _arRsp ? _arRsp->Recycle(index) : kIOReturnNotReady;
}
