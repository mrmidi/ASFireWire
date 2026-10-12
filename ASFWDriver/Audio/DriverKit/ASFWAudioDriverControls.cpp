//
// ASFWAudioDriverControls.cpp
// ASFWDriver
//
// Protocol-backed control forwarding for ASFWAudioDriver.
//

#include "ASFWAudioDevice.h"
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

namespace ASFW::Audio::DriverKit {

kern_return_t PublishHostOutputVolumeControls(ASFWAudioDriver& driver,
                                              ASFWAudioDriver_IVars& ivars) noexcept {
    if (!ivars.audioDevice) {
        return kIOReturnNotReady;
    }
    float initialDb = 0.0f;
    float minDb = 0.0f;
    const kern_return_t volumeStatus = ASFW::Isoch::Audio::AddHostOutputVolumeToDevice(
        driver, *ivars.audioDevice, ivars.device.outputVolumeControl, initialDb, minDb);
    ASFW_LOG(Audio, "ADK GRAPH op=device.AddHostOutputVolume kr=0x%x", volumeStatus);
    if (volumeStatus != kIOReturnSuccess) {
        return volumeStatus;
    }
    ivars.device.hostOutputVolumeMinDb = minDb;
    ivars.runtime.hostOutputGain.SetTarget(
        ASFW::Audio::Runtime::HostOutputGainFromDecibels(initialDb, minDb));
    ivars.runtime.hostOutputGain.SetMuted(false);
    const kern_return_t muteStatus = ASFW::Isoch::Audio::AddHostOutputMuteToDevice(
        driver, *ivars.audioDevice, ivars.device.outputMuteControl);
    ASFW_LOG(Audio, "ADK GRAPH op=device.AddHostOutputMute kr=0x%x", muteStatus);
    return kIOReturnSuccess;
}

void WithdrawHostOutputVolumeControls(ASFWAudioDriver_IVars& ivars) noexcept {
    if (ivars.audioDevice) {
        if (ivars.device.outputMuteControl) {
            const kern_return_t kr =
                ivars.audioDevice->RemoveControl(ivars.device.outputMuteControl.get());
            ASFW_LOG(Audio, "ADK GRAPH op=device.RemoveHostOutputMute kr=0x%x", kr);
        }
        if (ivars.device.outputVolumeControl) {
            const kern_return_t kr =
                ivars.audioDevice->RemoveControl(ivars.device.outputVolumeControl.get());
            ASFW_LOG(Audio, "ADK GRAPH op=device.RemoveHostOutputVolume kr=0x%x", kr);
        }
    }
    ivars.device.outputMuteControl.reset();
    ivars.device.outputVolumeControl.reset();
    ASFW::Audio::Runtime::ReleaseHostOutputGain(ivars.runtime.hostOutputGain);
}

void SyncHostOutputVolumeControls(ASFWAudioDriver& driver, ASFWAudioDriver_IVars& ivars) noexcept {
    // Only a profile with a host-realised volume has controls the setting governs.
    if (!ivars.device.hostOutputVolume || !ivars.audioDevice) {
        return;
    }
    const bool enabled = ASFW::Audio::Runtime::CoreAudioVolumeSetting::Shared().Enabled();
    const bool published = static_cast<bool>(ivars.device.outputVolumeControl);
    if (enabled == published) {
        return;
    }
    if (enabled) {
        (void)PublishHostOutputVolumeControls(driver, ivars);
    } else {
        WithdrawHostOutputVolumeControls(ivars);
    }
    ASFW_LOG(Audio, "[HostVolume] coreAudioControls=%u (live)", enabled ? 1u : 0u);
}

} // namespace ASFW::Audio::DriverKit
