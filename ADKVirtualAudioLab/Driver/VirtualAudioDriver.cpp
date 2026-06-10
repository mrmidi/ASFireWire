#include <AudioDriverKit/AudioDriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSString.h>
#include "VirtualAudioDriver.h"
#include "VirtualAudioDevice.h"

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
    kern_return_t kr = Start(provider, SUPERDISPATCH);
    if (kr != kIOReturnSuccess) {
        return kr;
    }
    
    ivars->workQueue = GetWorkQueue();
    if (ivars->workQueue.get() == nullptr) {
        return kIOReturnInvalid;
    }
    
    IOLog("VirtualAudioDriver: Start\n");
    
    ivars->audioDevice = OSSharedPtr(OSTypeAlloc(VirtualAudioDevice), OSNoRetain);
    if (ivars->audioDevice) {
        auto deviceUID = OSSharedPtr(OSString::withCString("VirtualADKAudioLabDevice"), OSNoRetain);
        auto modelUID = OSSharedPtr(OSString::withCString("VirtualADKAudioLabModel"), OSNoRetain);
        auto manufacturerUID = OSSharedPtr(OSString::withCString("Alexander Shabelnikov"), OSNoRetain);
        
        if (ivars->audioDevice->init(this, false, deviceUID.get(), modelUID.get(), manufacturerUID.get(), 512)) {
            ivars->audioDevice->SetName(deviceUID.get());
            AddObject(ivars->audioDevice.get());
        }
    }

    kr = RegisterService();
    return kr;
}

kern_return_t VirtualAudioDriver::Stop_Impl(IOService* provider)
{
    IOLog("VirtualAudioDriver: Stop\n");
    
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
    IOLog("VirtualAudioDriver: StartDevice 0x%x\n", (uint32_t)in_object_id);
    return kIOReturnSuccess;
}

kern_return_t VirtualAudioDriver::StopDevice(IOUserAudioObjectID in_object_id,
                                             IOUserAudioStartStopFlags in_flags)
{
    IOLog("VirtualAudioDriver: StopDevice 0x%x\n", (uint32_t)in_object_id);
    return kIOReturnSuccess;
}
