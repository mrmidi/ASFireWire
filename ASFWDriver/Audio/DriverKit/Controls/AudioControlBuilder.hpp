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

void ResetBoolControlSlots(BoolControlSlot* slots, uint32_t count);

[[nodiscard]] kern_return_t AddBooleanControlsToDevice(
    ASFWAudioDriver& driver,
    IOUserAudioDevice& audioDevice,
    BoolControlSlot* slots,
    uint32_t slotCount);

} // namespace ASFW::Isoch::Audio
