// SPDX-License-Identifier: Apache-2.0
#include "ASFWAvcLevelControl.h"
#include "ASFWAudioDriver.h"
#include "../../../Logging/Logging.hpp"
#include <DriverKit/IOLib.h>
#include <DriverKit/OSMetaClass.h>

OSSharedPtr<ASFWAvcLevelControl> ASFWAvcLevelControl::Create(ASFWAudioDriver* driver,
    ASFW::Audio::Model::AvcPublishedControl descriptor) {
    auto* control = OSTypeAlloc(ASFWAvcLevelControl);
    if (!control) return nullptr;
    if (!control->init(driver, descriptor)) { control->release(); return nullptr; }
    return OSSharedPtr(control, OSNoRetain);
}
bool ASFWAvcLevelControl::init(ASFWAudioDriver* driver,
    ASFW::Audio::Model::AvcPublishedControl descriptor) {
    using ASFW::Audio::Model::AvcVolumeRange;
    if (!driver || !descriptor.hasVolume || !descriptor.range.Valid()) return false;
    if (!super::init(driver, true, AvcVolumeRange::Decibels(descriptor.current),
        {AvcVolumeRange::Decibels(descriptor.range.minimum), AvcVolumeRange::Decibels(descriptor.range.maximum)},
        descriptor.element, static_cast<IOUserAudioObjectPropertyScope>(descriptor.scope), IOUserAudioClassID::VolumeControl)) return false;
    ivars = IONewZero(ASFWAvcLevelControl_IVars, 1);
    if (!ivars) return false;
    ivars->driver = driver; ivars->descriptor = descriptor;
    ASFW_LOG(Audio, "[AvcControl] init volume token=0x%06x scope=0x%08x element=%u currentRaw=%d minRaw=%d maxRaw=%d stepRaw=%d source=discovery", descriptor.token, descriptor.scope, descriptor.element, descriptor.current, descriptor.range.minimum, descriptor.range.maximum, descriptor.range.resolution);
    return true;
}
void ASFWAvcLevelControl::free() {
    if (ivars) IOSafeDeleteNULL(ivars, ASFWAvcLevelControl_IVars, 1);
    super::free();
}
kern_return_t ASFWAvcLevelControl::HandleChangeDecibelValue(float value) {
    ASFW_LOG(Audio, "[AvcControl] callback volume-db token=0x%06x requestedDb=%f ready=%u", ivars ? ivars->descriptor.token : 0, value, ivars != nullptr);
    if (!ivars) return kIOReturnNotReady;
    const auto raw = ivars->descriptor.range.Quantize(value);
    if (!raw) { ASFW_LOG(Audio, "[AvcControl] volume-db invalid token=0x%06x kr=0x%x", ivars->descriptor.token, kIOReturnBadArgument); return kIOReturnBadArgument; }
    ASFW_LOG(Audio, "[AvcControl] volume-db quantized token=0x%06x raw=%d", ivars->descriptor.token, *raw);
    int32_t confirmed{};
    const auto result = ivars->driver->ApplyAvcFeatureControl(ivars->descriptor.token, false, *raw, &confirmed);
    if (result != kIOReturnSuccess) { ASFW_LOG(Audio, "[AvcControl] volume-db failed token=0x%06x kr=0x%x", ivars->descriptor.token, result); return result; }
    if (confirmed == INT16_MIN || confirmed < ivars->descriptor.range.minimum || confirmed > ivars->descriptor.range.maximum) {
        ASFW_LOG(Audio, "[AvcControl] volume-db invalid readback token=0x%06x confirmedRaw=%d kr=0x%x", ivars->descriptor.token, confirmed, kIOReturnError);
        return kIOReturnError;
    }
    const auto status = SetDecibelValue(ASFW::Audio::Model::AvcVolumeRange::Decibels(static_cast<int16_t>(confirmed)));
    ASFW_LOG(Audio, "[AvcControl] volume-db complete token=0x%06x confirmedRaw=%d kr=0x%x", ivars->descriptor.token, confirmed, status);
    return status;
}
kern_return_t ASFWAvcLevelControl::HandleChangeScalarValue(float value) {
    ASFW_LOG(Audio, "[AvcControl] callback volume-scalar token=0x%06x requestedScalar=%f", ivars ? ivars->descriptor.token : 0, value);
    if (!std::isfinite(value) || value < 0 || value > 1) {
        ASFW_LOG(Audio, "[AvcControl] volume-scalar invalid kr=0x%x", kIOReturnBadArgument);
        return kIOReturnBadArgument;
    }
    // Use ADK's own scalar/decibel conversion, exposed by the SDK's generated
    // IOUserAudioLevelControl interface. HAL readback and writes share one curve.
    // Scalar is a normalized slider position, not an AV/C wire value.
    return HandleChangeDecibelValue(_GetDecibelFromScalarValue(value));
}
