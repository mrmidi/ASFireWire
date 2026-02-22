#pragma once

#include "ASFWAudioDriver.h"
#include "ASFWProtocolBooleanControl.h"
#include "AudioDriverConfig.hpp"

#include <AudioDriverKit/AudioDriverKit.h>
#include <DriverKit/OSSharedPtr.h>

namespace ASFW::Isoch::Audio {

struct BoolControlSlot {
    BoolControlDescriptor descriptor{};
    bool valid{false};
    OSSharedPtr<ASFWProtocolBooleanControl> control{};
};

void ResetBoolControlSlots(BoolControlSlot* slots, uint32_t count);

void AddBooleanControlsToDevice(ASFWAudioDriver& driver,
                                IOUserAudioDevice& audioDevice,
                                BoolControlSlot* slots,
                                uint32_t slotCount);

} // namespace ASFW::Isoch::Audio
