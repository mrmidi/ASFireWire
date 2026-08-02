#include "InterruptManager.hpp"

#ifndef ASFW_HOST_TEST
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOService.h>
#include <DriverKit/OSAction.h>
#endif

#include <utility>
#include "../Logging/Logging.hpp"
#include "RegisterMap.hpp"
#include "HardwareInterface.hpp"

namespace ASFW::Driver {

InterruptManager::InterruptManager() = default;
InterruptManager::~InterruptManager() { Teardown(); }

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
    // The manager (and its shadow mask) now survives suspend, but the silicon's
    // IntMask does not — a stale shadow would make UnmaskInterrupts skip the
    // hardware write after the wake re-init's soft reset.
    shadowMask_.store(0, std::memory_order_release);
}

void InterruptManager::Teardown() {
    if (!source_) {
        queue_.reset();
        handler_.reset();
        return;
    }

    source_->SetEnableWithCompletion(false, nullptr);

    // The kernel-side source performs IOService::unregisterInterrupt in its
    // free(), i.e. whenever the last reference drops. Hand the final references
    // to the cancel completion so that free is ordered after cancellation
    // instead of landing whenever the async release RPC is processed. (Releasing
    // before the completion ran is the Cancel_Impl crash noted in
    // WatchdogCoordinator::Stop; releasing without Cancel at all is the
    // 2026-07-11 IOSharedInterruptController panic.)
    IOInterruptDispatchSource* source = source_.detach();
    OSAction* handler = handler_.detach();
    const kern_return_t kr = source->Cancel(^{
        if (handler) {
            handler->release();
        }
        source->release();
    });
    if (kr != kIOReturnSuccess) {
        // Completion will never run; fall back to direct release.
        if (handler) {
            handler->release();
        }
        source->release();
    }
    queue_.reset();
}

void InterruptManager::EnableInterrupts(uint32_t bits) {
    shadowMask_.fetch_or(bits, std::memory_order_release);
}

void InterruptManager::DisableInterrupts(uint32_t bits) {
    shadowMask_.fetch_and(~bits, std::memory_order_release);
}

uint32_t InterruptManager::EnabledMask() const {
    return shadowMask_.load(std::memory_order_acquire);
}

void InterruptManager::MaskInterrupts(HardwareInterface* hw, uint32_t bits) {
    if (!hw) return;
    hw->Write(Register32::kIntMaskClear, bits);
    DisableInterrupts(bits);
}

void InterruptManager::UnmaskInterrupts(HardwareInterface* hw, uint32_t bits) {
    if (!hw) return;
    
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
