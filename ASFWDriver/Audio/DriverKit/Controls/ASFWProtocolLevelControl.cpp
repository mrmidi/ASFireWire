//
// ASFWProtocolLevelControl.cpp
// ASFWDriver
//
// Protocol-routed IOUserAudioLevelControl implementation.
//
// Host changes are optimistic (documentation/AUDIO_BACKENDS_CONTROLS.md §5.3): the control
// takes the new value at once and the device write is queued behind it. The protocol
// coalesces those writes, so a slider drag or a held volume key never blocks this queue
// on a bus round trip.
//

#include "ASFWProtocolLevelControl.h"
#include "ASFWAudioDriver.h"
#include "../../../Logging/Logging.hpp"

#include <DriverKit/IOLib.h>
#include <DriverKit/OSMetaClass.h>

namespace {

[[nodiscard]] float ClampDecibels(float value, float minDb, float maxDb) noexcept {
    if (!(value > minDb)) { // also catches NaN
        return minDb;
    }
    return value > maxDb ? maxDb : value;
}

void ForwardToProtocol(ASFWProtocolLevelControl_IVars* iv, float decibels) {
    if (!iv || !iv->ownerDriver) {
        return;
    }
    const kern_return_t status = iv->ownerDriver->ApplyProtocolLevelControl(
        iv->classIdFourCC, iv->scopeFourCC, iv->routedElement, decibels);
    if (status != kIOReturnSuccess) {
        ASFW_LOG(Audio,
                 "ASFWProtocolLevelControl: apply failed class=0x%08x scope=0x%08x element=%u "
                 "centiDb=%d status=0x%x",
                 iv->classIdFourCC,
                 iv->scopeFourCC,
                 iv->routedElement,
                 static_cast<int>(decibels * 100.0f),
                 status);
    }
}

} // namespace

OSSharedPtr<ASFWProtocolLevelControl> ASFWProtocolLevelControl::Create(
    ASFWAudioDriver* ownerDriver,
    bool isSettable,
    float initialDecibels,
    float minDecibels,
    float maxDecibels,
    IOUserAudioObjectPropertyElement controlElement,
    IOUserAudioObjectPropertyScope controlScope,
    IOUserAudioClassID controlClassID,
    uint32_t classIdFourCC, // NOLINT(bugprone-easily-swappable-parameters)
    uint32_t scopeFourCC,
    uint32_t routedElement)
{
    auto* control = OSTypeAlloc(ASFWProtocolLevelControl);
    if (!control) {
        return nullptr;
    }

    if (!control->init(ownerDriver,
                       isSettable,
                       initialDecibels,
                       minDecibels,
                       maxDecibels,
                       controlElement,
                       controlScope,
                       controlClassID,
                       classIdFourCC,
                       scopeFourCC,
                       routedElement)) {
        control->release();
        return nullptr;
    }

    return OSSharedPtr(control, OSNoRetain);
}

// The property signature mirrors IOUserAudio property routing inputs.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool ASFWProtocolLevelControl::init(
    ASFWAudioDriver* ownerDriver,
    bool isSettable,
    float initialDecibels,
    float minDecibels,
    float maxDecibels,
    IOUserAudioObjectPropertyElement controlElement,
    IOUserAudioObjectPropertyScope controlScope,
    IOUserAudioClassID controlClassID,
    uint32_t classIdFourCC, // NOLINT(bugprone-easily-swappable-parameters)
    uint32_t scopeFourCC,
    uint32_t routedElement)
{
    if (!ownerDriver || !(maxDecibels > minDecibels)) {
        return false;
    }

    const IOUserAudioLevelControlRange range{.m_min = minDecibels, .m_max = maxDecibels};
    if (!super::init(ownerDriver,
                     isSettable,
                     ClampDecibels(initialDecibels, minDecibels, maxDecibels),
                     range,
                     controlElement,
                     controlScope,
                     controlClassID)) {
        return false;
    }

    ivars = IONewZero(ASFWProtocolLevelControl_IVars, 1);
    if (!ivars) {
        return false;
    }

    ivars->ownerDriver = ownerDriver;
    ivars->classIdFourCC = classIdFourCC;
    ivars->scopeFourCC = scopeFourCC;
    ivars->routedElement = routedElement;
    ivars->minDecibels = minDecibels;
    ivars->maxDecibels = maxDecibels;
    return true;
}

void ASFWProtocolLevelControl::free()
{
    if (ivars) {
        IOSafeDeleteNULL(ivars, ASFWProtocolLevelControl_IVars, 1);
    }
    super::free();
}

kern_return_t ASFWProtocolLevelControl::HandleChangeDecibelValue(float in_decibel_value)
{
    if (!ivars || !ivars->ownerDriver) {
        return kIOReturnNotReady;
    }
    const float decibels = ClampDecibels(in_decibel_value, ivars->minDecibels, ivars->maxDecibels);
    const kern_return_t status = SetDecibelValue(decibels);
    if (status != kIOReturnSuccess) {
        return status;
    }
    ForwardToProtocol(ivars, decibels);
    return kIOReturnSuccess;
}

kern_return_t ASFWProtocolLevelControl::HandleChangeScalarValue(float in_scalar_value)
{
    // The volume keys and the Sound settings slider arrive here.
    if (!ivars || !ivars->ownerDriver) {
        return kIOReturnNotReady;
    }
    const kern_return_t status = SetScalarValue(in_scalar_value);
    if (status != kIOReturnSuccess) {
        return status;
    }
    // Convert with the control's own transfer function, so the dB the device receives is
    // the dB this control reports. Any other curve would make the device's echo of the new
    // level disagree with the slider and move it.
    ForwardToProtocol(ivars, _GetDecibelFromScalarValue(in_scalar_value));
    return kIOReturnSuccess;
}
