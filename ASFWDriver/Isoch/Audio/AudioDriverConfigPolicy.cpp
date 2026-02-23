#include "AudioDriverConfig.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ASFW::Isoch::Audio {
namespace {

void AppendBoolControl(ParsedAudioDriverConfig& inOutConfig,
                       const BoolControlDescriptor& descriptor) {
    if (inOutConfig.boolControlCount >= kMaxBoolControls) {
        return;
    }
    inOutConfig.boolControls[inOutConfig.boolControlCount++] = descriptor;
}

} // namespace

void InitializeAudioDriverConfigDefaults(ParsedAudioDriverConfig& outConfig) {
    memset(&outConfig, 0, sizeof(outConfig));

    strlcpy(outConfig.deviceName, "FireWire Audio", sizeof(outConfig.deviceName));
    outConfig.channelCount = kDefaultChannelCount;
    outConfig.inputChannelCount = kDefaultChannelCount;
    outConfig.outputChannelCount = kDefaultChannelCount;

    outConfig.sampleRates[0] = kDefaultSampleRate;
    outConfig.sampleRateCount = 1;
    outConfig.currentSampleRate = kDefaultSampleRate;
    outConfig.streamMode = StreamMode::kNonBlocking;

    strlcpy(outConfig.inputPlugName, "Input", sizeof(outConfig.inputPlugName));
    strlcpy(outConfig.outputPlugName, "Output", sizeof(outConfig.outputPlugName));

    for (uint32_t i = 0; i < kMaxNamedChannels; ++i) {
        snprintf(outConfig.inputChannelNames[i], sizeof(outConfig.inputChannelNames[i]), "In %u", i + 1);
        snprintf(outConfig.outputChannelNames[i], sizeof(outConfig.outputChannelNames[i]), "Out %u", i + 1);
    }
}

void BuildFallbackBoolControls(ParsedAudioDriverConfig& inOutConfig) {
    if (inOutConfig.boolControlCount != 0 || !inOutConfig.hasPhantomOverride) {
        return;
    }

    const uint32_t mask = inOutConfig.phantomSupportedMask;
    for (uint32_t bit = 0; bit < 32; ++bit) {
        const uint32_t flag = 1u << bit;
        if ((mask & flag) == 0u) {
            continue;
        }

        const BoolControlDescriptor descriptor{
            .classIdFourCC = kClassIdPhantomPower,
            .scopeFourCC = kScopeInput,
            .element = bit + 1u,
            .isSettable = true,
            .initialValue = (inOutConfig.phantomInitialMask & flag) != 0u,
        };
        AppendBoolControl(inOutConfig, descriptor);
    }
}

void ApplyBringupSingleFormatPolicy(ParsedAudioDriverConfig& inOutConfig) {
    // Bring-up note: dynamic sample-rate advertisement is intentionally deferred.
    inOutConfig.sampleRates[0] = kDefaultSampleRate;
    inOutConfig.sampleRateCount = 1;
    inOutConfig.currentSampleRate = kDefaultSampleRate;
}

void ClampAudioDriverChannels(ParsedAudioDriverConfig& inOutConfig,
                              uint32_t maxSupportedChannels) {
    if (inOutConfig.inputChannelCount == 0) {
        inOutConfig.inputChannelCount = inOutConfig.channelCount;
    } else if (inOutConfig.inputChannelCount > maxSupportedChannels) {
        inOutConfig.inputChannelCount = maxSupportedChannels;
    }
    if (inOutConfig.outputChannelCount == 0) {
        inOutConfig.outputChannelCount = inOutConfig.channelCount;
    } else if (inOutConfig.outputChannelCount > maxSupportedChannels) {
        inOutConfig.outputChannelCount = maxSupportedChannels;
    }

    if (inOutConfig.inputChannelCount == 0) {
        inOutConfig.inputChannelCount = kDefaultChannelCount;
    }
    if (inOutConfig.outputChannelCount == 0) {
        inOutConfig.outputChannelCount = kDefaultChannelCount;
    }

    inOutConfig.channelCount = std::max(inOutConfig.inputChannelCount,
                                        inOutConfig.outputChannelCount);
}

const char* ScopeLabel(uint32_t scopeFourCC) {
    switch (scopeFourCC) {
        case static_cast<uint32_t>('inpt'):
            return "Input";
        case static_cast<uint32_t>('outp'):
            return "Output";
        case static_cast<uint32_t>('glob'):
            return "Global";
        default:
            return "Scope";
    }
}

} // namespace ASFW::Isoch::Audio
