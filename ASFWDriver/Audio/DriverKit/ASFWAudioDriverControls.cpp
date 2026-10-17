//
// ASFWAudioDriverControls.cpp
// ASFWDriver
//
// Protocol-backed control forwarding for ASFWAudioDriver.
//

#include "ASFWAudioDriverPrivate.hpp"
#include <bit>
#include <cmath>

kern_return_t ASFWAudioDriver::ApplyProtocolBooleanControl(uint32_t classIdFourCC,
                                                           uint32_t element,
                                                           bool value)
{
    if (!ivars || !ivars->device.audioNub) {
        return kIOReturnNotReady;
    }
    return ivars->device.audioNub->SetProtocolBooleanControl(classIdFourCC, element, value);
}

kern_return_t ASFWAudioDriver::ReadProtocolBooleanControl(uint32_t classIdFourCC,
                                                          uint32_t element,
                                                          bool* outValue)
{
    if (!outValue) {
        return kIOReturnBadArgument;
    }
    *outValue = false;
    if (!ivars || !ivars->device.audioNub) {
        return kIOReturnNotReady;
    }
    return ivars->device.audioNub->GetProtocolBooleanControl(classIdFourCC, element, outValue);
}

kern_return_t ASFWAudioDriver::ApplyAvcFeatureControl(uint32_t token, bool muteControl,
                                                     int32_t value, int32_t* outConfirmedValue) {
    if (!ivars || !ivars->device.audioNub) return kIOReturnNotReady;
    return ivars->device.audioNub->SetAvcFeatureControl(token, muteControl, value, outConfirmedValue);
}

// Delivered on the same Default queue as graph construction and teardown.
// Set*Value updates HAL only; HandleChange* is the hardware-write entry point.
void IMPL(ASFWAudioDriver, ControlParameterChanged) {
    if (!ivars || action != ivars->controlChangeAction.get() || !ivars->audioDevice ||
        guid != ivars->device.guid || origin != 0 || revision < ivars->device.controlRevision) return;
    if (revision > ivars->device.controlRevision) {
        ivars->device.controlRevision = revision;
        for (auto& slot : ivars->device.avcControlSlots) slot.levelSequence = slot.muteSequence = 0;
    }
    const uint32_t token = parameterId >> 1;
    const bool isMute = (parameterId & 1) != 0;
    for (uint32_t index = 0; index < ivars->device.avcControlCount; ++index) {
        const auto& descriptor = ivars->device.avcControls[index];
        if (descriptor.token != token) continue;
        auto& slot = ivars->device.avcControlSlots[index];
        auto& last = isMute ? slot.muteSequence : slot.levelSequence;
        if ((sequence == 0 && last != 0) || (sequence != 0 && sequence <= last)) return;
        kern_return_t status = kIOReturnUnsupported;
        if (isMute && kind == 0 && bits <= 1 && slot.mute) {
            status = slot.mute->SetControlValue(bits != 0);
        } else if (!isMute && kind == 1 && slot.level) {
            const double db = std::bit_cast<double>(bits);
            if (!std::isfinite(db) || db < ASFW::Audio::Model::AvcVolumeRange::Decibels(descriptor.range.minimum) ||
                db > ASFW::Audio::Model::AvcVolumeRange::Decibels(descriptor.range.maximum)) return;
            status = slot.level->SetDecibelValue(static_cast<float>(db));
        }
        if (status == kIOReturnSuccess) last = sequence;
        else ASFW_LOG(Audio, "[ControlChanged] HAL update failed parameter=%u kr=0x%x", parameterId, status);
        return;
    }
}
