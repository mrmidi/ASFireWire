#include "AudioDriverConfig.hpp"

#include "../../Model/AudioPropertyKeys.hpp"

#include <DriverKit/OSArray.h>
#include <DriverKit/OSBoolean.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSString.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ASFW::Isoch::Audio {
namespace {

namespace Keys = ASFW::Audio::Model::PropertyKeys;

[[nodiscard]] bool ReadOSBoolValue(OSObject* object, bool fallback) {
    auto* booleanObject = OSDynamicCast(OSBoolean, object);
    if (booleanObject == nullptr) {
        return fallback;
    }
    return booleanObject == kOSBooleanTrue;
}

void AppendBoolControl(ParsedAudioDriverConfig& inOutConfig,
                       const BoolControlDescriptor& descriptor) {
    if (inOutConfig.boolControlCount >= kMaxBoolControls) {
        return;
    }
    inOutConfig.boolControls[inOutConfig.boolControlCount++] = descriptor;
}

void ParseIdentityProperties(OSDictionary* properties, ParsedAudioDriverConfig& inOutConfig) {
    if (auto* guid = OSDynamicCast(OSNumber, properties->getObject(Keys::kGuid))) {
        inOutConfig.guid = guid->unsigned64BitValue();
    }
    if (auto* vendor = OSDynamicCast(OSNumber, properties->getObject(Keys::kVendorId))) {
        inOutConfig.vendorId = vendor->unsigned32BitValue();
    }
    if (auto* model = OSDynamicCast(OSNumber, properties->getObject(Keys::kModelId))) {
        inOutConfig.modelId = model->unsigned32BitValue();
    }
    if (auto* inputChannels = OSDynamicCast(OSNumber, properties->getObject(Keys::kInputChannelCount))) {
        inOutConfig.inputChannelCount = inputChannels->unsigned32BitValue();
        inOutConfig.hasExplicitInputChannelCount = true;
    }
    if (auto* outputChannels = OSDynamicCast(OSNumber, properties->getObject(Keys::kOutputChannelCount))) {
        inOutConfig.outputChannelCount = outputChannels->unsigned32BitValue();
        inOutConfig.hasExplicitOutputChannelCount = true;
    }
}

void ParsePhantomProperties(OSDictionary* properties, ParsedAudioDriverConfig& inOutConfig) {
    inOutConfig.hasPhantomOverride =
        ReadOSBoolValue(properties->getObject(Keys::kHasPhantomOverride), false);
    if (auto* supportedMask = OSDynamicCast(OSNumber, properties->getObject(Keys::kPhantomSupportedMask))) {
        inOutConfig.phantomSupportedMask = supportedMask->unsigned32BitValue();
    }
    if (auto* initialMask = OSDynamicCast(OSNumber, properties->getObject(Keys::kPhantomInitialMask))) {
        inOutConfig.phantomInitialMask = initialMask->unsigned32BitValue();
    }
}

void ParseDevicePresentationProperties(OSDictionary* properties,
                                       ParsedAudioDriverConfig& inOutConfig) {
    if (auto* name = OSDynamicCast(OSString, properties->getObject(Keys::kDeviceName))) {
        strlcpy(inOutConfig.deviceName, name->getCStringNoCopy(), sizeof(inOutConfig.deviceName));
    }
    if (auto* count = OSDynamicCast(OSNumber, properties->getObject(Keys::kChannelCount))) {
        inOutConfig.channelCount = count->unsigned32BitValue();
    }
    if (auto* rate = OSDynamicCast(OSNumber, properties->getObject(Keys::kCurrentSampleRate))) {
        inOutConfig.currentSampleRate = static_cast<double>(rate->unsigned32BitValue());
    }
    if (auto* mode = OSDynamicCast(OSNumber, properties->getObject(Keys::kStreamMode))) {
        inOutConfig.streamMode = (mode->unsigned32BitValue() ==
                                  static_cast<uint32_t>(StreamMode::kBlocking))
            ? StreamMode::kBlocking
            : StreamMode::kNonBlocking;
    }
}

void ParseSampleRates(OSDictionary* properties, ParsedAudioDriverConfig& inOutConfig) {
    auto* rates = OSDynamicCast(OSArray, properties->getObject(Keys::kSampleRates));
    if (rates == nullptr) {
        return;
    }

    inOutConfig.sampleRateCount = 0;
    const uint32_t cappedCount = std::min(rates->getCount(), kMaxSampleRates);
    for (uint32_t i = 0; i < cappedCount; ++i) {
        auto* rate = OSDynamicCast(OSNumber, rates->getObject(i));
        if (rate == nullptr) {
            continue;
        }
        inOutConfig.sampleRates[inOutConfig.sampleRateCount++] =
            static_cast<double>(rate->unsigned32BitValue());
    }
}

void ParsePlugNames(OSDictionary* properties, ParsedAudioDriverConfig& inOutConfig) {
    if (auto* inputName = OSDynamicCast(OSString, properties->getObject(Keys::kInputPlugName))) {
        strlcpy(inOutConfig.inputPlugName, inputName->getCStringNoCopy(), sizeof(inOutConfig.inputPlugName));
    }
    if (auto* outputName = OSDynamicCast(OSString, properties->getObject(Keys::kOutputPlugName))) {
        strlcpy(inOutConfig.outputPlugName, outputName->getCStringNoCopy(), sizeof(inOutConfig.outputPlugName));
    }
}

// Read an optional OSArray of OSString device labels into a fixed [N][64] array.
// Indices align with the channel index; missing/empty entries stay empty so
// BuildChannelNamesFromPlugs synthesizes a name for that slot.
void ParseChannelNameArray(OSDictionary* properties,
                           const char* key,
                           char (*dst)[64]) {
    auto* array = OSDynamicCast(OSArray, properties->getObject(key));
    if (array == nullptr) {
        return;
    }
    const uint32_t count = std::min(array->getCount(), kMaxNamedChannels);
    for (uint32_t i = 0; i < count; ++i) {
        if (auto* name = OSDynamicCast(OSString, array->getObject(i))) {
            strlcpy(dst[i], name->getCStringNoCopy(), 64);
        }
    }
}

void ParseChannelNames(OSDictionary* properties, ParsedAudioDriverConfig& inOutConfig) {
    ParseChannelNameArray(properties, Keys::kInputChannelNames, inOutConfig.deviceInputChannelNames);
    ParseChannelNameArray(properties, Keys::kOutputChannelNames, inOutConfig.deviceOutputChannelNames);
}

void ParseBoolControlOverrides(OSDictionary* properties, ParsedAudioDriverConfig& inOutConfig) {
    auto* overrideArray = OSDynamicCast(OSArray, properties->getObject(Keys::kBoolControlOverrides));
    if (overrideArray == nullptr) {
        return;
    }

    for (uint32_t index = 0; index < overrideArray->getCount(); ++index) {
        auto* entry = OSDynamicCast(OSDictionary, overrideArray->getObject(index));
        if (!entry) {
            continue;
        }

        auto* classNumber = OSDynamicCast(OSNumber, entry->getObject(Keys::kBoolClassId));
        auto* scopeNumber = OSDynamicCast(OSNumber, entry->getObject(Keys::kBoolScope));
        auto* elementNumber = OSDynamicCast(OSNumber, entry->getObject(Keys::kBoolElement));
        if (classNumber == nullptr || scopeNumber == nullptr || elementNumber == nullptr) {
            continue;
        }

        const BoolControlDescriptor descriptor{
            .classIdFourCC = classNumber->unsigned32BitValue(),
            .scopeFourCC = scopeNumber->unsigned32BitValue(),
            .element = elementNumber->unsigned32BitValue(),
            .isSettable = ReadOSBoolValue(entry->getObject(Keys::kBoolSettable), false),
            .initialValue = ReadOSBoolValue(entry->getObject(Keys::kBoolInitial), false),
        };
        AppendBoolControl(inOutConfig, descriptor);
    }
}

} // namespace

