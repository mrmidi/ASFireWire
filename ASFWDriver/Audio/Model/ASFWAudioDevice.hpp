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
    // Per-channel device labels in channel order (empty = synthesize names).
    std::vector<std::string> inputChannelNames{};
    std::vector<std::string> outputChannelNames{};
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
        auto inputChannelCountNum =
            OSSharedPtr(OSNumber::withNumber(inputChannelCount, 32), OSNoRetain);
        auto outputChannelCountNum =
            OSSharedPtr(OSNumber::withNumber(outputChannelCount, 32), OSNoRetain);
        auto sampleRatesArray = OSSharedPtr(
            OSArray::withCapacity(static_cast<uint32_t>(sampleRates.size())), OSNoRetain);
        auto inputPlugNameStr = OSSharedPtr(OSString::withCString(inputPlugName.c_str()), OSNoRetain);
        auto outputPlugNameStr = OSSharedPtr(OSString::withCString(outputPlugName.c_str()), OSNoRetain);
        auto currentRateNum = OSSharedPtr(OSNumber::withNumber(currentSampleRate, 32), OSNoRetain);

        if (!deviceNameStr || !channelCountNum || !guidNum || !vendorIdNum || !modelIdNum ||
            !inputChannelCountNum || !outputChannelCountNum || !sampleRatesArray ||
            !inputPlugNameStr || !outputPlugNameStr || !currentRateNum) {
            return false;
        }

        for (uint32_t rate : sampleRates) {
            auto rateNum = OSSharedPtr(OSNumber::withNumber(rate, 32), OSNoRetain);
            if (rateNum) {
                sampleRatesArray->setObject(rateNum.get());
            }
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

        // Sample rates advertised to CoreAudio. The HAL builds a stream format
        // per entry (ASFWAudioDriverGraph), so this is what the user can select.
        if (!sampleRates.empty()) {
            auto rateArray = OSSharedPtr(OSArray::withCapacity(
                static_cast<uint32_t>(sampleRates.size())), OSNoRetain);
            if (rateArray) {
                for (uint32_t hz : sampleRates) {
                    auto n = OSSharedPtr(OSNumber::withNumber(hz, 32), OSNoRetain);
                    if (n) {
                        rateArray->setObject(n.get());
                    }
                }
                properties->setObject(PropertyKeys::kSampleRates, rateArray.get());
            }
        }
        if (auto curRate = OSSharedPtr(OSNumber::withNumber(currentSampleRate, 32), OSNoRetain)) {
            properties->setObject(PropertyKeys::kCurrentSampleRate, curRate.get());
        }

        // Per-channel device labels (optional). The audio side reads these in
        // channel order and prefers them over synthesized names.
        PublishChannelNames(properties, PropertyKeys::kInputChannelNames, inputChannelNames);
        PublishChannelNames(properties, PropertyKeys::kOutputChannelNames, outputChannelNames);

        return true;
    }

private:
    static void PublishChannelNames(OSDictionary* properties,
                                    const char* key,
                                    const std::vector<std::string>& names) {
        if (names.empty()) {
            return;
        }
        auto array = OSSharedPtr(OSArray::withCapacity(
            static_cast<uint32_t>(names.size())), OSNoRetain);
        if (!array) {
            return;
        }
        for (const auto& name : names) {
            auto str = OSSharedPtr(OSString::withCString(name.c_str()), OSNoRetain);
            // Keep the index aligned with the channel index even for empty
            // labels; the audio side falls back to a synthesized name per slot.
            if (str) {
                array->setObject(str.get());
            }
        }
        properties->setObject(key, array.get());
    }
};

} // namespace ASFW::Audio::Model
