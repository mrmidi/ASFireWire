#include <new> // first, before DriverKit headers (libc++ placement-new clash)
#include <DriverKit/IOLib.h>
#include <DriverKit/OSData.h>
#include <os/log.h>

#include "LabDiagUserClient.h"
#include "VirtualAudioDriver.h"
#include "VirtualAudioDevice.h"

#include "../Lab/PacketDumpBlob.hpp"

#define LAB_LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "[ADKLab] " fmt, ##__VA_ARGS__)

struct LabDiagUserClient_IVars
{
    VirtualAudioDriver* driver{nullptr}; // borrowed: our provider, retained by IOKit attach
};

bool LabDiagUserClient::init()
{
    if (!super::init()) {
        return false;
    }
    ivars = IONewZero(LabDiagUserClient_IVars, 1);
    return ivars != nullptr;
}

void LabDiagUserClient::free()
{
    IOSafeDeleteNULL(ivars, LabDiagUserClient_IVars, 1);
    super::free();
}

kern_return_t LabDiagUserClient::Start_Impl(IOService* provider)
{
    kern_return_t kr = Start(provider, SUPERDISPATCH);
    if (kr != kIOReturnSuccess) {
        return kr;
    }
    ivars->driver = OSDynamicCast(VirtualAudioDriver, provider);
    if (ivars->driver == nullptr) {
        LAB_LOG("LabDiagUserClient::Start - provider is not VirtualAudioDriver");
        Stop(provider, SUPERDISPATCH);
        return kIOReturnBadArgument;
    }
    LAB_LOG("LabDiagUserClient started");
    return kIOReturnSuccess;
}

kern_return_t LabDiagUserClient::Stop_Impl(IOService* provider)
{
    ivars->driver = nullptr;
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t LabDiagUserClient::ExternalMethod(
    uint64_t selector, IOUserClientMethodArguments* arguments,
    const IOUserClientMethodDispatch* dispatch, OSObject* target,
    void* reference)
{
    if (arguments == nullptr || ivars == nullptr || ivars->driver == nullptr) {
        return kIOReturnNotReady;
    }

    switch (selector) {
    case ASFW::Lab::kLabDiagSelectorDumpPackets: {
        uint32_t count = ASFW::Lab::kPacketDumpDefaultRecords;
        uint64_t anchor = ASFW::Lab::kPacketDumpAnchorLatest;
        if (arguments->scalarInputCount >= 1) {
            count = static_cast<uint32_t>(arguments->scalarInput[0]);
        }
        if (arguments->scalarInputCount >= 2) {
            anchor = arguments->scalarInput[1];
        }

        VirtualAudioDevice* device = ivars->driver->GetVirtualAudioDevice();
        if (device == nullptr) {
            return kIOReturnNotReady;
        }

        OSData* blob = nullptr;
        kern_return_t kr = device->CopyPacketDump(count, anchor, &blob);
        if (kr != kIOReturnSuccess) {
            return kr;
        }
        arguments->structureOutput = blob; // ownership passes to the dispatcher
        return kIOReturnSuccess;
    }
    default:
        return kIOReturnBadArgument;
    }
}
