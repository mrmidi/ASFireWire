#include "PhyAccess.hpp"
#include "ASOHCI.h" // generated header for owner type forward reference if needed
#include <os/log.h>

ASOHCIPHYAccess::~ASOHCIPHYAccess() {
    if (_lock) {
        IORecursiveLockFree(_lock);
        _lock = nullptr;
    }
}

bool ASOHCIPHYAccess::init(ASOHCI * owner, IOPCIDevice * pci, uint8_t bar0) {
    _owner = owner;
    _pci = pci;
    _bar0 = bar0;
    _lock = IORecursiveLockAlloc();
    if (!_lock) return false;
    return true;
}

void ASOHCIPHYAccess::acquire() {
    if (_lock) IORecursiveLockLock(_lock);
}

void ASOHCIPHYAccess::release() {
    if (_lock) IORecursiveLockUnlock(_lock);
}

bool ASOHCIPHYAccess::waitForWriteComplete(uint32_t timeoutIterations) {
    // Wait until WritePending bit clears
    while (timeoutIterations--) {
        uint32_t v=0; _pci->MemoryRead32(_bar0, kOHCI_PhyControl, &v);
        if ((v & kOHCI_PhyControl_WritePending) == 0) return true;
        // small delay – in DriverKit we can optionally spin; no IOSleep in user mode driver env; use IODelay
        IODelay(10); // 10 microseconds incremental wait
    }
    return false;
}

bool ASOHCIPHYAccess::waitForReadComplete(uint32_t timeoutIterations) {
    // ReadDone alias is Int_MasterEnable; hardware sets after read cycle completes with data in PhyControl[23:16]
    while (timeoutIterations--) {
        uint32_t v=0; _pci->MemoryRead32(_bar0, kOHCI_PhyControl, &v);
        if ((v & kOHCI_PhyControl_ReadDone) != 0) return true;
        IODelay(10);
    }
    return false;
}

kern_return_t ASOHCIPHYAccess::readPhyRegister(uint8_t reg, uint8_t * value) {
    if (!value) return kIOReturnBadArgument;
    if (reg > 31) return kIOReturnBadArgument;
    acquire();
    // Ensure previous write complete
    if (!waitForWriteComplete(1000)) {
        os_log(ASLog(), "PHY: read timeout waiting prior write clear (reg=%u)", reg);
        release();
        return kIOReturnTimeout;
    }
    // Initiate read: write PhyControl with Read flag (bit 15 set) and register index bits [10:6]
    uint32_t cmd = (1u<<15) | ((uint32_t)(reg & 0x1F) << 6);
    _pci->MemoryWrite32(_bar0, kOHCI_PhyControl, cmd);
    if (!waitForReadComplete(1000)) {
        os_log(ASLog(), "PHY: read timeout waiting ReadDone (reg=%u)", reg);
        release();
        return kIOReturnTimeout;
    }
    uint32_t v=0; _pci->MemoryRead32(_bar0, kOHCI_PhyControl, &v);
    *value = (uint8_t)((v >> 16) & 0xFF);
    release();
    return kIOReturnSuccess;
}

kern_return_t ASOHCIPHYAccess::writePhyRegister(uint8_t reg, uint8_t value) {
    if (reg > 31) return kIOReturnBadArgument;
    acquire();
    if (!waitForWriteComplete(1000)) {
        os_log(ASLog(), "PHY: write timeout waiting prior write clear (reg=%u)", reg);
        release();
        return kIOReturnTimeout;
    }
    uint32_t cmd = ((uint32_t)(reg & 0x1F) << 6) | ((uint32_t)value << 16) | kOHCI_PhyControl_WritePending;
    _pci->MemoryWrite32(_bar0, kOHCI_PhyControl, cmd);
    // Hardware sets WritePending then clears when done. Wait again.
    if (!waitForWriteComplete(1000)) {
        os_log(ASLog(), "PHY: write completion timeout (reg=%u)", reg);
        release();
        return kIOReturnTimeout;
    }
    release();
    return kIOReturnSuccess;
}

kern_return_t ASOHCIPHYAccess::updatePhyRegisterWithMask(uint8_t reg, uint8_t value, uint8_t mask) {
    if (mask == 0) return kIOReturnSuccess; // nothing to change
    uint8_t cur=0; kern_return_t kr = readPhyRegister(reg, &cur);
    if (kr != kIOReturnSuccess) return kr;
    uint8_t newVal = (cur & ~mask) | (value & mask);
    if (newVal == cur) return kIOReturnSuccess;
    return writePhyRegister(reg, newVal);
}
