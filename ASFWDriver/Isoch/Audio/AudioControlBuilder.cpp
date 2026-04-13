#include "AudioControlBuilder.hpp"

#include "../../Logging/Logging.hpp"

#include <DriverKit/OSString.h>

#include <cstdio>

namespace ASFW::Isoch::Audio {
namespace {

void BuildControlName(const BoolControlDescriptor& descriptor,
                      char (&outName)[96]) {
    if (descriptor.classIdFourCC == kClassIdPhantomPower &&
        descriptor.scopeFourCC == kScopeInput &&
        (descriptor.element == 1u || descriptor.element == 2u)) {
        snprintf(outName, sizeof(outName), "Phantom Power %u", descriptor.element);
        return;
    }

    if (descriptor.classIdFourCC == kClassIdPhaseInvert &&
        descriptor.scopeFourCC == kScopeInput &&
        (descriptor.element == 1u || descriptor.element == 2u)) {
        snprintf(outName, sizeof(outName), "Polarity %u", descriptor.element);
        return;
    }

    snprintf(outName,
             sizeof(outName),
             "%s Bool %u",
             ScopeLabel(descriptor.scopeFourCC),
             descriptor.element);
}

} // namespace

void ResetBoolControlSlots(BoolControlSlot* slots, uint32_t count) {
    if (!slots) {
        return;
    }
    for (uint32_t index = 0; index < count; ++index) {
        slots[index].control.reset();
        slots[index].valid = false;
    }
}

void AddBooleanControlsToDevice(ASFWAudioDriver& driver,
                                IOUserAudioDevice& audioDevice,
                                BoolControlSlot* slots,
                                uint32_t slotCount) {
    if (!slots) {
        return;
    }

    for (uint32_t index = 0; index < slotCount; ++index) {
        auto& slot = slots[index];
        if (!slot.valid) {
            continue;
        }

        bool controlValue = slot.descriptor.initialValue;
        bool hardwareValue = false;
        const kern_return_t readStatus = driver.ReadProtocolBooleanControl(slot.descriptor.classIdFourCC,
                                                                           slot.descriptor.element,
                                                                           &hardwareValue);
        if (readStatus == kIOReturnSuccess) {
            controlValue = hardwareValue;
        } else {
            ASFW_LOG(Audio,
                     "ASFWAudioDriver: bool control read fallback class=0x%08x element=%u status=0x%x",
                     slot.descriptor.classIdFourCC,
                     slot.descriptor.element,
                     readStatus);
        }

        auto control = ASFWProtocolBooleanControl::Create(
            &driver,
            slot.descriptor.isSettable,
            controlValue,
            slot.descriptor.element,
            static_cast<IOUserAudioObjectPropertyScope>(slot.descriptor.scopeFourCC),
            static_cast<IOUserAudioClassID>(slot.descriptor.classIdFourCC),
            slot.descriptor.classIdFourCC,
            slot.descriptor.element);
        if (!control) {
            ASFW_LOG(Audio,
                     "ASFWAudioDriver: Failed to create bool control class=0x%08x element=%u",
                     slot.descriptor.classIdFourCC,
                     slot.descriptor.element);
            slot.valid = false;
            continue;
        }

        char controlName[96] = {};
        BuildControlName(slot.descriptor, controlName);

        auto controlNameString = OSSharedPtr(OSString::withCString(controlName), OSNoRetain);
        if (controlNameString) {
            control->SetName(controlNameString.get());
        }

        kern_return_t status = audioDevice.AddControl(control.get());
        if (status != kIOReturnSuccess) {
            ASFW_LOG(Audio,
                     "ASFWAudioDriver: Failed to add bool control class=0x%08x element=%u status=0x%x",
                     slot.descriptor.classIdFourCC,
                     slot.descriptor.element,
                     status);
            slot.valid = false;
            continue;
        }

        slot.control = control;
        ASFW_LOG(Audio,
                 "ASFWAudioDriver: Added bool control class=0x%08x scope=0x%08x element=%u initial=%u",
                 slot.descriptor.classIdFourCC,
                 slot.descriptor.scopeFourCC,
                 slot.descriptor.element,
                 controlValue ? 1u : 0u);
    }
}

} // namespace ASFW::Isoch::Audio
