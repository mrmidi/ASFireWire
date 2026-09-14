#pragma once

#include "ASFWAudioDriver.h"
#include "ASFWProtocolBooleanControl.h"
#include "ASFWEmulatedMuteControl.h"
#include "ASFWProtocolLevelControl.h"
#include "../Config/AudioDriverConfig.hpp"

#include <AudioDriverKit/AudioDriverKit.h>
#include <DriverKit/OSSharedPtr.h>

namespace ASFW::Isoch::Audio {

struct BoolControlSlot {
    BoolControlDescriptor descriptor{};
    bool valid{false};
    OSSharedPtr<ASFWProtocolBooleanControl> control{};
};

void ResetBoolControlSlots(BoolControlSlot* slots, uint32_t count);

[[nodiscard]] kern_return_t AddBooleanControlsToDevice(
    ASFWAudioDriver& driver,
    IOUserAudioDevice& audioDevice,
    BoolControlSlot* slots,
    uint32_t slotCount);

/// Publish the protocol's master output volume ('vlme', output, main) -- what the volume
/// keys drive -- when the device backs one. Returns kIOReturnUnsupported, having added
/// nothing, when it does not. `outInitialDecibels` is the value the control starts at.
[[nodiscard]] kern_return_t AddProtocolOutputVolumeToDevice(
    ASFWAudioDriver& driver,
    IOUserAudioDevice& audioDevice,
    OSSharedPtr<ASFWProtocolLevelControl>& outControl,
    float& outInitialDecibels,
    float& outMinDecibels);

/// Publish an output mute standing in for hardware that has none: it writes the minimum
/// level and restores the volume control's level on unmute (Runtime/OutputMutePolicy.hpp).
/// Only meaningful for a device that already has the output volume control.
[[nodiscard]] kern_return_t AddEmulatedOutputMuteToDevice(
    ASFWAudioDriver& driver,
    IOUserAudioDevice& audioDevice,
    OSSharedPtr<ASFWEmulatedMuteControl>& outControl);

} // namespace ASFW::Isoch::Audio
