// SPDX-License-Identifier: Apache-2.0
#include "ASFWParameterBooleanControl.h"
#include "ASFWAudioDriver.h"
#include <DriverKit/IOLib.h>
#include <DriverKit/OSMetaClass.h>
#include <DriverKit/OSString.h>
#include <bit>
#include <algorithm>
#include <cmath>
#include <new>
using namespace ASFW::Audio::Controls;
OSSharedPtr<ASFWParameterBooleanControl> ASFWParameterBooleanControl::Create(ASFWAudioDriver* driver, CoreAudioControl descriptor) {
    auto* control = OSTypeAlloc(ASFWParameterBooleanControl);
    if (!control) return nullptr;
    if (!control->init(driver, std::move(descriptor))) { control->release(); return nullptr; }
    return OSSharedPtr(control, OSNoRetain);
}
bool ASFWParameterBooleanControl::init(ASFWAudioDriver* driver, CoreAudioControl descriptor) {
    if (!driver) return false;
    if (!super::init(driver, descriptor.settable, std::get<bool>(descriptor.value), descriptor.element,
        static_cast<IOUserAudioObjectPropertyScope>(descriptor.scope),
        static_cast<IOUserAudioClassID>(descriptor.classId))) return false;
    auto* storage = IONewZero(ASFWParameterBooleanControl_IVars, 1);
    if (!storage) return false;
    ivars = ::new (storage) ASFWParameterBooleanControl_IVars{driver, std::move(descriptor)};
    return true;
}
void ASFWParameterBooleanControl::free() {
    if (ivars) {
        ivars->~ASFWParameterBooleanControl_IVars();
        IOSafeDeleteNULL(ivars, ASFWParameterBooleanControl_IVars, 1);
    }
    super::free();
}
kern_return_t ASFWParameterBooleanControl::HandleChangeControlValue(bool value) {
    if (!ivars || !ivars->descriptor.settable) return kIOReturnNotWritable;
    uint64_t kind{}, bits{};
    const auto status = ivars->driver->ApplyControlParameter(ivars->descriptor.parameter.value, 0, value, &kind, &bits);
    if (status != kIOReturnSuccess) return status;
    if (kind != 0 || bits > 1) return kIOReturnError;
    return SetControlValue(bits != 0);
}
