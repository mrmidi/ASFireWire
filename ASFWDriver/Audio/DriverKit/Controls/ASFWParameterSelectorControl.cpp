// SPDX-License-Identifier: Apache-2.0
#include "ASFWParameterSelectorControl.h"
#include "ASFWAudioDriver.h"
#include <DriverKit/IOLib.h>
#include <DriverKit/OSMetaClass.h>
#include <DriverKit/OSString.h>
#include <bit>
#include <algorithm>
#include <cmath>
#include <new>
#include "../../../Logging/Logging.hpp"
using namespace ASFW::Audio::Controls;
OSSharedPtr<ASFWParameterSelectorControl> ASFWParameterSelectorControl::Create(ASFWAudioDriver* driver, CoreAudioControl descriptor) {
    auto* control = OSTypeAlloc(ASFWParameterSelectorControl);
    if (!control) return nullptr;
    if (!control->init(driver, std::move(descriptor))) { control->release(); return nullptr; }
    return OSSharedPtr(control, OSNoRetain);
}
bool ASFWParameterSelectorControl::init(ASFWAudioDriver* driver, CoreAudioControl descriptor) {
    if (!driver) return false;
    if (!super::init(driver, descriptor.settable, descriptor.element,
        static_cast<IOUserAudioObjectPropertyScope>(descriptor.scope),
        static_cast<IOUserAudioClassID>(descriptor.classId))) return false;
    auto* storage = IONewZero(ASFWParameterSelectorControl_IVars, 1);
    if (!storage) return false;
    ivars = ::new (storage) ASFWParameterSelectorControl_IVars{driver, std::move(descriptor)};
    std::vector<IOUserAudioSelectorValueDescription> descriptions;
    for (const auto& item : std::get<ASFW::AudioModel::EnumDomain>(ivars->descriptor.domain).values) {
        // HAL selectors are uint32; two's-complement preserves signed enum values.
        if (item.value < INT32_MIN || item.value > UINT32_MAX) return false;
        auto name = OSSharedPtr(OSString::withCString(item.name.c_str()), OSNoRetain);
        if (!name) return false;
        descriptions.push_back({static_cast<uint32_t>(item.value), std::move(name)});
    }
    if (AddControlValueDescriptions(descriptions.data(), descriptions.size()) != kIOReturnSuccess) return false;
    const uint32_t initial = static_cast<uint32_t>(std::get<int64_t>(ivars->descriptor.value));
    if (SetCurrentSelectedValues(&initial, 1) != kIOReturnSuccess) return false;
    return true;
}
void ASFWParameterSelectorControl::free() {
    ASFW_LOG(Audio, "[Lifecycle] ASFWParameterSelectorControl free obj=%p", this);
    if (ivars) {
        ivars->~ASFWParameterSelectorControl_IVars();
        IOSafeDeleteNULL(ivars, ASFWParameterSelectorControl_IVars, 1);
    }
    super::free();
}
kern_return_t ASFWParameterSelectorControl::HandleChangeSelectedValues(const IOUserAudioSelectorValue* values, size_t count) {
    if (!ivars || !ivars->descriptor.settable) return kIOReturnNotWritable;
    if (!values || count != 1) return kIOReturnBadArgument;
    const auto& items = std::get<ASFW::AudioModel::EnumDomain>(ivars->descriptor.domain).values;
    const auto item = std::ranges::find_if(items, [&](const auto& item) { return static_cast<uint32_t>(item.value) == values[0]; });
    if (item == items.end()) return kIOReturnBadArgument;
    uint64_t kind{}, bits{};
    const auto status = ivars->driver->ApplyControlParameter(ivars->descriptor.parameter.value, 2,
        static_cast<uint64_t>(item->value), &kind, &bits);
    if (status != kIOReturnSuccess) return status;
    if (kind != 2) return kIOReturnError;
    const auto confirmed = static_cast<uint32_t>(bits);
    return SetCurrentSelectedValues(&confirmed, 1);
}
