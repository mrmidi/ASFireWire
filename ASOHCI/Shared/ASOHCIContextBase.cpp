//
// ASOHCIContextBase.cpp
// Shared OHCI context base (AT/AR)
//
// Spec refs: OHCI 1.1 §3.1 (Context registers), §3.1.1 (run/active/dead/wake), §3.1.2 (CommandPtr)
//

#include "ASOHCIContextBase.hpp"
#include "LogHelper.hpp"
#include <DriverKit/IOLib.h>
#include <DriverKit/IOLib.h>

kern_return_t ASOHCIContextBase::Initialize(IOPCIDevice* pci,
                                            uint8_t barIndex,
                                            ASContextKind kind,
                                            const ASContextOffsets& offsets)
{
    if (!pci) {
        os_log(ASLog(), "ASOHCIContextBase: Invalid PCI device");
        return kIOReturnBadArgument;
    }

    _pci = pci;
    _bar = barIndex;
    _kind = kind;
    _offs = offsets;

    os_log(ASLog(), "ASOHCIContextBase: Init kind=%u base=0x%x set=0x%x clr=0x%x cmd=0x%x",
           static_cast<unsigned>(_kind), _offs.contextBase, _offs.contextControlSet, _offs.contextControlClear, _offs.commandPtr);

    return kIOReturnSuccess;
}

kern_return_t ASOHCIContextBase::Start()
{
    if (!_pci) return kIOReturnNotReady;

    uint32_t cc = ReadContextControlCached();
    if (cc & kOHCI_ContextControl_active) {
        os_log(ASLog(), "ASOHCIContextBase: Cannot start - context active");
        return kIOReturnBusy;
    }
    if (cc & kOHCI_ContextControl_dead) {
        os_log(ASLog(), "ASOHCIContextBase: Cannot start - context dead");
        return kIOReturnError;
    }

    // Empty program (addr=0, Z=0). Caller will enqueue real work.
    WriteCommandPtr(0, 0);
    WriteContextSet(kOHCI_ContextControl_run);
    os_log(ASLog(), "ASOHCIContextBase: Started (kind=%u)", static_cast<unsigned>(_kind));
    return kIOReturnSuccess;
}

kern_return_t ASOHCIContextBase::Stop()
{
    if (!_pci) return kIOReturnNotReady;

    WriteContextClear(kOHCI_ContextControl_run);

    const uint32_t kMaxWaitMS = 100;
    const uint32_t kWaitStepMS = 1;
    uint32_t waited = 0;
    while (waited < kMaxWaitMS) {
        uint32_t cc = ReadContextControlCached();
        if ((cc & kOHCI_ContextControl_active) == 0) {
            os_log(ASLog(), "ASOHCIContextBase: Stopped after %u ms", waited);
            return kIOReturnSuccess;
        }
        IOSleep(kWaitStepMS);
        waited += kWaitStepMS;
    }
    os_log(ASLog(), "ASOHCIContextBase: Timeout waiting to stop");
    return kIOReturnTimeout;
}

kern_return_t ASOHCIContextBase::Wake()
{
    if (!_pci) return kIOReturnNotReady;
    WriteContextSet(kOHCI_ContextControl_wake);
    os_log(ASLog(), "ASOHCIContextBase: Wake signaled");
    return kIOReturnSuccess;
}

void ASOHCIContextBase::OnBusResetBegin()
{
    // Common policy: stop acquiring during reset window
    WriteContextClear(kOHCI_ContextControl_run);
}

void ASOHCIContextBase::OnBusResetEnd()
{
    _outstanding = 0;
}

bool ASOHCIContextBase::IsRunning() const
{
    if (!_pci) return false;
    return (ReadContextControlCached() & kOHCI_ContextControl_run) != 0;
}

bool ASOHCIContextBase::IsActive() const
{
    if (!_pci) return false;
    return (ReadContextControlCached() & kOHCI_ContextControl_active) != 0;
}

uint32_t ASOHCIContextBase::ReadContextControlCached() const
{
    if (!_pci) return 0;
    uint32_t v = 0;
    _pci->MemoryRead32(_bar, _offs.contextBase, &v);
    return v;
}

kern_return_t ASOHCIContextBase::WriteCommandPtr(uint32_t descriptorAddress, uint8_t zNibble)
{
    if (!_pci) return kIOReturnNotReady;

    if ((descriptorAddress & 0xF) != 0) {
        os_log(ASLog(), "ASOHCIContextBase: CommandPtr addr 0x%x not 16B aligned", descriptorAddress);
        return kIOReturnBadArgument;
    }
    if (zNibble > 0xF) {
        os_log(ASLog(), "ASOHCIContextBase: Z nibble invalid %u", zNibble);
        return kIOReturnBadArgument;
    }

    uint32_t cmd = (descriptorAddress & 0xFFFFFFF0u) | (zNibble & 0xFu);
    _pci->MemoryWrite32(_bar, _offs.commandPtr, cmd);
    os_log(ASLog(), "ASOHCIContextBase: CommandPtr=0x%08x (addr=0x%x Z=%u)", cmd, descriptorAddress, zNibble);
    return kIOReturnSuccess;
}

void ASOHCIContextBase::WriteContextSet(uint32_t value)
{
    if (_pci) {
        _pci->MemoryWrite32(_bar, _offs.contextControlSet, value);
    }
}

void ASOHCIContextBase::WriteContextClear(uint32_t value)
{
    if (_pci) {
        _pci->MemoryWrite32(_bar, _offs.contextControlClear, value);
    }
}

uint32_t ASOHCIContextBase::ReadContextSet() const
{
    // Read current ContextControl via read address (contextBase)
    if (!_pci) return 0;
    uint32_t v = 0;
    _pci->MemoryRead32(_bar, _offs.contextBase, &v);
    return v;
}

void ASOHCIContextBase::RecoverDeadContext()
{
    WriteContextClear(kOHCI_ContextControl_run);
    _outstanding = 0;
}
