//
// ASFWAudioDriverControls.cpp
// ASFWDriver
//
// Protocol-backed control forwarding for ASFWAudioDriver.
//

#include "ASFWAudioDriverPrivate.hpp"

kern_return_t ASFWAudioDriver::ApplyProtocolBooleanControl(uint32_t classIdFourCC,
                                                           uint32_t element,
                                                           bool value)
{
    if (!ivars) {
        return kIOReturnNotReady;
    }
    // The mute beside a host-realised master volume silences the PCM gain, like the volume.
    if (ivars->device.hostOutputVolume &&
        classIdFourCC == ASFW::Isoch::Audio::kHostOutputMuteRoute) {
        ivars->runtime.hostOutputGain.SetMuted(value);
        ASFW_LOG(Audio, "[HostMute] muted=%u", value ? 1u : 0u);
        return kIOReturnSuccess;
    }
    if (!ivars->device.audioNub) {
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

kern_return_t ASFWAudioDriver::ApplyHostOutputVolume(float decibels) {
    if (!ivars || !ivars->device.hostOutputVolume) {
        return kIOReturnNotReady;
    }
    const float gain = ASFW::Audio::Runtime::HostOutputGainFromDecibels(
        decibels, ivars->device.hostOutputVolumeMinDb);
    ivars->runtime.hostOutputGain.SetTarget(gain);
    ASFW_LOG(Audio, "[HostVolume] centiDb=%d gainMilli=%d",
             static_cast<int>(decibels * 100.0f), static_cast<int>(gain * 1000.0f));
    return kIOReturnSuccess;
}
