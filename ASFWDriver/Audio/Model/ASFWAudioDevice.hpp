//
// ASFWAudioDevice.hpp
// ASFWDriver
//
// Driver-side audio endpoint model used to configure ASFWAudioNub/ASFWAudioDriver.
//

#pragma once

#include <DriverKit/OSArray.h>
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

struct ASFWAudioDevice {
    uint64_t guid{0};
    std::string deviceName{"FireWire Audio"};
    uint32_t channelCount{2};
    std::vector<uint32_t> sampleRates{};
    uint32_t currentSampleRate{48000};
    std::string inputPlugName{"Input"};
    std::string outputPlugName{"Output"};
    StreamMode streamMode{StreamMode::kNonBlocking};

    // Populate properties consumed by ASFWAudioDriver.
    // Returns false only if required objects could not be created.
    bool PopulateNubProperties(OSDictionary* properties) const {
        if (!properties) {
            return false;
        }

        auto deviceNameStr = OSSharedPtr(OSString::withCString(deviceName.c_str()), OSNoRetain);
        auto channelCountNum = OSSharedPtr(OSNumber::withNumber(channelCount, 32), OSNoRetain);
        auto guidNum = OSSharedPtr(OSNumber::withNumber(guid, 64), OSNoRetain);
        auto sampleRatesArray = OSSharedPtr(
            OSArray::withCapacity(static_cast<uint32_t>(sampleRates.size())), OSNoRetain);
        auto inputPlugNameStr = OSSharedPtr(OSString::withCString(inputPlugName.c_str()), OSNoRetain);
        auto outputPlugNameStr = OSSharedPtr(OSString::withCString(outputPlugName.c_str()), OSNoRetain);
        auto currentRateNum = OSSharedPtr(OSNumber::withNumber(currentSampleRate, 32), OSNoRetain);
        auto streamModeNum = OSSharedPtr(
            OSNumber::withNumber(static_cast<uint32_t>(streamMode), 32), OSNoRetain);

        if (!deviceNameStr || !channelCountNum || !guidNum ||
            !sampleRatesArray || !inputPlugNameStr || !outputPlugNameStr ||
            !currentRateNum || !streamModeNum) {
            return false;
        }

        for (uint32_t rate : sampleRates) {
            auto rateNum = OSSharedPtr(OSNumber::withNumber(rate, 32), OSNoRetain);
            if (rateNum) {
                sampleRatesArray->setObject(rateNum.get());
            }
        }

        properties->setObject("ASFWDeviceName", deviceNameStr.get());
        properties->setObject("ASFWChannelCount", channelCountNum.get());
        properties->setObject("ASFWSampleRates", sampleRatesArray.get());
        properties->setObject("ASFWGUID", guidNum.get());
        properties->setObject("ASFWInputPlugName", inputPlugNameStr.get());
        properties->setObject("ASFWOutputPlugName", outputPlugNameStr.get());
        properties->setObject("ASFWCurrentSampleRate", currentRateNum.get());
        properties->setObject("ASFWStreamMode", streamModeNum.get());

        return true;
    }
};

} // namespace ASFW::Audio::Model
