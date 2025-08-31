#include "ASOHCIInterruptRouter.hpp"
#include "../Async/ASOHCIARManager.hpp"
#include "../Async/ASOHCIATManager.hpp"
#include "../Core/ASOHCIIVars.h"
#include "../Core/LogHelper.hpp"
#include "../Core/OHCIConstants.hpp"
#include "../Isoch/ASOHCIIRManager.hpp"
#include "../Isoch/ASOHCIITManager.hpp"
#include "ASOHCIRegisterIO.hpp"
#include <DriverKit/IODispatchQueue.h>
#include <net.mrmidi.ASFireWire.ASOHCI/ASOHCI.h>

// OSDefineMetaClassAndStructors not needed in DriverKit

// Destructor not needed in DriverKit

void ASOHCIInterruptRouter::SetATManager(ASOHCIATManager *m) { _at = m; }
void ASOHCIInterruptRouter::SetARManager(ASOHCIARManager *m) { _ar = m; }
void ASOHCIInterruptRouter::SetITManager(ASOHCIITManager *m) { _it = m; }
void ASOHCIInterruptRouter::SetIRManager(ASOHCIIRManager *m) { _ir = m; }
void ASOHCIInterruptRouter::SetController(ASOHCI *ohci) { _ohci = ohci; }

void ASOHCIInterruptRouter::OnAT_Request_TxComplete() {
  if (_at)
    _at->OnInterrupt_ReqTxComplete();
}
void ASOHCIInterruptRouter::OnAT_Response_TxComplete() {
  if (_at)
    _at->OnInterrupt_RspTxComplete();
}

void ASOHCIInterruptRouter::OnAR_Request_PacketArrived() {
  if (_ar)
    _ar->OnRequestPacketIRQ();
}
void ASOHCIInterruptRouter::OnAR_Response_PacketArrived() {
  if (_ar)
    _ar->OnResponsePacketIRQ();
}

void ASOHCIInterruptRouter::OnIsoTxMask(uint32_t mask) {
  if (_it && mask)
    _it->OnInterrupt_TxEventMask(mask);
}
void ASOHCIInterruptRouter::OnIsoRxMask(uint32_t mask) {
  if (_ir && mask)
    _ir->OnInterrupt_RxEventMask(mask);
}

void ASOHCIInterruptRouter::OnCycleInconsistent(uint64_t time) {
  if (!_ohci || !_ohci->ivars)
    return;
  auto *iv = _ohci->ivars;

  const uint64_t rate_limit_ns = 1000000000ULL; // 1s
  bool should_log = false;
  iv->cycleInconsistentCount++;
  if (iv->lastCycleInconsistentTime == 0 ||
      (time > iv->lastCycleInconsistentTime &&
       (time - iv->lastCycleInconsistentTime) > rate_limit_ns)) {
    should_log = true;
    iv->lastCycleInconsistentTime = time;
  }
  if (should_log) {
    os_log(ASLog(),
           "ASOHCI: Cycle inconsistent detected (count=%u) - isochronous "
           "timing mismatch",
           iv->cycleInconsistentCount);
  }

  if (_it)
    _it->OnInterrupt_CycleInconsistent();
}

void ASOHCIInterruptRouter::OnPostedWriteError() {
  if (!_ohci || !_ohci->ivars || !_ohci->ivars->pciDevice)
    return;
  auto *iv = _ohci->ivars;
  auto *pci = iv->pciDevice.get();
  uint32_t hi = 0, lo = 0;
  if (iv->regs) {
    iv->regs->Read32(kOHCI_PostedWriteAddressHi, &hi);
    iv->regs->Read32(kOHCI_PostedWriteAddressLo, &lo);
    iv->regs->Write32(kOHCI_IntEventClear, kOHCI_Int_PostedWriteErr);
  } else {
    pci->MemoryRead32(iv->barIndex, kOHCI_PostedWriteAddressHi, &hi);
    pci->MemoryRead32(iv->barIndex, kOHCI_PostedWriteAddressLo, &lo);
    pci->MemoryWrite32(iv->barIndex, kOHCI_IntEventClear,
                       kOHCI_Int_PostedWriteErr);
  }
  os_log(ASLog(), "ASOHCI: Posted Write Error addr=%08x:%08x (cleared)", hi,
         lo);
}

