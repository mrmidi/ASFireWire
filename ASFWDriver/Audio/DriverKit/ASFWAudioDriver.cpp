//
// ASFWAudioDriver.cpp
// ASFWDriver
//
// AudioDriverKit driver core. Focused lifecycle, graph, IO, direct-memory, and
// zero-timestamp helpers live in neighboring ASFWAudioDriver*.cpp modules.
//

#include "ASFWAudioDriverPrivate.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>

#include <new>

bool ASFWAudioDriver::init()
{
    bool result = super::init();
    if (!result) {
        ASFW_LOG(Audio, "ASFWAudioDriver: super::init() failed");
        return false;
    }

    auto* ivarStorage = IONewZero(ASFWAudioDriver_IVars, 1);
    if (!ivarStorage) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to allocate ivars");
        return false;
    }
    ivars = ::new (ivarStorage) ASFWAudioDriver_IVars{};

    ASFW::Audio::DriverKit::ResetDeviceStateFromDefaultConfig(*ivars);
    ASFW_LOG(Audio, "ASFWAudioDriver: init() succeeded");
    return true;
}

void ASFWAudioDriver::free()
{
    ASFW_LOG(Audio, "ASFWAudioDriver: free()");

    if (ivars) {
        ASFW::Audio::DriverKit::UnbindDirectAudioSkeleton(*ivars);

        ivars->device.audioNub = nullptr;
        ivars->device.boolControlCount = 0;
        ASFW::Isoch::Audio::ResetBoolControlSlots(ivars->device.boolControls,
                                                  ASFW::Isoch::Audio::kMaxBoolControls);

        ivars->outputStream.reset();
        ivars->inputStream.reset();
        ivars->outputMap.reset();
        ivars->inputMap.reset();
        ivars->controlMap.reset();
        ivars->outputBuffer.reset();
        ivars->inputBuffer.reset();
        ivars->controlBuffer.reset();
        ivars->audioDevice.reset();
        ivars->workQueue.reset();
        ivars->~ASFWAudioDriver_IVars();
        IOSafeDeleteNULL(ivars, ASFWAudioDriver_IVars, 1);
    }

    super::free();
}

kern_return_t IMPL(ASFWAudioDriver, NewUserClient)
{
    ASFW_LOG(Audio, "ASFWAudioDriver: NewUserClient(type=%u)", in_type);

    if (!out_user_client) {
        return kIOReturnBadArgument;
    }
    *out_user_client = nullptr;

    if (in_type == kIOUserAudioDriverUserClientType) {
        return super::NewUserClient(in_type, out_user_client, SUPERDISPATCH);
    }

    return kIOReturnBadArgument;
}
