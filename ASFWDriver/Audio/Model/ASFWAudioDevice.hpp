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

/// One isochronous stream's resolved wire shape, as published to the audio side.
///
/// `channelOffset` is the running sum of the PCM channel counts of the streams
/// before this one. It is NOT index * width-of-stream-0: those coincide only
/// while every stream is the same width, which is exactly the assumption the
/// geometry resolver removed.
struct ASFWAudioWireStream {
    uint32_t pcmChannels{0};
    uint32_t am824Slots{0};
    uint32_t midiPorts{0};
    uint32_t channelOffset{0};

    friend bool operator==(const ASFWAudioWireStream&,
                           const ASFWAudioWireStream&) noexcept = default;
};

struct ASFWAudioDevice {
    uint64_t guid{0};
    uint32_t vendorId{0};
    uint32_t modelId{0};
    /// The device catalog's resolved ProfileBuilderId, as a raw uint32 so this
    /// struct stays free of the DeviceProfiles headers. Zero means unresolved,
    /// and the audio side then falls back to matching on (vendorId, modelId) --
    /// loudly, because that pair cannot identify every family.
    uint32_t profileBuilderId{0};
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

    // Resolved per-stream wire geometry, in stream order. Empty means the
    // publisher had none to offer and the audio side must fall back to its
    // profile constants -- which is what every device did before this existed,
    // and is wrong for any device whose streams are not all the same width.
    std::vector<ASFWAudioWireStream> playbackStreams{};  // host -> device (DICE RX)
    std::vector<ASFWAudioWireStream> captureStreams{};   // device -> host (DICE TX)

    /// The audio side must use the resolved geometry above and must NOT fall
    /// back to profile constants. Set by families that always resolve before
    /// publishing (DICE); see PropertyKeys::kResolvedGeometryRequired.
    bool resolvedGeometryRequired{false};

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
        auto streamModeNum = OSSharedPtr(
            OSNumber::withNumber(static_cast<uint32_t>(streamMode), 32), OSNoRetain);

        if (!deviceNameStr || !channelCountNum || !guidNum || !vendorIdNum || !modelIdNum ||
            !inputChannelCountNum || !outputChannelCountNum || !sampleRatesArray ||
            !inputPlugNameStr || !outputPlugNameStr || !currentRateNum || !streamModeNum) {
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
        if (auto builderNum =
                OSSharedPtr(OSNumber::withNumber(profileBuilderId, 32), OSNoRetain)) {
            properties->setObject(PropertyKeys::kProfileBuilderId, builderNum.get());
        }
        properties->setObject(PropertyKeys::kInputChannelCount, inputChannelCountNum.get());
        properties->setObject(PropertyKeys::kOutputChannelCount, outputChannelCountNum.get());
        properties->setObject(PropertyKeys::kInputPlugName, inputPlugNameStr.get());
        properties->setObject(PropertyKeys::kOutputPlugName, outputPlugNameStr.get());
        properties->setObject(PropertyKeys::kCurrentSampleRate, currentRateNum.get());
        properties->setObject(PropertyKeys::kStreamMode, streamModeNum.get());

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

        // Resolved per-stream geometry. This is what lets the audio side frame
        // packets from what the device reported instead of from a compiled-in
        // constant, so it is the one property an asymmetric device cannot work
        // without -- hence a serialization failure fails the whole publication
        // rather than quietly producing a device missing its geometry.
        if (!PublishWireStreams(properties, PropertyKeys::kPlaybackStreams, playbackStreams) ||
            !PublishWireStreams(properties, PropertyKeys::kCaptureStreams, captureStreams)) {
            return false;
        }
        if (resolvedGeometryRequired) {
            auto required = OSSharedPtr(OSNumber::withNumber(uint64_t{1}, 32), OSNoRetain);
            if (!required) {
                return false;
            }
            properties->setObject(PropertyKeys::kResolvedGeometryRequired, required.get());
        }

        return true;
    }

private:
    [[nodiscard]] static bool PublishWireStreams(
        OSDictionary* properties,
        const char* key,
        const std::vector<ASFWAudioWireStream>& streams) {
        if (streams.empty()) {
            return true;
        }
        auto array = OSSharedPtr(
            OSArray::withCapacity(static_cast<uint32_t>(streams.size())), OSNoRetain);
        if (!array) {
            return false;
        }
        for (const auto& stream : streams) {
            auto entry = OSSharedPtr(OSDictionary::withCapacity(4), OSNoRetain);
            auto pcm = OSSharedPtr(OSNumber::withNumber(stream.pcmChannels, 32), OSNoRetain);
            auto slots = OSSharedPtr(OSNumber::withNumber(stream.am824Slots, 32), OSNoRetain);
            auto midi = OSSharedPtr(OSNumber::withNumber(stream.midiPorts, 32), OSNoRetain);
            auto offset = OSSharedPtr(OSNumber::withNumber(stream.channelOffset, 32), OSNoRetain);
            if (!entry || !pcm || !slots || !midi || !offset) {
                // A partial array describes a device that does not exist. The
                // caller turns this into a failed publication rather than a
                // published device with the wrong shape.
                return false;
            }
            entry->setObject(PropertyKeys::kStreamPcmChannels, pcm.get());
            entry->setObject(PropertyKeys::kStreamAm824Slots, slots.get());
            entry->setObject(PropertyKeys::kStreamMidiPorts, midi.get());
            entry->setObject(PropertyKeys::kStreamChannelOffset, offset.get());
            array->setObject(entry.get());
        }
        properties->setObject(key, array.get());
        return true;
    }

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