void ASOHCIInterruptRouter::OnCycleTooLong() {
  if (!_ohci || !_ohci->ivars || !_ohci->ivars->pciDevice)
    return;
  auto *iv = _ohci->ivars;
  auto *pci = iv->pciDevice.get();
  uint32_t nodeIdReg = 0;
  if (iv->regs)
    iv->regs->Read32(kOHCI_NodeID, &nodeIdReg);
  else
    pci->MemoryRead32(iv->barIndex, kOHCI_NodeID, &nodeIdReg);
  bool hardwareIsRoot = ((nodeIdReg & kOHCI_NodeID_root) != 0);
  bool idValid = ((nodeIdReg & kOHCI_NodeID_idValid) != 0);
  if (idValid && hardwareIsRoot) {
    if (iv->regs)
      iv->regs->Write32(kOHCI_LinkControlSet, kOHCI_LC_CycleMaster);
    else
      pci->MemoryWrite32(iv->barIndex, kOHCI_LinkControlSet,
                         kOHCI_LC_CycleMaster);
    os_log(ASLog(), "ASOHCI: CycleTooLong detected - asserting CycleMaster "
                    "(root node takeover)");
  } else {
    os_log(ASLog(),
           "ASOHCI: CycleTooLong detected but not root node (idValid=%d "
           "hwRoot=%d)",
           idValid, hardwareIsRoot);
  }
}

void ASOHCIInterruptRouter::OnBusReset(uint64_t time) {
  if (!_ohci || !_ohci->ivars || !_ohci->ivars->pciDevice)
    return;
  auto *iv = _ohci->ivars;
  auto *pci = iv->pciDevice.get();

  // Mask BusReset while handling
  pci->MemoryWrite32(iv->barIndex, kOHCI_IntMaskClear, kOHCI_Int_BusReset);
  iv->busResetMasked = true;
  iv->lastBusResetTime = time;
  os_log(ASLog(), "ASOHCI: BusReset masked during handling");

  // Commit Config ROM header if staged
  // if (iv->configROMManager) {
  //   iv->configROMManager->CommitOnBusReset();
  // }

  // Reset topology accumulation for new cycle
  // if (iv->topology) iv->topology->Clear();

  // Notify managers
  if (_at)
    _at->OnBusResetBegin();
  if (_ir)
    _ir->OnInterrupt_BusReset();

  // Track collapsed resets or start a new cycle
  if (iv->selfIDInProgress) {
    ++iv->collapsedBusResets;
  } else {
    iv->selfIDInProgress = true;
    iv->collapsedBusResets = 0;
    _ohci->ArmSelfIDReceive(/*clearCount=*/true);
  }

  // Ack the BusReset event bit before later re-enabling
  pci->MemoryWrite32(iv->barIndex, kOHCI_IntEventClear, kOHCI_Int_BusReset);

  // Bus reset callback to link API client
  // if (iv->busResetCallback) {
  //   iv->busResetCallback(iv->busResetCallbackContext);
  // }

  // Log NodeID change if relevant
  uint32_t nodeID = 0;
  pci->MemoryRead32(iv->barIndex, kOHCI_NodeID, &nodeID);
  bool idValid = ((nodeID >> 31) & 1) != 0;
  bool isRoot = ((nodeID >> 30) & 1) != 0;
  if (nodeID != iv->lastLoggedNodeID || idValid != iv->lastLoggedValid ||
      isRoot != iv->lastLoggedRoot) {
    uint8_t nAdr = (uint8_t)((nodeID >> 16) & 0x3F);
    os_log(ASLog(), "ASOHCI: NodeID=0x%08x valid=%d root=%d addr=%u (changed)",
           nodeID, idValid, isRoot, nAdr);
    iv->lastLoggedNodeID = nodeID;
    iv->lastLoggedValid = idValid;
    iv->lastLoggedRoot = isRoot;
  }
}

