//
// ASFWAudioDevice.hpp
// ASFWDriver
//
// Driver-side audio endpoint model used to configure ASFWAudioNub/ASFWAudioDriver.
//

#pragma once

#include "AudioPropertyKeys.hpp"

#include <DriverKit/OSArray.h>
#include <DriverKit/OSBoolean.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/OSString.h>
#include <cstdint>
#include <string>
#include <vector>

namespace ASFW::Audio::Model {

enum class StreamMode : uint8_t {
    kNonBlocking = 0,
    kBlocking = 1,
};

struct BoolControlOverride {
    uint32_t classIdFourCC{0};
    uint32_t scopeFourCC{0};
    uint32_t element{0};
    bool isSettable{false};
    bool initialValue{false};
};

struct ASFWAudioDevice {
    uint64_t guid{0};
    uint32_t vendorId{0};
    uint32_t modelId{0};
    std::string deviceName{"FireWire Audio"};
    uint32_t channelCount{2};
    uint32_t inputChannelCount{2};
    uint32_t outputChannelCount{2};
    std::vector<uint32_t> sampleRates{};
    uint32_t currentSampleRate{48000};
    std::string inputPlugName{"Input"};
    std::string outputPlugName{"Output"};
    StreamMode streamMode{StreamMode::kNonBlocking};
    bool hasPhantomOverride{false};
    uint32_t phantomSupportedMask{0};
    uint32_t phantomInitialMask{0};
    std::vector<BoolControlOverride> boolControlOverrides{};

    // Populate properties consumed by ASFWAudioDriver.
    // Returns false only if required objects could not be created.
    bool PopulateNubProperties(OSDictionary* properties) const {
        if (!properties) {
            return false;
        }

        auto deviceNameStr = OSSharedPtr(OSString::withCString(deviceName.c_str()), OSNoRetain);
        auto channelCountNum = OSSharedPtr(OSNumber::withNumber(channelCount, 32), OSNoRetain);
        auto guidNum = OSSharedPtr(OSNumber::withNumber(guid, 64), OSNoRetain);
        auto vendorIdNum = OSSharedPtr(OSNumber::withNumber(vendorId, 32), OSNoRetain);
        auto modelIdNum = OSSharedPtr(OSNumber::withNumber(modelId, 32), OSNoRetain);
        auto inputChannelCountNum = OSSharedPtr(OSNumber::withNumber(inputChannelCount, 32), OSNoRetain);
        auto outputChannelCountNum = OSSharedPtr(OSNumber::withNumber(outputChannelCount, 32), OSNoRetain);
        auto sampleRatesArray = OSSharedPtr(
            OSArray::withCapacity(static_cast<uint32_t>(sampleRates.size())), OSNoRetain);
        auto inputPlugNameStr = OSSharedPtr(OSString::withCString(inputPlugName.c_str()), OSNoRetain);
        auto outputPlugNameStr = OSSharedPtr(OSString::withCString(outputPlugName.c_str()), OSNoRetain);
        auto currentRateNum = OSSharedPtr(OSNumber::withNumber(currentSampleRate, 32), OSNoRetain);
        auto streamModeNum = OSSharedPtr(
            OSNumber::withNumber(static_cast<uint32_t>(streamMode), 32), OSNoRetain);
        auto hasPhantomOverrideBool = OSSharedPtr(
            hasPhantomOverride ? kOSBooleanTrue : kOSBooleanFalse,
            OSNoRetain);
        auto phantomSupportedMaskNum = OSSharedPtr(OSNumber::withNumber(phantomSupportedMask, 32), OSNoRetain);
        auto phantomInitialMaskNum = OSSharedPtr(OSNumber::withNumber(phantomInitialMask, 32), OSNoRetain);
        auto boolControlOverridesArray = OSSharedPtr(
            OSArray::withCapacity(static_cast<uint32_t>(boolControlOverrides.size())), OSNoRetain);

        if (!deviceNameStr || !channelCountNum || !guidNum || !vendorIdNum || !modelIdNum ||
            !inputChannelCountNum || !outputChannelCountNum ||
            !sampleRatesArray || !inputPlugNameStr || !outputPlugNameStr ||
            !currentRateNum || !streamModeNum || !hasPhantomOverrideBool ||
            !phantomSupportedMaskNum || !phantomInitialMaskNum || !boolControlOverridesArray) {
            return false;
        }

        for (uint32_t rate : sampleRates) {
            auto rateNum = OSSharedPtr(OSNumber::withNumber(rate, 32), OSNoRetain);
            if (rateNum) {
                sampleRatesArray->setObject(rateNum.get());
            }
        }

        for (const auto& overrideDesc : boolControlOverrides) {
            auto dict = OSSharedPtr(OSDictionary::withCapacity(5), OSNoRetain);
            auto classIdNum = OSSharedPtr(OSNumber::withNumber(overrideDesc.classIdFourCC, 32), OSNoRetain);
            auto scopeNum = OSSharedPtr(OSNumber::withNumber(overrideDesc.scopeFourCC, 32), OSNoRetain);
            auto elementNum = OSSharedPtr(OSNumber::withNumber(overrideDesc.element, 32), OSNoRetain);
            auto settableNum = OSSharedPtr(
                overrideDesc.isSettable ? kOSBooleanTrue : kOSBooleanFalse,
                OSNoRetain);
            auto initialNum = OSSharedPtr(
                overrideDesc.initialValue ? kOSBooleanTrue : kOSBooleanFalse,
                OSNoRetain);
            if (!dict || !classIdNum || !scopeNum || !elementNum || !settableNum || !initialNum) {
                continue;
            }
            dict->setObject(PropertyKeys::kBoolClassId, classIdNum.get());
            dict->setObject(PropertyKeys::kBoolScope, scopeNum.get());
            dict->setObject(PropertyKeys::kBoolElement, elementNum.get());
            dict->setObject(PropertyKeys::kBoolSettable, settableNum.get());
            dict->setObject(PropertyKeys::kBoolInitial, initialNum.get());
            boolControlOverridesArray->setObject(dict.get());
        }

        properties->setObject(PropertyKeys::kDeviceName, deviceNameStr.get());
        properties->setObject(PropertyKeys::kChannelCount, channelCountNum.get());
        properties->setObject(PropertyKeys::kSampleRates, sampleRatesArray.get());
        properties->setObject(PropertyKeys::kGuid, guidNum.get());
        properties->setObject(PropertyKeys::kVendorId, vendorIdNum.get());
        properties->setObject(PropertyKeys::kModelId, modelIdNum.get());
        properties->setObject(PropertyKeys::kInputChannelCount, inputChannelCountNum.get());
        properties->setObject(PropertyKeys::kOutputChannelCount, outputChannelCountNum.get());
        properties->setObject(PropertyKeys::kInputPlugName, inputPlugNameStr.get());
        properties->setObject(PropertyKeys::kOutputPlugName, outputPlugNameStr.get());
        properties->setObject(PropertyKeys::kCurrentSampleRate, currentRateNum.get());
        properties->setObject(PropertyKeys::kStreamMode, streamModeNum.get());
        properties->setObject(PropertyKeys::kHasPhantomOverride, hasPhantomOverrideBool.get());
        properties->setObject(PropertyKeys::kPhantomSupportedMask, phantomSupportedMaskNum.get());
        properties->setObject(PropertyKeys::kPhantomInitialMask, phantomInitialMaskNum.get());
        properties->setObject(PropertyKeys::kBoolControlOverrides, boolControlOverridesArray.get());

        return true;
    }
};

} // namespace ASFW::Audio::Model
