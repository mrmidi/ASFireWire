//
// ASFWProtocolBooleanControl.cpp
// ASFWDriver
//
// Protocol-routed IOUserAudioBooleanControl implementation.
//

#include "ASFWProtocolBooleanControl.h"
#include "ASFWAudioDriver.h"
#include "../../../Logging/Logging.hpp"

#include <DriverKit/IOLib.h>
#include <DriverKit/OSMetaClass.h>

OSSharedPtr<ASFWProtocolBooleanControl> ASFWProtocolBooleanControl::Create(
    ASFWAudioDriver* ownerDriver,
    bool isSettable,
    bool controlValue,
    IOUserAudioObjectPropertyElement controlElement,
    IOUserAudioObjectPropertyScope controlScope,
    IOUserAudioClassID controlClassID,
    uint32_t classIdFourCC, // NOLINT(bugprone-easily-swappable-parameters)
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

// The property signature mirrors IOUserAudio property routing inputs.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool ASFWProtocolBooleanControl::init(
    ASFWAudioDriver* ownerDriver,
    bool isSettable,
    bool controlValue,
    IOUserAudioObjectPropertyElement controlElement,
    IOUserAudioObjectPropertyScope controlScope,
    IOUserAudioClassID controlClassID,
    uint32_t classIdFourCC, // NOLINT(bugprone-easily-swappable-parameters)
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
    ASFW_LOG(Audio, "[AvcControl] init boolean class=0x%08x route=0x%06x scope=0x%08x element=%u value=%u settable=%u", classIdFourCC, routedElement, static_cast<uint32_t>(controlScope), controlElement, controlValue, isSettable);
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
    ASFW_LOG(Audio, "[AvcControl] callback boolean class=0x%08x route=0x%06x requested=%u ready=%u", ivars ? ivars->classIdFourCC : 0, ivars ? ivars->routedElement : 0, in_control_value, ivars && ivars->ownerDriver);
    if (!ivars || !ivars->ownerDriver) {
        return kIOReturnNotReady;
    }

    if (ivars->classIdFourCC == static_cast<uint32_t>(IOUserAudioClassID::MuteControl)) {
        int32_t confirmed{};
        const auto status = ivars->ownerDriver->ApplyAvcFeatureControl(ivars->routedElement, true,
                                                                      in_control_value ? 1 : 0, &confirmed);
        const auto halStatus = status == kIOReturnSuccess ? SetControlValue(confirmed != 0) : status;
        ASFW_LOG(Audio, "[AvcControl] mute complete token=0x%06x confirmed=%d applyKr=0x%x halKr=0x%x", ivars->routedElement, confirmed, status, halStatus);
        return halStatus;
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

    const auto status = SetControlValue(in_control_value);
    ASFW_LOG(Audio, "[AvcControl] boolean complete class=0x%08x route=0x%06x kr=0x%x", ivars->classIdFourCC, ivars->routedElement, status);
    return status;
}
