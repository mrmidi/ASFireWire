#pragma once

#include "ASFWAudioDriver.h"
#include "ASFWProtocolBooleanControl.h"
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
    float& outInitialDecibels);

} // namespace ASFW::Isoch::Audio
