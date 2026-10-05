// SPDX-License-Identifier: Apache-2.0
#include <new>
#include <utility>
#include <DriverKit/IOLib.h>
#include "ASFWAvcAudioStream.h"
#include "ASFWAudioDevice.h"

struct ASFWAvcAudioStream_IVars {
    IOLock* lock{nullptr};
    OSSharedPtr<ASFWAudioDevice> owner;
};
bool ASFWAvcAudioStream::init(IOUserAudioDriver* driver, IOUserAudioStreamDirection direction,
                             IOMemoryDescriptor* memory) {
    if (!super::init(driver, direction, memory)) return false;
    auto* storage = IONewZero(ASFWAvcAudioStream_IVars, 1);
    ivars = storage ? ::new(storage) ASFWAvcAudioStream_IVars{} : nullptr;
    if (!ivars) return false;
    ivars->lock = IOLockAlloc();
    return ivars->lock != nullptr;
}
void ASFWAvcAudioStream::free() {
    if (ivars) {
        if (ivars->lock) IOLockFree(ivars->lock);
        ivars->~ASFWAvcAudioStream_IVars();
        IOSafeDeleteNULL(ivars, ASFWAvcAudioStream_IVars, 1);
    }
    super::free();
}
void ASFWAvcAudioStream::Bind(ASFWAudioDevice* owner) {
    if (!ivars || !ivars->lock) return;
    auto replacement = OSSharedPtr<ASFWAudioDevice>(owner, OSRetain);
    IOLockLock(ivars->lock);
    auto prior = std::move(ivars->owner);
    ivars->owner = std::move(replacement);
    IOLockUnlock(ivars->lock);
    // Graph teardown unbinds before removing streams, breaking the owner cycle.
}
kern_return_t ASFWAvcAudioStream::HandleChangeCurrentStreamFormat(
    const IOUserAudioStreamBasicDescription* format) {
    if (!ivars || !ivars->lock) return kIOReturnNotReady;
    IOLockLock(ivars->lock);
    auto owner = ivars->owner;
    IOLockUnlock(ivars->lock);
    return owner ? owner->RequestAvcStreamFormat(this, format) : kIOReturnNotReady;
}
