// SPDX-License-Identifier: Apache-2.0
#include "ASFWParameterLevelControl.h"
#include "ASFWAudioDriver.h"
#include <DriverKit/IOLib.h>
#include <DriverKit/OSMetaClass.h>
#include <DriverKit/OSString.h>
#include <DriverKit/OSData.h>
#include <bit>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
using namespace ASFW::Audio::Controls;
OSSharedPtr<ASFWParameterLevelControl> ASFWParameterLevelControl::Create(ASFWAudioDriver* driver, CoreAudioControl descriptor) {
    auto* control = OSTypeAlloc(ASFWParameterLevelControl);
    if (!control) return nullptr;
    if (!control->init(driver, std::move(descriptor))) { control->release(); return nullptr; }
    return OSSharedPtr(control, OSNoRetain);
}
bool ASFWParameterLevelControl::init(ASFWAudioDriver* driver, CoreAudioControl descriptor) {
    if (!driver) return false;
    const auto& range = std::get<ASFW::AudioModel::ScalarDomain>(descriptor.domain);
    const float initial = static_cast<float>(descriptor.normalized ? *ScalarToPosition(range, std::get<double>(descriptor.value)) : std::get<double>(descriptor.value));
    if (!super::init(driver, descriptor.settable, initial,
        {static_cast<float>(descriptor.normalized ? 0 : range.min), static_cast<float>(descriptor.normalized ? 1 : range.max)},
        descriptor.element, static_cast<IOUserAudioObjectPropertyScope>(descriptor.scope),
        static_cast<IOUserAudioClassID>(descriptor.classId))) return false;
    auto* storage = IONewZero(ASFWParameterLevelControl_IVars, 1);
    if (!storage) return false;
    ivars = ::new (storage) ASFWParameterLevelControl_IVars{driver, std::move(descriptor)};
    return true;
}
void ASFWParameterLevelControl::free() {
    if (ivars) {
        ivars->~ASFWParameterLevelControl_IVars();
        IOSafeDeleteNULL(ivars, ASFWParameterLevelControl_IVars, 1);
    }
    super::free();
}
kern_return_t ASFWParameterLevelControl::HandleChangeDecibelValue(float value) {
    if (!ivars || !ivars->descriptor.settable) return kIOReturnNotWritable;
    if (!std::isfinite(value)) return kIOReturnBadArgument;
    const auto& d = ivars->descriptor;
    double requested = value;
    if (!d.normalized) {
        const auto& range = std::get<ASFW::AudioModel::ScalarDomain>(d.domain);
        const auto position = ScalarToPosition(range, std::clamp(requested, range.min, range.max));
        if (!position) return kIOReturnBadArgument;
        requested = *PositionToScalar(range, *position);
    } else if (value < 0 || value > 1) return kIOReturnBadArgument;
    uint64_t kind{}, bits{};
    const auto status = ivars->driver->ApplyControlParameter(d.parameter.value, d.normalized ? 4 : 1,
        std::bit_cast<uint64_t>(requested), &kind, &bits);
    if (status != kIOReturnSuccess) return status;
    if (kind != (d.normalized ? 4u : 1u)) return kIOReturnError;
    const double confirmed = std::bit_cast<double>(bits);
    if (!std::isfinite(confirmed)) return kIOReturnError;
    return SetDecibelValue(static_cast<float>(confirmed));
}
kern_return_t ASFWParameterLevelControl::HandleChangeScalarValue(float value) {
    if (!std::isfinite(value) || value < 0 || value > 1) return kIOReturnBadArgument;
    // Preserve ADK's established output-volume curve. Input gain is an own
    // normalized position over the current provider range, not amplitude.
    return HandleChangeDecibelValue(ivars && ivars->descriptor.normalized ? value : _GetDecibelFromScalarValue(value));
}

kern_return_t ASFWParameterLevelControl::_GetPropertyData(
    const IOUserAudioObjectPropertyAddress* address, OSData* qualifier, OSData** data) {
    if (ivars && ivars->descriptor.normalized && address && data) {
        const auto selector = static_cast<uint32_t>(address->mSelector);
        // AudioHardwareBase.h defines lcsv/lcsd/lcds. This control's internal
        // level and its scalar are the same normalized position.
        if (selector == 'lcsv') {
            const float position = GetDecibelValue();
            *data = OSData::withBytes(&position, sizeof(position));
            return *data ? kIOReturnSuccess : kIOReturnNoMemory;
        }

    }
    return super::_GetPropertyData(address, qualifier, data);
}
