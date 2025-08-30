//
// ASOHCIITManager.cpp
// Isochronous Transmit (IT) manager skeleton
//
// Spec refs (OHCI 1.1): Chapter 6 (IsoXmitIntEvent/Mask demux), §9.2 (context
// discovery),
//   §9.4 (appending constraints), §9.5 (interrupt causes / cycle inconsistent
//   handling)
//

#include "ASOHCIITManager.hpp"
#include "ASOHCICtxProbe.hpp"
#include "LogHelper.hpp"

#include <DriverKit/IOReturn.h>

kern_return_t ASOHCIITManager::Initialize(IOPCIDevice *pci, uint8_t barIndex,
                                          const ITPolicy &defaultPolicy) {
  if (!pci)
    return kIOReturnBadArgument;
  _pci = pci;
  _bar = barIndex;
  _defaultPolicy = defaultPolicy;

  // Use MMIO probe of IT windows: detect real, responding contexts (§4.2 /
  // §9.2).
  _numCtx = ProbeITContextCount(pci, barIndex).count;
  if (_numCtx > 32)
    _numCtx = 32;
  os_log(ASLog(),
         "ITManager: Initialize (bar=%u, dynamic allocation) contexts=%u", _bar,
         _numCtx);

  // Initialize shared descriptor pool with dynamic allocation (Linux-style)
  kern_return_t r = _pool.Initialize(pci, barIndex);
  if (r != kIOReturnSuccess) {
    os_log(ASLog(), "ITManager: pool init failed 0x%x", r);
  }

  for (uint32_t i = 0; i < _numCtx; ++i) {
    _ctx[i].Initialize(pci, barIndex, i);
    _ctx[i].ApplyPolicy(_defaultPolicy);
  }
  return kIOReturnSuccess;
}

kern_return_t ASOHCIITManager::StartAll() {
  if (!_pci)
    return kIOReturnNotReady;
  // Enable interrupt mask bits for each context present
  uint32_t mask = (_numCtx >= 32) ? 0xFFFFFFFFu : ((1u << _numCtx) - 1u);
  _pci->MemoryWrite32(_bar, kOHCI_IsoXmitIntMaskSet, mask);
  for (uint32_t i = 0; i < _numCtx; ++i) {
    _ctx[i].Start();
  }
  os_log(ASLog(), "ITManager: StartAll enabled mask=0x%x", mask);
  return kIOReturnSuccess;
}

kern_return_t ASOHCIITManager::StopAll() {
  if (!_pci)
    return kIOReturnNotReady;
  _pci->MemoryWrite32(_bar, kOHCI_IsoXmitIntMaskClear, 0xFFFFFFFFu);
  for (uint32_t i = 0; i < _numCtx; ++i) {
    _ctx[i].Stop();
  }
  os_log(ASLog(), "ITManager: StopAll");
  return kIOReturnSuccess;
}

kern_return_t ASOHCIITManager::Queue(uint32_t ctxId, ITSpeed spd, uint8_t tag,
                                     uint8_t channel, uint8_t sy,
                                     const uint32_t *payloadPAs,
                                     const uint32_t *payloadSizes,
                                     uint32_t fragments,
                                     const ITQueueOptions &opts) {
  if (ctxId >= _numCtx)
    return kIOReturnBadArgument;
  if (!_pool.IsInitialized())
    return kIOReturnNotReady;
  if (fragments == 0 || !payloadPAs || !payloadSizes)
    return kIOReturnBadArgument;

  _builder.Begin(_pool, fragments + 2); // header + payloads + last
  _builder.AddHeaderImmediate(spd, tag, channel, sy, 0 /*dataLength TBD*/,
                              ITIntPolicy::kAlways);
  uint32_t totalLen = 0;
  for (uint32_t i = 0; i < fragments; ++i) {
    _builder.AddPayloadFragment(payloadPAs[i], payloadSizes[i]);
    totalLen += payloadSizes[i];
  }
  // (Future) patch length into header's quad0 if needed.
  ITDesc::Program p = _builder.Finalize();
  if (!p.headPA)
    return kIOReturnNoResources;
  return _ctx[ctxId].Enqueue(p, opts);
}

void ASOHCIITManager::OnInterrupt_TxEventMask(uint32_t mask) {
  uint32_t remaining = mask;
  while (remaining) {
    uint32_t bit = __builtin_ctz(remaining);
    if (bit < _numCtx) {
      _ctx[bit].OnInterruptTx();
    }
    remaining &= ~(1u << bit);
  }
}

void ASOHCIITManager::OnInterrupt_CycleInconsistent() {
  for (uint32_t i = 0; i < _numCtx; ++i) {
    _ctx[i].OnCycleInconsistent();
  }
}

uint32_t ASOHCIITManager::ProbeContextCount() {
  if (!_pci)
    return 0;
  // Strategy: write all-ones to mask set, read back event clear; hardware only
  // implements bits for existing contexts.
  _pci->MemoryWrite32(_bar, kOHCI_IsoXmitIntMaskSet, 0xFFFFFFFFu);
  uint32_t mask = 0;
  _pci->MemoryRead32(_bar, kOHCI_IsoXmitIntMaskSet,
                     &mask); // reading set returns current mask state
  // Clear any unintended enables
  _pci->MemoryWrite32(_bar, kOHCI_IsoXmitIntMaskClear, 0xFFFFFFFFu);
  if (mask == 0)
    return 0;
  // Count contiguous low-order bits until first zero (assume contexts numbered
  // from 0)
  uint32_t count = 0;
  while (mask & 0x1) {
    count++;
    mask >>= 1;
  }
  return count;
}
