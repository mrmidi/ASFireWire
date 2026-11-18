// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioDriverKitCompat.hpp - AudioDriverKit setters that only exist on DriverKit 25+.
//
// DriverKit 25 (macOS 26) added IOUserAudioDevice::SetWantsStreamFormatsRestored and
// IOUserAudioClockDevice::SetWantsControlsRestored. The DriverKit 25.1 SDK headers carry
// no availability annotation for them, so with DRIVERKIT_DEPLOYMENT_TARGET 24.0 the linker
// would bind both strongly and dyld would refuse to load the dext on macOS 15 (DriverKit 24).
//
// Reference them through weak imports of their exported (Itanium-mangled) names instead,
// and call them only when the running DriverKit provides them. Both are plain non-virtual
// members (see IOUserAudioDevice.iig / IOUserAudioClockDevice.iig), so the object pointer is
// passed as the first argument; the typed `self` parameter lets the compiler apply the base
// class adjustment when it converts from the concrete device type.
//
// Without them (DriverKit 24) the macOS 15 host keeps its own default behaviour; issue #246
// reports that stream formats and control values are then not persisted across reconnects.

#pragma once

#include <AudioDriverKit/IOUserAudioClockDevice.h>
#include <AudioDriverKit/IOUserAudioDevice.h>

namespace ASFW::Audio::ADKCompat {

void SetWantsStreamFormatsRestoredImpl(IOUserAudioDevice* self, bool wants)
    __asm__("__ZN17IOUserAudioDevice29SetWantsStreamFormatsRestoredEb") __attribute__((weak_import));

void SetWantsControlsRestoredImpl(IOUserAudioClockDevice* self, bool wants)
    __asm__("__ZN22IOUserAudioClockDevice24SetWantsControlsRestoredEb") __attribute__((weak_import));

/// Calls IOUserAudioDevice::SetWantsStreamFormatsRestored when the running DriverKit has it.
/// @return true if the setter was available and called.
[[nodiscard]] inline bool SetWantsStreamFormatsRestored(IOUserAudioDevice* device, bool wants) {
    if (device == nullptr || &SetWantsStreamFormatsRestoredImpl == nullptr) {
        return false;
    }
    SetWantsStreamFormatsRestoredImpl(device, wants);
    return true;
}

/// Calls IOUserAudioClockDevice::SetWantsControlsRestored when the running DriverKit has it.
/// @return true if the setter was available and called.
[[nodiscard]] inline bool SetWantsControlsRestored(IOUserAudioClockDevice* device, bool wants) {
    if (device == nullptr || &SetWantsControlsRestoredImpl == nullptr) {
        return false;
    }
    SetWantsControlsRestoredImpl(device, wants);
    return true;
}

} // namespace ASFW::Audio::ADKCompat
