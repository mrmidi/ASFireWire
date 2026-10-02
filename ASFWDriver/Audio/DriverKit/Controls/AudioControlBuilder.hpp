#pragma once

#include "ASFWAudioDriver.h"
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

} // namespace ASFW::Isoch::Audio
