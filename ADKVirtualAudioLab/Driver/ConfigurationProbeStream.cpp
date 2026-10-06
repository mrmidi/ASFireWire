#include <new>
#include <utility>
#include <DriverKit/IOLib.h>
#include "ConfigurationProbeStream.h"
#include "ConfigurationProbeDevice.h"
struct ConfigurationProbeStream_IVars {
    IOLock* lock{nullptr};
    OSSharedPtr<ConfigurationProbeDevice> owner;
};
bool ConfigurationProbeStream::init(IOUserAudioDriver* driver, IOUserAudioStreamDirection direction,
    IOMemoryDescriptor* memory) {
    if (!super::init(driver, direction, memory)) return false;
    ivars = IONewZero(ConfigurationProbeStream_IVars, 1);
    if (!ivars) return false;
    ivars->lock = IOLockAlloc();
    return ivars->lock != nullptr;
}
void ConfigurationProbeStream::free() {
    if (ivars && ivars->lock) IOLockFree(ivars->lock);
    IOSafeDeleteNULL(ivars, ConfigurationProbeStream_IVars, 1);
    super::free();
}
void ConfigurationProbeStream::Bind(ConfigurationProbeDevice* owner) {
    if (!ivars || !ivars->lock) return;
    auto replacement = OSSharedPtr<ConfigurationProbeDevice>(owner, OSRetain);
    IOLockLock(ivars->lock);
    auto prior = std::move(ivars->owner);
    ivars->owner = std::move(replacement);
    IOLockUnlock(ivars->lock);
    // Release outside the lock; Shutdown breaks the explicit ownership cycle.
}
kern_return_t ConfigurationProbeStream::HandleChangeCurrentStreamFormat(
    const IOUserAudioStreamBasicDescription* format) {
    if (!ivars || !ivars->lock) return kIOReturnNotReady;
    IOLockLock(ivars->lock);
    auto owner = ivars->owner;
    IOLockUnlock(ivars->lock);
    return owner ? owner->RequestFormat(this, format) : kIOReturnNotReady;
}
