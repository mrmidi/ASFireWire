#include <AudioDriverKit/AudioDriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSString.h>
#include <os/log.h>
#include "VirtualAudioDriver.h"
#include "VirtualAudioDevice.h"

#define LAB_LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "[ADKLab] " fmt, ##__VA_ARGS__)

struct VirtualAudioDriver_IVars
{
    OSSharedPtr<IODispatchQueue> workQueue;
    OSSharedPtr<VirtualAudioDevice> audioDevice;
};

bool VirtualAudioDriver::init()
{
    if (!super::init()) {
        return false;
    }
    
    ivars = IONewZero(VirtualAudioDriver_IVars, 1);
    if (ivars == nullptr) {
        return false;
    }
    
    return true;
}

void VirtualAudioDriver::free()
{
    if (ivars != nullptr) {
        ivars->workQueue.reset();
        ivars->audioDevice.reset();
    }
    IOSafeDeleteNULL(ivars, VirtualAudioDriver_IVars, 1);
    super::free();
}

kern_return_t VirtualAudioDriver::Start_Impl(IOService* provider)
{
    LAB_LOG("Start_Impl entering");
    kern_return_t kr = Start(provider, SUPERDISPATCH);
    if (kr != kIOReturnSuccess) {
        LAB_LOG("Start super failed with 0x%{public}08x", kr);
        return kr;
    }
    
    ivars->workQueue = GetWorkQueue();
    if (ivars->workQueue.get() == nullptr) {
        LAB_LOG("GetWorkQueue returned null");
        return kIOReturnInvalid;
    }
    
    LAB_LOG("Allocating VirtualAudioDevice");
    ivars->audioDevice = OSSharedPtr(OSTypeAlloc(VirtualAudioDevice), OSNoRetain);
    if (!ivars->audioDevice) {
        LAB_LOG("Failed to allocate VirtualAudioDevice");
        return kIOReturnNoMemory;
    }
    
    auto deviceUID = OSSharedPtr(OSString::withCString("VirtualADKAudioLabDevice"), OSNoRetain);
    auto modelUID = OSSharedPtr(OSString::withCString("VirtualADKAudioLabModel"), OSNoRetain);
    auto manufacturerUID = OSSharedPtr(OSString::withCString("Alexander Shabelnikov"), OSNoRetain);
    
    if (!deviceUID || !modelUID || !manufacturerUID) {
        LAB_LOG("Failed to allocate UID strings");
        return kIOReturnNoMemory;
    }
    
    LAB_LOG("Initializing VirtualAudioDevice");
    if (!ivars->audioDevice->init(this, false, deviceUID.get(), modelUID.get(), manufacturerUID.get(), 512)) {
        LAB_LOG("VirtualAudioDevice::init failed");
        return kIOReturnInternalError;
    }
    
    LAB_LOG("Setting device name");
    kr = ivars->audioDevice->SetName(deviceUID.get());
    if (kr != kIOReturnSuccess) {
        LAB_LOG("SetName failed with 0x%{public}08x", kr);
        return kr;
    }
    
    LAB_LOG("Adding device object");
    kr = AddObject(ivars->audioDevice.get());
    if (kr != kIOReturnSuccess) {
        LAB_LOG("AddObject failed with 0x%{public}08x", kr);
        return kr;
    }
    
    LAB_LOG("Registering service");
    kr = RegisterService();
    if (kr != kIOReturnSuccess) {
        LAB_LOG("RegisterService failed with 0x%{public}08x", kr);
        return kr;
    }
    
    LAB_LOG("Start_Impl completed successfully");
    return kIOReturnSuccess;
}

kern_return_t VirtualAudioDriver::Stop_Impl(IOService* provider)
{
    LAB_LOG("Stop_Impl");
    
    if (ivars->audioDevice) {
        RemoveObject(ivars->audioDevice.get());
        ivars->audioDevice.reset();
    }
    
    ivars->workQueue.reset();
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t VirtualAudioDriver::NewUserClient_Impl(uint32_t in_type, IOUserClient** out_user_client)
{
    if (in_type == kIOUserAudioDriverUserClientType) {
        return super::NewUserClient(in_type, out_user_client, SUPERDISPATCH);
    }
    return kIOReturnBadArgument;
}

kern_return_t VirtualAudioDriver::StartDevice(IOUserAudioObjectID in_object_id,
                                              IOUserAudioStartStopFlags in_flags)
{
    LAB_LOG("StartDevice 0x%{public}x", (uint32_t)in_object_id);
    
    if (!ivars->audioDevice || in_object_id != ivars->audioDevice->GetObjectID()) {
        LAB_LOG("StartDevice - unknown object id 0x%{public}x", (uint32_t)in_object_id);
        return kIOReturnBadArgument;
    }
    
    __block kern_return_t kr = kIOReturnSuccess;
    ivars->workQueue->DispatchSync(^(){
        kr = super::StartDevice(in_object_id, in_flags);
    });
    
    if (kr != kIOReturnSuccess) {
        LAB_LOG("StartDevice - super::StartDevice failed with 0x%{public}08x", kr);
    }
    return kr;
}

kern_return_t VirtualAudioDriver::StopDevice(IOUserAudioObjectID in_object_id,
                                             IOUserAudioStartStopFlags in_flags)
{
    LAB_LOG("StopDevice 0x%{public}x", (uint32_t)in_object_id);
    
    if (!ivars->audioDevice || in_object_id != ivars->audioDevice->GetObjectID()) {
        LAB_LOG("StopDevice - unknown object id 0x%{public}x", (uint32_t)in_object_id);
        return kIOReturnBadArgument;
    }
    
    __block kern_return_t kr = kIOReturnSuccess;
    ivars->workQueue->DispatchSync(^(){
        kr = super::StopDevice(in_object_id, in_flags);
    });
    
    if (kr != kIOReturnSuccess) {
        LAB_LOG("StopDevice - super::StopDevice failed with 0x%{public}08x", kr);
    }
    return kr;
}