// Fill each element name, preferring a per-channel device label when present
// and falling back to the synthesized "<plug> N". Centralizes the rule so both
// the initial parse and the post-profile regeneration in BuildAudioGraph agree.
void BuildChannelNamesFromPlugs(ParsedAudioDriverConfig& inOutConfig) {
    const uint32_t maxInputChannels = std::min(inOutConfig.inputChannelCount, kMaxNamedChannels);
    const uint32_t maxOutputChannels = std::min(inOutConfig.outputChannelCount, kMaxNamedChannels);
    for (uint32_t index = 0; index < maxInputChannels; ++index) {
        if (inOutConfig.deviceInputChannelNames[index][0] != '\0') {
            strlcpy(inOutConfig.inputChannelNames[index],
                    inOutConfig.deviceInputChannelNames[index],
                    sizeof(inOutConfig.inputChannelNames[index]));
            continue;
        }
        snprintf(inOutConfig.inputChannelNames[index],
                 sizeof(inOutConfig.inputChannelNames[index]),
                 "%s %u",
                 inOutConfig.inputPlugName,
                 index + 1);
    }
    for (uint32_t index = 0; index < maxOutputChannels; ++index) {
        if (inOutConfig.deviceOutputChannelNames[index][0] != '\0') {
            strlcpy(inOutConfig.outputChannelNames[index],
                    inOutConfig.deviceOutputChannelNames[index],
                    sizeof(inOutConfig.outputChannelNames[index]));
            continue;
        }
        snprintf(inOutConfig.outputChannelNames[index],
                 sizeof(inOutConfig.outputChannelNames[index]),
                 "%s %u",
                 inOutConfig.outputPlugName,
                 index + 1);
    }
}

void ParseAudioDriverConfigFromProperties(OSDictionary* properties,
                                          ParsedAudioDriverConfig& inOutConfig) {
    if (!properties) {
        return;
    }

    ParseIdentityProperties(properties, inOutConfig);
    ParsePhantomProperties(properties, inOutConfig);
    ParseDevicePresentationProperties(properties, inOutConfig);
    ParseSampleRates(properties, inOutConfig);
    ParsePlugNames(properties, inOutConfig);
    ParseChannelNames(properties, inOutConfig);
    ParseBoolControlOverrides(properties, inOutConfig);
    BuildChannelNamesFromPlugs(inOutConfig);
}

} // namespace ASFW::Isoch::Audio
