// SPDX-License-Identifier: Apache-2.0
#include "ASFWAvcLevelControl.h"
#include "ASFWAudioDriver.h"
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
    return true;
}
void ASFWAvcLevelControl::free() {
    if (ivars) IOSafeDeleteNULL(ivars, ASFWAvcLevelControl_IVars, 1);
    super::free();
}
kern_return_t ASFWAvcLevelControl::HandleChangeDecibelValue(float value) {
    if (!ivars) return kIOReturnNotReady;
    const auto raw = ivars->descriptor.range.Quantize(value);
    if (!raw) return kIOReturnBadArgument;
    int32_t confirmed{};
    const auto result = ivars->driver->ApplyAvcFeatureControl(ivars->descriptor.token, false, *raw, &confirmed);
    if (result != kIOReturnSuccess) return result;
    if (confirmed == INT16_MIN || confirmed < ivars->descriptor.range.minimum || confirmed > ivars->descriptor.range.maximum) return kIOReturnError;
    return SetDecibelValue(ASFW::Audio::Model::AvcVolumeRange::Decibels(static_cast<int16_t>(confirmed)));
}
kern_return_t ASFWAvcLevelControl::HandleChangeScalarValue(float value) {
    if (!std::isfinite(value) || value < 0 || value > 1) return kIOReturnBadArgument;
    // Use ADK's own scalar/decibel conversion, exposed by the SDK's generated
    // IOUserAudioLevelControl interface. HAL readback and writes share one curve.
    // Scalar is a normalized slider position, not an AV/C wire value.
    return HandleChangeDecibelValue(_GetDecibelFromScalarValue(value));
}
