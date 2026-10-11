#pragma once

#include "ASFWAudioDriver.h"
#include "ASFWHostLevelControl.h"
#include "ASFWProtocolBooleanControl.h"
#include "../Config/AudioDriverConfig.hpp"

#include <AudioDriverKit/AudioDriverKit.h>
#include <DriverKit/OSSharedPtr.h>

namespace ASFW::Isoch::Audio {

struct BoolControlSlot {
    BoolControlDescriptor descriptor{};
    bool valid{false};
    OSSharedPtr<ASFWProtocolBooleanControl> control{};
};

inline void ResetBoolControlSlots(BoolControlSlot* slots, uint32_t count) {
    if (!slots) {
        return;
    }
    for (uint32_t index = 0; index < count; ++index) {
        slots[index].control.reset();
        slots[index].valid = false;
    }
}

[[nodiscard]] kern_return_t AddBooleanControlsToDevice(
    ASFWAudioDriver& driver,
    IOUserAudioDevice& audioDevice,
    BoolControlSlot* slots,
    uint32_t slotCount);

/// Publish the master output volume realised in the host: the control drives the dext's
/// PCM gain (HostOutputGain) and the device receives no command. Starts at 0 dB, so a
/// device plays exactly as loud as before until the user turns it down.
[[nodiscard]] kern_return_t AddHostOutputVolumeToDevice(
    ASFWAudioDriver& driver,
    IOUserAudioDevice& audioDevice,
    OSSharedPtr<ASFWHostLevelControl>& outControl,
    float& outInitialDecibels,
    float& outMinDecibels);

/// Routing key of the host-realised master output mute. The HAL sees an ordinary mute
/// control; the key differs from 'mute' so the control does not take the AV/C mute path,
/// and the driver applies it to the PCM gain rather than the device.
inline constexpr uint32_t kHostOutputMuteRoute = static_cast<uint32_t>('hmut');

/// Publish the mute beside the host-realised master volume -- what the mute key toggles.
/// Starts unmuted. Like the volume, the device receives no command.
[[nodiscard]] kern_return_t AddHostOutputMuteToDevice(
    ASFWAudioDriver& driver,
    IOUserAudioDevice& audioDevice,
    OSSharedPtr<ASFWProtocolBooleanControl>& outControl);

} // namespace ASFW::Isoch::Audio
