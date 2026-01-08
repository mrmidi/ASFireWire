#pragma once

#include <DriverKit/IOReturn.h>

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSAction.h>
#endif

#include "../Controller/ControllerTypes.hpp"

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
    
    void EnableInterrupts(uint32_t bits);
    void DisableInterrupts(uint32_t bits);
    uint32_t EnabledMask() const;
    
    void MaskInterrupts(class HardwareInterface* hw, uint32_t bits);
    void UnmaskInterrupts(class HardwareInterface* hw, uint32_t bits);

private:
    OSSharedPtr<IOInterruptDispatchSource> source_;
    OSSharedPtr<IODispatchQueue> queue_;
    OSSharedPtr<OSAction> handler_;
    
    std::atomic<uint32_t> shadowMask_{0};
};

} // namespace ASFW::Driver
