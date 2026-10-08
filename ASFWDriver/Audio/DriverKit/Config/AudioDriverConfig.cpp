#include "AudioDriverConfig.hpp"

#include "../../Model/AudioPropertyKeys.hpp"
#include "../../../Logging/Logging.hpp"

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
    if (auto* builder =
            OSDynamicCast(OSNumber, properties->getObject(Keys::kProfileBuilderId))) {
        inOutConfig.profileBuilderId = builder->unsigned32BitValue();
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

// Parse one direction's per-stream geometry. An entry missing its PCM count is
// not describable, so the whole array is rejected rather than partially
// accepted: a half-read geometry would silently mis-frame the streams it did
// read, which is the failure this property exists to prevent.
void ParseWireStreams(OSDictionary* properties,
                      const char* key,
                      ParsedWireStream (&outStreams)[kMaxConfiguredStreams],
                      uint32_t& outCount) {
    outCount = 0;
    auto* array = OSDynamicCast(OSArray, properties->getObject(key));
    if (array == nullptr) {
        return;
    }

    const uint32_t published = array->getCount();
    if (published > kMaxConfiguredStreams) {
        ASFW_LOG(Audio,
                 "AudioDriverConfig: %{public}s publishes %u streams, more than this build "
                 "can configure (%u) - ignoring the whole array rather than truncating it",
                 key, published, kMaxConfiguredStreams);
        return;
    }

    ParsedWireStream parsed[kMaxConfiguredStreams]{};
    for (uint32_t i = 0; i < published; ++i) {
        auto* entry = OSDynamicCast(OSDictionary, array->getObject(i));
        if (entry == nullptr) {
            ASFW_LOG(Audio, "AudioDriverConfig: %{public}s entry %u is not a dictionary", key, i);
            return;
        }
        auto* pcm = OSDynamicCast(OSNumber, entry->getObject(Keys::kStreamPcmChannels));
        if (pcm == nullptr || pcm->unsigned32BitValue() == 0) {
            ASFW_LOG(Audio, "AudioDriverConfig: %{public}s entry %u states no PCM channels", key, i);
            return;
        }
        parsed[i].pcmChannels = pcm->unsigned32BitValue();
        if (auto* slots =
                OSDynamicCast(OSNumber, entry->getObject(Keys::kStreamAm824Slots))) {
            parsed[i].am824Slots = slots->unsigned32BitValue();
        }
        if (auto* midi =
                OSDynamicCast(OSNumber, entry->getObject(Keys::kStreamMidiPorts))) {
            parsed[i].midiPorts = midi->unsigned32BitValue();
        }
        if (auto* offset =
                OSDynamicCast(OSNumber, entry->getObject(Keys::kStreamChannelOffset))) {
            parsed[i].channelOffset = offset->unsigned32BitValue();
        }
        if (auto* map = OSDynamicCast(OSArray, entry->getObject(Keys::kStreamPcmSlotMap))) {
            parsed[i].hasPcmSlotMap = true;
            const auto count = map->getCount();
            if (count > parsed[i].pcmSlotMap.slotForChannel.size() ||
                (count != 0 && count != parsed[i].pcmChannels)) return;
            parsed[i].pcmSlotMap.slotCount = count;
            parsed[i].pcmSlotMap.channelCount = count;
            for (uint32_t channel = 0; channel < count; ++channel) {
                auto* slot = OSDynamicCast(OSNumber, map->getObject(channel));
                if (slot == nullptr || slot->unsigned32BitValue() >= parsed[i].am824Slots) return;
                parsed[i].pcmSlotMap.slotForChannel[channel] = static_cast<uint8_t>(slot->unsigned32BitValue());
            }
        }
        // A data block has to be at least as wide as the PCM it carries.
        if (parsed[i].am824Slots == 0) {
            parsed[i].am824Slots = parsed[i].pcmChannels + parsed[i].midiPorts;
        }
    }

    for (uint32_t i = 0; i < published; ++i) {
        outStreams[i] = parsed[i];
    }
    outCount = published;
}

void ParseAudioDriverConfigFromProperties(OSDictionary* properties,
                                          ParsedAudioDriverConfig& inOutConfig) {
    if (!properties) {
        return;
    }

    if (auto* controls = OSDynamicCast(OSArray, properties->getObject(::ASFW::Audio::Model::kAvcControlsProperty))) {
        inOutConfig.avcControlCount = 0;
        for (uint32_t i = 0; i < std::min(controls->getCount(), ::ASFW::Audio::Model::kMaxAvcControls); ++i) {
            auto* entry = OSDynamicCast(OSDictionary, controls->getObject(i));
            if (!entry) continue;
            const auto get = [&](const char* key) -> uint32_t {
                auto* value = OSDynamicCast(OSNumber, entry->getObject(key));
                return value != nullptr ? value->unsigned32BitValue() : 0;
            };
            auto& control = inOutConfig.avcControls[inOutConfig.avcControlCount++];
            if (auto* name = OSDynamicCast(OSString, entry->getObject("Name")))
                strlcpy(control.name, name->getCStringNoCopy(), sizeof(control.name));
            control.token = get("Token"); control.scope = get("Scope"); control.element = get("Element");
            control.hasMute = get("HasMute") != 0; control.muted = get("Muted") != 0;
            control.hasVolume = get("HasVolume") != 0; control.current = static_cast<int16_t>(get("Current"));
            control.range = {static_cast<int16_t>(get("Minimum")), static_cast<int16_t>(get("Maximum")), static_cast<int16_t>(get("Resolution"))};
            if (!control.range.Valid()) control.hasVolume = false;
        }
    }
    ParseIdentityProperties(properties, inOutConfig);
    ParseDevicePresentationProperties(properties, inOutConfig);
    ParseSampleRates(properties, inOutConfig);
    if (auto* value = OSDynamicCast(OSNumber, properties->getObject(Keys::kRateRouteIncarnation)))
        inOutConfig.rateRouteIncarnation = value->unsigned64BitValue();
    if (auto* value = OSDynamicCast(OSNumber, properties->getObject(Keys::kRateRouteEpoch)))
        inOutConfig.rateRouteEpoch = value->unsigned64BitValue();
    if (auto* value = OSDynamicCast(OSNumber, properties->getObject(Keys::kRateBusGeneration)))
        inOutConfig.rateBusGeneration = value->unsigned32BitValue();
    if (auto* value = OSDynamicCast(OSNumber, properties->getObject(Keys::kUsesRateFormations)))
        inOutConfig.usesRateFormations = value->unsigned32BitValue() != 0;
    inOutConfig.rateFormationCandidates.clear();
    if (auto* catalog = OSDynamicCast(OSArray, properties->getObject(Keys::kRateFormations))) {
        std::vector<::ASFW::Audio::Runtime::RateFormation> parsed;
        bool valid = catalog->getCount() <= 7;
        for (uint32_t i = 0; valid && i < catalog->getCount(); ++i) {
            auto* entry = OSDynamicCast(OSDictionary, catalog->getObject(i));
            if (!entry) { valid = false; break; }
            auto* rate = OSDynamicCast(OSNumber, entry->getObject(Keys::kCurrentSampleRate));
            auto* mode = OSDynamicCast(OSNumber, entry->getObject(Keys::kStreamMode));
            auto* protocol = OSDynamicCast(OSNumber, entry->getObject(Keys::kFormationProtocolSupported));
            auto* validated = OSDynamicCast(OSNumber, entry->getObject(Keys::kFormationHardwareValidated));
            if (rate == nullptr || mode == nullptr || protocol == nullptr || validated == nullptr || mode->unsigned32BitValue() > 1 ||
                !::ASFW::Encoding::AmdtpRateGeometryForSampleRate(rate->unsigned32BitValue()) ||
                std::ranges::any_of(parsed, [&](const auto& f) { return f.sampleRateHz == rate->unsigned32BitValue(); })) {
                valid = false; break;
            }
            ParsedWireStream playback[kMaxConfiguredStreams]{}, capture[kMaxConfiguredStreams]{};
            uint32_t playbackCount = 0, captureCount = 0;
            ParseWireStreams(entry, Keys::kPlaybackStreams, playback, playbackCount);
            ParseWireStreams(entry, Keys::kCaptureStreams, capture, captureCount);
            if (!playbackCount || !captureCount) { valid = false; break; }
            ::ASFW::Audio::Runtime::RateFormation formation{};
            formation.sampleRateHz = rate->unsigned32BitValue();
            formation.mode = mode->unsigned32BitValue() == 1
                ? ::ASFW::Encoding::StreamMode::kBlocking : ::ASFW::Encoding::StreamMode::kNonBlocking;
            formation.protocolSupported = protocol->unsigned32BitValue() != 0;
            formation.hardwareValidated = validated->unsigned32BitValue() != 0;
            if (auto* packed = OSDynamicCast(OSNumber, entry->getObject("ASFWMotuPackedPcm")))
                formation.packedPcm = packed->unsigned32BitValue() != 0;
            for (uint32_t j = 0; j < playbackCount; ++j)
                formation.playback.push_back({playback[j].pcmChannels, playback[j].am824Slots,
                    inOutConfig.usesRateFormations ? (playback[j].midiPorts ? 1U : 0U) : playback[j].midiPorts,
                    playback[j].pcmSlotMap, inOutConfig.usesRateFormations ? playback[j].midiPorts : 0U});
            for (uint32_t j = 0; j < captureCount; ++j)
                formation.capture.push_back({capture[j].pcmChannels, capture[j].am824Slots,
                    inOutConfig.usesRateFormations ? (capture[j].midiPorts ? 1U : 0U) : capture[j].midiPorts,
                    capture[j].pcmSlotMap, inOutConfig.usesRateFormations ? capture[j].midiPorts : 0U});
            parsed.push_back(std::move(formation));
        }
        if (valid) inOutConfig.rateFormationCandidates = std::move(parsed);
        else ASFW_LOG(Audio, "AudioDriverConfig: rejected incomplete/ambiguous rate formation catalog");
    }
    ParsePlugNames(properties, inOutConfig);
    ParseChannelNames(properties, inOutConfig);
    ParseWireStreams(properties, Keys::kPlaybackStreams,
                     inOutConfig.playbackStreams, inOutConfig.playbackStreamCount);
    ParseWireStreams(properties, Keys::kCaptureStreams,
                     inOutConfig.captureStreams, inOutConfig.captureStreamCount);
    if (auto* required =
            OSDynamicCast(OSNumber, properties->getObject(Keys::kResolvedGeometryRequired))) {
        inOutConfig.resolvedGeometryRequired = required->unsigned32BitValue() != 0;
    }
    if (auto* fromDevice =
            OSDynamicCast(OSNumber, properties->getObject(Keys::kDeviceSampleRates))) {
        inOutConfig.deviceSampleRates = fromDevice->unsigned32BitValue() != 0;
    }
    BuildChannelNamesFromPlugs(inOutConfig);
}

} // namespace ASFW::Isoch::Audio
