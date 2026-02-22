#include "AudioDriverConfig.hpp"

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

[[nodiscard]] bool ReadOSBoolValue(OSObject* object, bool fallback) {
    auto* booleanObject = OSDynamicCast(OSBoolean, object);
    if (!booleanObject) {
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

void BuildChannelNamesFromPlugs(ParsedAudioDriverConfig& inOutConfig) {
    const uint32_t maxChannels = std::min(inOutConfig.channelCount, kMaxNamedChannels);
    for (uint32_t index = 0; index < maxChannels; ++index) {
        snprintf(inOutConfig.inputChannelNames[index],
                 sizeof(inOutConfig.inputChannelNames[index]),
                 "%s %u",
                 inOutConfig.inputPlugName,
                 index + 1);
        snprintf(inOutConfig.outputChannelNames[index],
                 sizeof(inOutConfig.outputChannelNames[index]),
                 "%s %u",
                 inOutConfig.outputPlugName,
                 index + 1);
    }
}

} // namespace

void ParseAudioDriverConfigFromProperties(OSDictionary* properties,
                                          ParsedAudioDriverConfig& inOutConfig) {
    if (!properties) {
        return;
    }

    if (auto* guid = OSDynamicCast(OSNumber, properties->getObject("ASFWGUID"))) {
        inOutConfig.guid = guid->unsigned64BitValue();
    }
    if (auto* vendor = OSDynamicCast(OSNumber, properties->getObject("ASFWVendorID"))) {
        inOutConfig.vendorId = vendor->unsigned32BitValue();
    }
    if (auto* model = OSDynamicCast(OSNumber, properties->getObject("ASFWModelID"))) {
        inOutConfig.modelId = model->unsigned32BitValue();
    }
    if (auto* inputChannels = OSDynamicCast(OSNumber, properties->getObject("ASFWInputChannelCount"))) {
        inOutConfig.inputChannelCount = inputChannels->unsigned32BitValue();
    }
    if (auto* outputChannels = OSDynamicCast(OSNumber, properties->getObject("ASFWOutputChannelCount"))) {
        inOutConfig.outputChannelCount = outputChannels->unsigned32BitValue();
    }

    inOutConfig.hasPhantomOverride = ReadOSBoolValue(properties->getObject("ASFWHasPhantomOverride"), false);
    if (auto* supportedMask = OSDynamicCast(OSNumber, properties->getObject("ASFWPhantomSupportedMask"))) {
        inOutConfig.phantomSupportedMask = supportedMask->unsigned32BitValue();
    }
    if (auto* initialMask = OSDynamicCast(OSNumber, properties->getObject("ASFWPhantomInitialMask"))) {
        inOutConfig.phantomInitialMask = initialMask->unsigned32BitValue();
    }

    if (auto* name = OSDynamicCast(OSString, properties->getObject("ASFWDeviceName"))) {
        strlcpy(inOutConfig.deviceName, name->getCStringNoCopy(), sizeof(inOutConfig.deviceName));
    }

    if (auto* count = OSDynamicCast(OSNumber, properties->getObject("ASFWChannelCount"))) {
        inOutConfig.channelCount = count->unsigned32BitValue();
    }

    if (auto* rates = OSDynamicCast(OSArray, properties->getObject("ASFWSampleRates"))) {
        inOutConfig.sampleRateCount = 0;
        const uint32_t cappedCount = std::min(rates->getCount(), kMaxSampleRates);
        for (uint32_t i = 0; i < cappedCount; ++i) {
            auto* rate = OSDynamicCast(OSNumber, rates->getObject(i));
            if (!rate) {
                continue;
            }
            inOutConfig.sampleRates[inOutConfig.sampleRateCount++] =
                static_cast<double>(rate->unsigned32BitValue());
        }
    }

    if (auto* inputName = OSDynamicCast(OSString, properties->getObject("ASFWInputPlugName"))) {
        strlcpy(inOutConfig.inputPlugName, inputName->getCStringNoCopy(), sizeof(inOutConfig.inputPlugName));
    }
    if (auto* outputName = OSDynamicCast(OSString, properties->getObject("ASFWOutputPlugName"))) {
        strlcpy(inOutConfig.outputPlugName, outputName->getCStringNoCopy(), sizeof(inOutConfig.outputPlugName));
    }

    if (auto* rate = OSDynamicCast(OSNumber, properties->getObject("ASFWCurrentSampleRate"))) {
        inOutConfig.currentSampleRate = static_cast<double>(rate->unsigned32BitValue());
    }

    if (auto* mode = OSDynamicCast(OSNumber, properties->getObject("ASFWStreamMode"))) {
        inOutConfig.streamMode = (mode->unsigned32BitValue() == static_cast<uint32_t>(StreamMode::kBlocking))
            ? StreamMode::kBlocking
            : StreamMode::kNonBlocking;
    }

    if (auto* overrideArray = OSDynamicCast(OSArray, properties->getObject("ASFWBoolControlOverrides"))) {
        for (uint32_t index = 0; index < overrideArray->getCount(); ++index) {
            auto* entry = OSDynamicCast(OSDictionary, overrideArray->getObject(index));
            if (!entry) {
                continue;
            }

            auto* classNumber = OSDynamicCast(OSNumber, entry->getObject("ClassID"));
            auto* scopeNumber = OSDynamicCast(OSNumber, entry->getObject("Scope"));
            auto* elementNumber = OSDynamicCast(OSNumber, entry->getObject("Element"));
            if (!classNumber || !scopeNumber || !elementNumber) {
                continue;
            }

            const BoolControlDescriptor descriptor{
                .classIdFourCC = classNumber->unsigned32BitValue(),
                .scopeFourCC = scopeNumber->unsigned32BitValue(),
                .element = elementNumber->unsigned32BitValue(),
                .isSettable = ReadOSBoolValue(entry->getObject("Settable"), false),
                .initialValue = ReadOSBoolValue(entry->getObject("Initial"), false),
            };
            AppendBoolControl(inOutConfig, descriptor);
        }
    }

    BuildChannelNamesFromPlugs(inOutConfig);
}

} // namespace ASFW::Isoch::Audio
