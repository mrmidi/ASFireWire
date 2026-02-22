//
// ASFWProtocolBooleanControl.cpp
// ASFWDriver
//
// Protocol-routed IOUserAudioBooleanControl implementation.
//

#include "ASFWProtocolBooleanControl.h"
#include "ASFWAudioDriver.h"
#include "../../Logging/Logging.hpp"

#include <DriverKit/IOLib.h>
#include <DriverKit/OSMetaClass.h>

OSSharedPtr<ASFWProtocolBooleanControl> ASFWProtocolBooleanControl::Create(
    ASFWAudioDriver* ownerDriver,
    bool isSettable,
    bool controlValue,
    IOUserAudioObjectPropertyElement controlElement,
    IOUserAudioObjectPropertyScope controlScope,
    IOUserAudioClassID controlClassID,
    uint32_t classIdFourCC,
    uint32_t routedElement)
{
    auto* control = OSTypeAlloc(ASFWProtocolBooleanControl);
    if (!control) {
        return nullptr;
    }

    if (!control->init(ownerDriver,
                       isSettable,
                       controlValue,
                       controlElement,
                       controlScope,
                       controlClassID,
                       classIdFourCC,
                       routedElement)) {
        control->release();
        return nullptr;
    }

    return OSSharedPtr(control, OSNoRetain);
}

bool ASFWProtocolBooleanControl::init(
    ASFWAudioDriver* ownerDriver,
    bool isSettable,
    bool controlValue,
    IOUserAudioObjectPropertyElement controlElement,
    IOUserAudioObjectPropertyScope controlScope,
    IOUserAudioClassID controlClassID,
    uint32_t classIdFourCC,
    uint32_t routedElement)
{
    if (!ownerDriver) {
        return false;
    }

    if (!super::init(ownerDriver,
                     isSettable,
                     controlValue,
                     controlElement,
                     controlScope,
                     controlClassID)) {
        return false;
    }

    ivars = IONewZero(ASFWProtocolBooleanControl_IVars, 1);
    if (!ivars) {
        return false;
    }

    ivars->ownerDriver = ownerDriver;
    ivars->classIdFourCC = classIdFourCC;
    ivars->routedElement = routedElement;
    return true;
}

void ASFWProtocolBooleanControl::free()
{
    if (ivars) {
        IOSafeDeleteNULL(ivars, ASFWProtocolBooleanControl_IVars, 1);
    }
    super::free();
}

kern_return_t ASFWProtocolBooleanControl::HandleChangeControlValue(bool in_control_value)
{
    if (!ivars || !ivars->ownerDriver) {
        return kIOReturnNotReady;
    }

    const kern_return_t applyStatus =
        ivars->ownerDriver->ApplyProtocolBooleanControl(ivars->classIdFourCC,
                                                        ivars->routedElement,
                                                        in_control_value);
    if (applyStatus != kIOReturnSuccess) {
        ASFW_LOG(Audio,
                 "ASFWProtocolBooleanControl: apply failed class=0x%08x element=%u value=%u status=0x%x",
                 ivars->classIdFourCC,
                 ivars->routedElement,
                 in_control_value ? 1u : 0u,
                 applyStatus);
        return applyStatus;
    }

    return SetControlValue(in_control_value);
}
