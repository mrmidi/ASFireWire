#pragma once

#include <DriverKit/IOReturn.h>

#ifdef ASFW_HOST_TEST
#include "HostDriverKitStubs.hpp"
#else
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSAction.h>
#endif

#include "ControllerTypes.hpp"

#include <atomic>

namespace ASFW::Driver {

class InterruptManager {
public:
    InterruptManager();
    ~InterruptManager();

    kern_return_t Initialise(IOService* owner,
                             OSSharedPtr<IODispatchQueue> queue,
                             OSSharedPtr<OSAction> handler);

    void Enable();
    void Disable();
    
    // Shadow mask management (OHCI IntMaskSet/Clear are write-only)
    // Reading IntMaskSet returns undefined value → use software shadow instead
    void EnableInterrupts(uint32_t bits);
    void DisableInterrupts(uint32_t bits);
    uint32_t EnabledMask() const;
    
    // Write to hardware and update the software shadow (single source of truth)
    // Call these instead of writing IntMaskSet/Clear directly to keep shadow in sync
    void MaskInterrupts(class HardwareInterface* hw, uint32_t bits);
    void UnmaskInterrupts(class HardwareInterface* hw, uint32_t bits);

private:
    OSSharedPtr<IOInterruptDispatchSource> source_;
    OSSharedPtr<IODispatchQueue> queue_;
    OSSharedPtr<OSAction> handler_;
    
    // CRITICAL: Shadow copy of interrupt mask (IntMaskSet/Clear are write-only per OHCI §5.7)
    std::atomic<uint32_t> shadowMask_{0};
};

} // namespace ASFW::Driver
