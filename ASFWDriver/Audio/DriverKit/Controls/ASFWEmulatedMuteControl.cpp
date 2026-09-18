//
// ASFWEmulatedMuteControl.cpp
// ASFWDriver
//
// See the .iig, and Runtime/OutputMutePolicy.hpp for the behaviour this stands in for.
//

#include "ASFWEmulatedMuteControl.h"
#include "ASFWAudioDriver.h"
#include "../../../Logging/Logging.hpp"

#include <DriverKit/IOLib.h>
#include <DriverKit/OSMetaClass.h>

OSSharedPtr<ASFWEmulatedMuteControl> ASFWEmulatedMuteControl::Create(
    ASFWAudioDriver* ownerDriver,
    bool initialMuted,
    IOUserAudioObjectPropertyElement controlElement,
    IOUserAudioObjectPropertyScope controlScope)
{
    auto* control = OSTypeAlloc(ASFWEmulatedMuteControl);
    if (!control) {
        return nullptr;
    }
    if (!control->init(ownerDriver, initialMuted, controlElement, controlScope)) {
        control->release();
        return nullptr;
    }
    return OSSharedPtr(control, OSNoRetain);
}

bool ASFWEmulatedMuteControl::init(
    ASFWAudioDriver* ownerDriver,
    bool initialMuted,
    IOUserAudioObjectPropertyElement controlElement,
    IOUserAudioObjectPropertyScope controlScope)
{
    if (!ownerDriver) {
        return false;
    }
    if (!super::init(ownerDriver,
                     /*in_is_settable=*/true,
                     initialMuted,
                     controlElement,
                     controlScope,
                     IOUserAudioClassID::MuteControl)) {
        return false;
    }
    ivars = IONewZero(ASFWEmulatedMuteControl_IVars, 1);
    if (!ivars) {
        return false;
    }
    ivars->ownerDriver = ownerDriver;
    return true;
}

void ASFWEmulatedMuteControl::free()
{
    if (ivars) {
        IOSafeDeleteNULL(ivars, ASFWEmulatedMuteControl_IVars, 1);
    }
    super::free();
}

kern_return_t ASFWEmulatedMuteControl::HandleChangeControlValue(bool in_control_value)
{
    if (!ivars || !ivars->ownerDriver) {
        return kIOReturnNotReady;
    }
    // Take the state first, then send the level: the write is coalesced downstream, so the
    // mute key never waits on the bus.
    const kern_return_t status = SetControlValue(in_control_value);
    if (status != kIOReturnSuccess) {
        return status;
    }
    const kern_return_t applyStatus = ivars->ownerDriver->ApplyOutputMute(in_control_value);
    if (applyStatus != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWEmulatedMuteControl: apply muted=%u failed status=0x%x",
                 in_control_value ? 1U : 0U, applyStatus);
    }
    return kIOReturnSuccess;
}
