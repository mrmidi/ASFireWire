#include "PhyAccess.hpp"
#include "ASOHCI.h" // generated header for owner type forward reference if needed
#include <os/log.h>

ASOHCIPHYAccess::~ASOHCIPHYAccess() {
  if (_lock) {
    IORecursiveLockFree(_lock);
    _lock = nullptr;
  }
}

bool ASOHCIPHYAccess::init(ASOHCI *owner, IOPCIDevice *pci, uint8_t bar0) {
  _owner = owner;
  _pci = pci;
  _bar0 = bar0;
  _lock = IORecursiveLockAlloc();
  if (!_lock)
    return false;
  return true;
}

void ASOHCIPHYAccess::acquire() {
  if (_lock)
    IORecursiveLockLock(_lock);
}

void ASOHCIPHYAccess::release() {
  if (_lock)
    IORecursiveLockUnlock(_lock);
}

bool ASOHCIPHYAccess::waitForWriteComplete(uint32_t timeoutIterations) {
  // OHCI 5.12: Wait until wrReg bit clears (hardware clears when request sent
  // to PHY) Mirror Linux strategy: a few quick polls, then ms sleeps up to
  // ~100ms total if needed.
  uint32_t v = 0;
  // Fast path: ~30 quick polls with short delay
  for (uint32_t i = 0; i < 30; ++i) {
    _pci->MemoryRead32(_bar0, kOHCI_PhyControl, &v);
    if ((v & kOHCI_PhyControl_wrReg) == 0)
      return true;
    IODelay(10);
  }
  // Slow path: up to ~100ms in 1ms steps
  for (uint32_t i = 0; i < 100; ++i) {
    _pci->MemoryRead32(_bar0, kOHCI_PhyControl, &v);
    if ((v & kOHCI_PhyControl_wrReg) == 0)
      return true;
    IOSleep(1);
  }
  return false;
}

bool ASOHCIPHYAccess::waitForReadComplete(uint32_t timeoutIterations) {
  // OHCI 5.12: Wait for rdDone bit set (hardware sets when PHY returns register
  // data)
  uint32_t v = 0;
  for (uint32_t i = 0; i < 30; ++i) {
    _pci->MemoryRead32(_bar0, kOHCI_PhyControl, &v);
    if ((v & kOHCI_PhyControl_rdDone) != 0)
      return true;
    IODelay(10);
  }
  for (uint32_t i = 0; i < 100; ++i) {
    _pci->MemoryRead32(_bar0, kOHCI_PhyControl, &v);
    if ((v & kOHCI_PhyControl_rdDone) != 0)
      return true;
    IOSleep(1);
  }
  return false;
}

kern_return_t ASOHCIPHYAccess::readPhyRegister(uint8_t reg, uint8_t *value) {
  if (!value)
    return kIOReturnBadArgument;
  if (reg > 31)
    return kIOReturnBadArgument;

  // OHCI 5.12: "Software shall not issue a read of PHY register 0"
  if (reg == 0) {
    os_log(ASLog(),
           "PHY: register 0 read forbidden by OHCI spec - use NodeID register");
    return kIOReturnBadArgument;
  }

  acquire();

  // OHCI 5.12: Ensure no outstanding PHY register request
  if (!waitForWriteComplete(1000)) {
    os_log(ASLog(), "PHY: read timeout waiting prior write clear (reg=%u)",
           reg);
    release();
    return kIOReturnTimeout;
  }

  // OHCI 5.12: Clear rdDone before initiating read
  uint32_t v = 0;
  _pci->MemoryRead32(_bar0, kOHCI_PhyControl, &v);
  if (v & kOHCI_PhyControl_rdDone) {
    // rdDone is cleared by hardware when rdReg or wrReg is set - we'll set
    // rdReg next
  }

  // OHCI 5.12: Initiate read - set rdReg (bit 15) and regAddr (bits 10:6)
  uint32_t cmd = kOHCI_PhyControl_rdReg |
                 ((uint32_t)(reg & 0x1F) << kOHCI_PhyControl_regAddr_Shift);
  _pci->MemoryWrite32(_bar0, kOHCI_PhyControl, cmd);

  // OHCI 5.12: Wait for rdDone bit to be set by hardware
  if (!waitForReadComplete(1000)) {
    os_log(ASLog(), "PHY: read timeout waiting rdDone (reg=%u)", reg);
    release();
    return kIOReturnTimeout;
  }

  // OHCI 5.12: Extract rdData from bits 23:16
  _pci->MemoryRead32(_bar0, kOHCI_PhyControl, &v);
  *value = (uint8_t)((v & kOHCI_PhyControl_rdData_Mask) >>
                     kOHCI_PhyControl_rdData_Shift);

  release();
  return kIOReturnSuccess;
}

kern_return_t ASOHCIPHYAccess::writePhyRegister(uint8_t reg, uint8_t value) {
  if (reg > 31)
    return kIOReturnBadArgument;

  acquire();

  // OHCI 5.12: Ensure no outstanding PHY register request
  if (!waitForWriteComplete(1000)) {
    os_log(ASLog(), "PHY: write timeout waiting prior request clear (reg=%u)",
           reg);
    release();
    return kIOReturnTimeout;
  }

  // OHCI 5.12: Initiate write - set wrReg (bit 14), regAddr (bits 10:6), wrData
  // (bits 7:0)
  uint32_t cmd = kOHCI_PhyControl_wrReg |
                 ((uint32_t)(reg & 0x1F) << kOHCI_PhyControl_regAddr_Shift) |
                 ((uint32_t)value << kOHCI_PhyControl_wrData_Shift);

  _pci->MemoryWrite32(_bar0, kOHCI_PhyControl, cmd);

  // OHCI 5.12: Wait for wrReg bit to clear (hardware clears when request sent
  // to PHY)
  if (!waitForWriteComplete(1000)) {
    os_log(ASLog(), "PHY: write completion timeout (reg=%u)", reg);
    release();
    return kIOReturnTimeout;
  }

  release();
  return kIOReturnSuccess;
}

kern_return_t ASOHCIPHYAccess::updatePhyRegisterWithMask(uint8_t reg,
                                                         uint8_t value,
                                                         uint8_t mask) {
  if (mask == 0)
    return kIOReturnSuccess; // nothing to change
  uint8_t cur = 0;
  kern_return_t kr = readPhyRegister(reg, &cur);
  if (kr != kIOReturnSuccess)
    return kr;
  uint8_t newVal = (cur & ~mask) | (value & mask);
  if (newVal == cur)
    return kIOReturnSuccess;
  return writePhyRegister(reg, newVal);
}
