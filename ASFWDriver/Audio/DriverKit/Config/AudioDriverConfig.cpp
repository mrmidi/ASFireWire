#include "AudioDriverConfig.hpp"

#include "../../Devices/AudioEndpointProfileWire.hpp"
#include "../../Model/AudioPropertyKeys.hpp"

#include <DriverKit/OSArray.h>
#include <DriverKit/OSData.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSString.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <span>

namespace ASFW::Isoch::Audio {
namespace {

namespace Keys = ASFW::Audio::Model::PropertyKeys;

[[nodiscard]] bool CopyRequiredString(OSDictionary* properties,
                                      const char* key,
                                      char* destination,
                                      size_t capacity) noexcept {
    auto* value = OSDynamicCast(OSString, properties->getObject(key));
    if (!value || !value->getCStringNoCopy() || value->getLength() == 0) {
        return false;
    }
    strlcpy(destination, value->getCStringNoCopy(), capacity);
    return true;
}

void CopyOptionalString(OSDictionary* properties,
                        const char* key,
                        char* destination,
                        size_t capacity) noexcept {
    if (auto* value = OSDynamicCast(OSString, properties->getObject(key));
        value && value->getCStringNoCopy()) {
        strlcpy(destination, value->getCStringNoCopy(), capacity);
    }
}

void ParsePlugNames(OSDictionary* properties,
                    ParsedAudioDriverConfig& config) noexcept {
    CopyOptionalString(properties, Keys::kInputPlugName,
                       config.inputPlugName, sizeof(config.inputPlugName));
    CopyOptionalString(properties, Keys::kOutputPlugName,
                       config.outputPlugName, sizeof(config.outputPlugName));
}

void ParseChannelNameArray(OSDictionary* properties,
                           const char* key,
                           char (*destination)[64]) noexcept {
    auto* array = OSDynamicCast(OSArray, properties->getObject(key));
    if (!array) return;
    const uint32_t count = std::min(array->getCount(), kMaxNamedChannels);
    for (uint32_t i = 0; i < count; ++i) {
        if (auto* name = OSDynamicCast(OSString, array->getObject(i));
            name && name->getCStringNoCopy()) {
            strlcpy(destination[i], name->getCStringNoCopy(), 64);
        }
    }
}

void ParseChannelNames(OSDictionary* properties,
                       ParsedAudioDriverConfig& config) noexcept {
    ParseChannelNameArray(properties, Keys::kInputChannelNames,
                          config.deviceInputChannelNames);
    ParseChannelNameArray(properties, Keys::kOutputChannelNames,
                          config.deviceOutputChannelNames);
}

} // namespace

void BuildChannelNamesFromPlugs(ParsedAudioDriverConfig& config) {
    const uint32_t maxInput = std::min(config.inputChannelCount,
                                       kMaxNamedChannels);
    const uint32_t maxOutput = std::min(config.outputChannelCount,
                                        kMaxNamedChannels);
    for (uint32_t index = 0; index < maxInput; ++index) {
        if (config.deviceInputChannelNames[index][0] != '\0') {
            strlcpy(config.inputChannelNames[index],
                    config.deviceInputChannelNames[index],
                    sizeof(config.inputChannelNames[index]));
        } else {
            snprintf(config.inputChannelNames[index],
                     sizeof(config.inputChannelNames[index]),
                     "%s %u", config.inputPlugName, index + 1);
        }
    }
    for (uint32_t index = 0; index < maxOutput; ++index) {
        if (config.deviceOutputChannelNames[index][0] != '\0') {
            strlcpy(config.outputChannelNames[index],
                    config.deviceOutputChannelNames[index],
                    sizeof(config.outputChannelNames[index]));
        } else {
            snprintf(config.outputChannelNames[index],
                     sizeof(config.outputChannelNames[index]),
                     "%s %u", config.outputPlugName, index + 1);
        }
    }
}

bool ParseAudioDriverConfigFromProperties(
    OSDictionary* properties,
    ParsedAudioDriverConfig& config) {
    if (!properties) return false;

    auto* profileData = OSDynamicCast(
        OSData, properties->getObject(Keys::kEndpointProfile));
    if (!profileData || !profileData->getBytesNoCopy() ||
        profileData->getLength() == 0 ||
        profileData->getLength() >
            ASFW::Audio::Devices::Wire::kAudioEndpointProfileWireMaxBytes) {
        return false;
    }
    const auto parsed = ASFW::Audio::Devices::Wire::Parse(
        std::span<const uint8_t>{
            static_cast<const uint8_t*>(profileData->getBytesNoCopy()),
            profileData->getLength()});
    if (!parsed || !parsed->endpointId || !parsed->deviceInstanceId ||
        parsed->supportedRateCount == 0) {
        return false;
    }

    config.resolvedProfile = *parsed;
    config.endpointId = parsed->endpointId.value;
    config.deviceInstanceId = parsed->deviceInstanceId.value;
    config.observedGuid = parsed->observedGuid;
    config.inputChannelCount = parsed->runtimeCaps.hostInputPcmChannels;
    config.outputChannelCount = parsed->runtimeCaps.hostOutputPcmChannels;
    config.hasExplicitInputChannelCount = true;
    config.hasExplicitOutputChannelCount = true;
    config.channelCount = std::max(config.inputChannelCount,
                                   config.outputChannelCount);
    config.currentSampleRate = parsed->currentSampleRateHz;
    config.sampleRateCount = parsed->supportedRateCount;
    for (uint8_t i = 0; i < parsed->supportedRateCount; ++i) {
        config.sampleRates[i] = parsed->supportedRates[i];
    }
    config.streamMode = parsed->streamMode ==
            ASFW::Audio::Devices::StreamModePolicy::Blocking
        ? StreamMode::kBlocking
        : StreamMode::kNonBlocking;

    if (!CopyRequiredString(properties, Keys::kDeviceName,
                            config.deviceName, sizeof(config.deviceName)) ||
        !CopyRequiredString(properties, Keys::kCoreAudioUid,
                            config.coreAudioUid, sizeof(config.coreAudioUid))) {
        return false;
    }
    CopyOptionalString(properties, Keys::kVendorName,
                       config.vendorName, sizeof(config.vendorName));
    config.resolvedProfile.deviceName = config.deviceName;
    config.resolvedProfile.vendorName = config.vendorName;
    config.resolvedProfile.coreAudioUid = config.coreAudioUid;

    ParsePlugNames(properties, config);
    ParseChannelNames(properties, config);
    BuildChannelNamesFromPlugs(config);
    return true;
}

} // namespace ASFW::Isoch::Audio