void ASOHCIInterruptRouter::OnSelfIDComplete(uint32_t selfIDCountReg,
                                             uint32_t generation,
                                             bool errorFlag) {
  if (!_ohci || !_ohci->ivars)
    return;
  auto *iv = _ohci->ivars;
  if (__atomic_load_n(&iv->stopping, __ATOMIC_ACQUIRE) ||
      __atomic_load_n(&iv->deviceGone, __ATOMIC_ACQUIRE)) {
    return;
  }

  // Create queue on-demand
  if (!_selfIDQueue) {
    IODispatchQueue *q = nullptr;
    kern_return_t kr = IODispatchQueue::Create("asohci_selfid", 0, 0, &q);
    if (kr == kIOReturnSuccess && q) {
      _selfIDQueue = q;
    } else {
      // Fallback to immediate processing
      ProcessSelfIDComplete(selfIDCountReg, generation, errorFlag);
      return;
    }
  }

  // Dispatch deferred processing
  struct Work {
    ASOHCIInterruptRouter *router;
    uint32_t cnt;
    uint32_t gen;
    bool err;
  };
  Work *w = new Work{this, selfIDCountReg, generation, errorFlag};
  if (!w) {
    ProcessSelfIDComplete(selfIDCountReg, generation, errorFlag);
    return;
  }
  _selfIDQueue->DispatchAsync(^{
    w->router->ProcessSelfIDComplete(w->cnt, w->gen, w->err);
    delete w;
  });
}

void ASOHCIInterruptRouter::ProcessSelfIDComplete(uint32_t selfIDCountReg,
                                                  uint32_t generation,
                                                  bool errorFlag) {
  (void)generation; // manager reads generation from register as needed
  if (!_ohci || !_ohci->ivars || !_ohci->ivars->pciDevice)
    return;
  auto *iv = _ohci->ivars;
  auto *pci = iv->pciDevice.get();

  os_log(ASLog(), "ASOHCI: SelfID count=%u gen=%u error=%d",
         (selfIDCountReg & kOHCI_SelfIDCount_selfIDSize) >> 2, generation,
         errorFlag ? 1 : 0);

  if (iv->selfIDManager) {
    // iv->selfIDManager->OnSelfIDComplete(selfIDCountReg);
  }
  // if (iv->selfIDCallback) {
  //   iv->selfIDCallback(iv->selfIDCallbackContext);
  // }

  if (!iv->cycleTimerArmed) {
    pci->MemoryWrite32(iv->barIndex, kOHCI_LinkControlSet,
                       kOHCI_LC_CycleTimerEnable);

    uint32_t nodeIdReg = 0;
    pci->MemoryRead32(iv->barIndex, kOHCI_NodeID, &nodeIdReg);
    bool hardwareIsRoot = ((nodeIdReg & kOHCI_NodeID_root) != 0);
    bool idValid = ((nodeIdReg & kOHCI_NodeID_idValid) != 0);
    if (idValid && hardwareIsRoot) {
      pci->MemoryWrite32(iv->barIndex, kOHCI_LinkControlSet,
                         kOHCI_LC_CycleMaster);
      os_log(ASLog(), "ASOHCI: CycleMaster asserted - this node is root");
    }
    uint32_t lcPost = 0;
    pci->MemoryRead32(iv->barIndex, kOHCI_LinkControlSet, &lcPost);
    os_log(ASLog(), "ASOHCI: CycleTimerEnable asserted (LinkControl=0x%08x)",
           lcPost);
    iv->cycleTimerArmed = true;
    pci->MemoryWrite32(iv->barIndex, kOHCI_IntMaskSet,
                       kOHCI_Int_CycleInconsistent);
  }

  iv->selfIDInProgress = false;
  iv->selfIDArmed = false;
  _ohci->ArmSelfIDReceive(false);
  pci->MemoryWrite32(iv->barIndex, kOHCI_IntMaskSet, kOHCI_Int_BusReset);
  iv->busResetMasked = false;
  os_log(ASLog(), "ASOHCI: BusReset re-enabled after Self-ID completion");
}
