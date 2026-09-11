#include "AudioControlBuilder.hpp"

#include "../../../Logging/Logging.hpp"
#include "../../Protocols/DeviceControl.hpp"

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

[[nodiscard]] kern_return_t AddBooleanControlsToDevice(
    ASFWAudioDriver& driver,
    IOUserAudioDevice& audioDevice,
    BoolControlSlot* slots,
    uint32_t slotCount) {
    if (!slots) {
        return slotCount == 0 ? kIOReturnSuccess : kIOReturnBadArgument;
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
                     "ASFWAudioDriver: bool control using descriptor initial value class=0x%08x element=%u status=0x%x",
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
            return kIOReturnNoMemory;
        }

        char controlName[96] = {};
        BuildControlName(slot.descriptor, controlName);

        auto controlNameString = OSSharedPtr(OSString::withCString(controlName), OSNoRetain);
        if (!controlNameString) {
            ASFW_LOG(Audio,
                     "ASFWAudioDriver: Failed to allocate bool control name class=0x%08x element=%u",
                     slot.descriptor.classIdFourCC,
                     slot.descriptor.element);
            slot.valid = false;
            return kIOReturnNoMemory;
        }
        const kern_return_t nameStatus =
            control->SetName(controlNameString.get());
        if (nameStatus != kIOReturnSuccess) {
            ASFW_LOG(Audio,
                     "ASFWAudioDriver: bool control SetName failed class=0x%08x element=%u kr=0x%x",
                     slot.descriptor.classIdFourCC,
                     slot.descriptor.element,
                     nameStatus);
            slot.valid = false;
            return nameStatus;
        }

        kern_return_t status = audioDevice.AddControl(control.get());
        if (status != kIOReturnSuccess) {
            ASFW_LOG(Audio,
                     "ASFWAudioDriver: Failed to add bool control class=0x%08x element=%u status=0x%x",
                     slot.descriptor.classIdFourCC,
                     slot.descriptor.element,
                     status);
            slot.valid = false;
            return status;
        }

        slot.control = control;
        ASFW_LOG(Audio,
                 "ASFWAudioDriver: Added bool control class=0x%08x scope=0x%08x element=%u initial=%u",
                 slot.descriptor.classIdFourCC,
                 slot.descriptor.scopeFourCC,
                 slot.descriptor.element,
                 controlValue ? 1u : 0u);
    }

    return kIOReturnSuccess;
}

kern_return_t AddProtocolOutputVolumeToDevice(
    ASFWAudioDriver& driver,
    IOUserAudioDevice& audioDevice,
    OSSharedPtr<ASFWProtocolLevelControl>& outControl,
    float& outInitialDecibels) {
    using ASFW::Audio::kControlClassVolume;
    using ASFW::Audio::kControlElementMain;
    using ASFW::Audio::kControlScopeOutput;

    bool settable = false;
    float minDb = 0.0f;
    float maxDb = 0.0f;
    if (driver.DescribeProtocolLevelControl(kControlClassVolume, kControlScopeOutput,
                                            kControlElementMain, &settable, &minDb,
                                            &maxDb) != kIOReturnSuccess) {
        return kIOReturnUnsupported;
    }

    // Until the device answers, start quiet rather than loud: a volume-key press from a
    // too-high starting point would write a too-high level.
    constexpr float kFallbackDb = -30.0f;
    float initialDb = kFallbackDb < minDb ? minDb : (kFallbackDb > maxDb ? maxDb : kFallbackDb);
    float readDb = 0.0f;
    const kern_return_t readStatus = driver.ReadProtocolLevelControl(
        kControlClassVolume, kControlScopeOutput, kControlElementMain, &readDb);
    if (readStatus == kIOReturnSuccess) {
        initialDb = readDb;
    } else {
        ASFW_LOG(Audio,
                 "ASFWAudioDriver: output volume not read yet (0x%x); starting at %d dB",
                 readStatus, static_cast<int>(initialDb));
    }

    auto control = ASFWProtocolLevelControl::Create(&driver,
                                                    settable,
                                                    initialDb,
                                                    minDb,
                                                    maxDb,
                                                    IOUserAudioObjectPropertyElementMain,
                                                    IOUserAudioObjectPropertyScope::Output,
                                                    IOUserAudioClassID::VolumeControl,
                                                    kControlClassVolume,
                                                    kControlScopeOutput,
                                                    kControlElementMain);
    if (!control) {
        return kIOReturnNoMemory;
    }
    const kern_return_t status = audioDevice.AddControl(control.get());
    if (status != kIOReturnSuccess) {
        return status;
    }

    outControl = control;
    outInitialDecibels = initialDb;
    ASFW_LOG(Audio,
             "ASFWAudioDriver: Added hardware output volume range=[%d, %d] dB initialCentiDb=%d",
             static_cast<int>(minDb), static_cast<int>(maxDb),
             static_cast<int>(initialDb * 100.0f));
    return kIOReturnSuccess;
}

} // namespace ASFW::Isoch::Audio
