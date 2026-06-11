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
        auto guidNum = OSSharedPtr(OSNumber::withNumber(guid, 64), OSNoRetain);
        auto vendorIdNum = OSSharedPtr(OSNumber::withNumber(vendorId, 32), OSNoRetain);
        auto modelIdNum = OSSharedPtr(OSNumber::withNumber(modelId, 32), OSNoRetain);

        if (!deviceNameStr || !guidNum || !vendorIdNum || !modelIdNum) {
            return false;
        }

        properties->setObject(PropertyKeys::kDeviceName, deviceNameStr.get());
        properties->setObject(PropertyKeys::kGuid, guidNum.get());
        properties->setObject(PropertyKeys::kVendorId, vendorIdNum.get());
        properties->setObject(PropertyKeys::kModelId, modelIdNum.get());

        return true;
    }
};

} // namespace ASFW::Audio::Model
