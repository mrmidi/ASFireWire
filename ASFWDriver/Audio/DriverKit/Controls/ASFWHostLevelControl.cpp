// SPDX-License-Identifier: Apache-2.0
#include "ASFWHostLevelControl.h"
#include "ASFWAudioDriver.h"
#include "../../../Logging/Logging.hpp"
#include <DriverKit/IOLib.h>
#include <DriverKit/OSMetaClass.h>

OSSharedPtr<ASFWHostLevelControl> ASFWHostLevelControl::Create(ASFWAudioDriver* driver,
    float initialDecibels, float minDecibels, float maxDecibels) {
    auto* control = OSTypeAlloc(ASFWHostLevelControl);
    if (!control) return nullptr;
    if (!control->init(driver, initialDecibels, minDecibels, maxDecibels)) {
        control->release();
        return nullptr;
    }
    return OSSharedPtr(control, OSNoRetain);
}

bool ASFWHostLevelControl::init(ASFWAudioDriver* driver,
    float initialDecibels, float minDecibels, float maxDecibels) {
    if (!driver || !(minDecibels < maxDecibels)) return false;
    if (!super::init(driver, true, initialDecibels, {minDecibels, maxDecibels},
        IOUserAudioObjectPropertyElementMain, IOUserAudioObjectPropertyScope::Output,
        IOUserAudioClassID::VolumeControl)) return false;
    ivars = IONewZero(ASFWHostLevelControl_IVars, 1);
    if (!ivars) return false;
    ivars->driver = driver;
    ivars->minDecibels = minDecibels;
    ivars->maxDecibels = maxDecibels;
    return true;
}

void ASFWHostLevelControl::free() {
    if (ivars) IOSafeDeleteNULL(ivars, ASFWHostLevelControl_IVars, 1);
    super::free();
}

kern_return_t ASFWHostLevelControl::HandleChangeDecibelValue(float value) {
    if (!ivars) return kIOReturnNotReady;
    if (value != value) return kIOReturnBadArgument; // NaN; infinities clamp below
    const float decibels = value < ivars->minDecibels ? ivars->minDecibels
                         : value > ivars->maxDecibels ? ivars->maxDecibels
                                                      : value;
    const auto result = ivars->driver->ApplyHostOutputVolume(decibels);
    if (result != kIOReturnSuccess) {
        ASFW_LOG(Audio, "[HostVolume] apply failed kr=0x%x", result);
        return result;
    }
    return SetDecibelValue(decibels);
}

kern_return_t ASFWHostLevelControl::HandleChangeScalarValue(float value) {
    if (!(value >= 0.0f && value <= 1.0f)) return kIOReturnBadArgument; // also NaN
    // The slider position goes through ADK's own scalar/decibel curve, so HAL readback and
    // writes agree.
    return HandleChangeDecibelValue(_GetDecibelFromScalarValue(value));
}
