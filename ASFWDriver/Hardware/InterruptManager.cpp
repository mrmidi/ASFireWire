#include "InterruptManager.hpp"

#ifndef ASFW_HOST_TEST
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOService.h>
#include <DriverKit/OSAction.h>
#endif

#include <utility>
#include "Logging.hpp"
#include "RegisterMap.hpp"
#include "HardwareInterface.hpp"

namespace ASFW::Driver {

InterruptManager::InterruptManager() = default;
InterruptManager::~InterruptManager() = default;

kern_return_t InterruptManager::Initialise(IOService* owner,
                                           OSSharedPtr<IODispatchQueue> queue,
                                           OSSharedPtr<OSAction> handler) {
    queue_ = std::move(queue);
    handler_ = std::move(handler);

    if (source_) {
        ASFW_LOG(Controller, "InterruptManager: source already exists, updating handler");
        if (handler_ && source_) {
            source_->SetHandler(handler_.get());
        }
        return kIOReturnSuccess;
    }

    IOInterruptDispatchSource* rawSource = nullptr;
    kern_return_t kr = IOInterruptDispatchSource::Create(owner, 0, queue_.get(), &rawSource);
    if (kr != kIOReturnSuccess) {
        return kr;
    }

    source_ = OSSharedPtr(rawSource, OSNoRetain);
    if (handler_) {
        kr = source_->SetHandler(handler_.get());
        if (kr != kIOReturnSuccess) {
            source_.reset();
            return kr;
        }
    }
    return kIOReturnSuccess;
}

void InterruptManager::Enable() {
    ASFW_LOG(Controller, "InterruptManager::Enable called: source=%p", source_.get());
    if (source_) {
        source_->SetEnableWithCompletion(true, nullptr);
    } else {
        ASFW_LOG(Controller, "⚠️  InterruptManager::Enable: NO SOURCE!");
    }
}

void InterruptManager::Disable() {
    if (source_) {
        source_->SetEnableWithCompletion(false, nullptr);
    }
}

// Shadow interrupt mask implementation
// OHCI §5.7: IntMaskSet/IntMaskClear are write-only registers
// Reading IntMaskSet returns undefined value → maintain software shadow
void InterruptManager::EnableInterrupts(uint32_t bits) {
    shadowMask_.fetch_or(bits, std::memory_order_release);
}

void InterruptManager::DisableInterrupts(uint32_t bits) {
    shadowMask_.fetch_and(~bits, std::memory_order_release);
}

uint32_t InterruptManager::EnabledMask() const {
    return shadowMask_.load(std::memory_order_acquire);
}

// Write to hardware and update the software shadow atomically
void InterruptManager::MaskInterrupts(HardwareInterface* hw, uint32_t bits) {
    if (!hw) return;
    hw->Write(Register32::kIntMaskClear, bits);
    DisableInterrupts(bits);  // Update shadow

    // OHCI §6.2 — IntMaskSet/IntMaskClear are write-only strobes.
    // Reads of these registers return undefined data, so the driver
    // must maintain a software shadow (shadowMask_) to track enabled
    // bits. This preserves consistent interrupt enable/disable logic
    // across controller resets and prevents undefined readback state.
}

void InterruptManager::UnmaskInterrupts(HardwareInterface* hw, uint32_t bits) {
    if (!hw) return;
    
    // Ensure masterIntEnable (bit 31) is always set when unmasking any bit.
    // Per OHCI §5.7: No interrupts are delivered to the system unless masterIntEnable=1.
    // This prevents lost-interrupt issues after bus reset or mask manipulation.
    const uint32_t cur  = shadowMask_.load(std::memory_order_acquire);
    const uint32_t want = (cur | bits | IntMaskBits::kMasterIntEnable);
    const uint32_t add  = want & ~cur;
    
    if (add) {
        hw->Write(Register32::kIntMaskSet, add);
        shadowMask_.fetch_or(add, std::memory_order_release);
        ASFW_LOG(Hardware, "IntMask updated: shadow=0x%08x add=0x%08x (masterEnable=%d busReset=%d)",
                 shadowMask_.load(), add,
                 (shadowMask_.load() >> 31) & 1,
                 (shadowMask_.load() >> 17) & 1);
    }
}

} // namespace ASFW::Driver
