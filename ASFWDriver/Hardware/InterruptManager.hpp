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
    ~InterruptManager(); // runs Teardown() if the source still exists

    kern_return_t Initialise(IOService* owner,
                             OSSharedPtr<IODispatchQueue> queue,
                             OSSharedPtr<OSAction> handler);

    // True once Initialise() has created the dispatch source. The source is
    // deliberately kept across sleep/wake (see ServiceContext::Reset ForSuspend):
    // destroying and re-creating it re-registers the same interrupt vector and
    // races the old source's deferred kernel-side unregisterInterrupt — on a
    // shared interrupt controller the second unregister panics the kernel
    // (NULL vectorData in IOSharedInterruptController::disableInterrupt).
    [[nodiscard]] bool HasSource() const noexcept { return static_cast<bool>(source_); }

    void Enable();
    void Disable();

    // Final teardown (driver Stop / destruction only — never for suspend).
    // Cancels the dispatch source and defers the last releases to the cancel
    // completion, so the kernel-side free (which unregisters the interrupt)
    // cannot land at an uncontrolled time.
    void Teardown();
    
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
