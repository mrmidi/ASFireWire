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
    IOLog("VirtualAudioDriver: Start_Impl entering\n");
    kern_return_t kr = Start(provider, SUPERDISPATCH);
    if (kr != kIOReturnSuccess) {
        IOLog("VirtualAudioDriver: Start super failed with 0x%08x\n", kr);
        return kr;
    }
    
    ivars->workQueue = GetWorkQueue();
    if (ivars->workQueue.get() == nullptr) {
        IOLog("VirtualAudioDriver: GetWorkQueue returned null\n");
        return kIOReturnInvalid;
    }
    
    IOLog("VirtualAudioDriver: Allocating VirtualAudioDevice\n");
    ivars->audioDevice = OSSharedPtr(OSTypeAlloc(VirtualAudioDevice), OSNoRetain);
    if (!ivars->audioDevice) {
        IOLog("VirtualAudioDriver: Failed to allocate VirtualAudioDevice\n");
        return kIOReturnNoMemory;
    }
    
    auto deviceUID = OSSharedPtr(OSString::withCString("VirtualADKAudioLabDevice"), OSNoRetain);
    auto modelUID = OSSharedPtr(OSString::withCString("VirtualADKAudioLabModel"), OSNoRetain);
    auto manufacturerUID = OSSharedPtr(OSString::withCString("Alexander Shabelnikov"), OSNoRetain);
    
    if (!deviceUID || !modelUID || !manufacturerUID) {
        IOLog("VirtualAudioDriver: Failed to allocate UID strings\n");
        return kIOReturnNoMemory;
    }
    
    IOLog("VirtualAudioDriver: Initializing VirtualAudioDevice\n");
    if (!ivars->audioDevice->init(this, false, deviceUID.get(), modelUID.get(), manufacturerUID.get(), 512)) {
        IOLog("VirtualAudioDriver: VirtualAudioDevice::init failed\n");
        return kIOReturnInternalError;
    }
    
    IOLog("VirtualAudioDriver: Setting device name\n");
    kr = ivars->audioDevice->SetName(deviceUID.get());
    if (kr != kIOReturnSuccess) {
        IOLog("VirtualAudioDriver: SetName failed with 0x%08x\n", kr);
        return kr;
    }
    
    IOLog("VirtualAudioDriver: Adding device object\n");
    kr = AddObject(ivars->audioDevice.get());
    if (kr != kIOReturnSuccess) {
        IOLog("VirtualAudioDriver: AddObject failed with 0x%08x\n", kr);
        return kr;
    }
    
    IOLog("VirtualAudioDriver: Registering service\n");
    kr = RegisterService();
    if (kr != kIOReturnSuccess) {
        IOLog("VirtualAudioDriver: RegisterService failed with 0x%08x\n", kr);
        return kr;
    }
    
    IOLog("VirtualAudioDriver: Start_Impl completed successfully\n");
    return kIOReturnSuccess;
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
