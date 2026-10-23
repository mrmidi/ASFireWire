// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ASFWAudioDevice.hpp"
#include "RateConfiguration.hpp"
#include "../Protocols/AudioTypes.hpp"
#include <optional>
#include <algorithm>
#include "../Wire/MOTU/MotuBlockLayout.hpp"
namespace ASFW::Audio {
[[nodiscard]] inline bool RuntimeCapsMatchConfiguration(
    const Model::ASFWAudioDevice& config, const AudioStreamRuntimeCaps& caps) noexcept {
    if (caps.sampleRateHz != config.currentSampleRate ||
        caps.hostInputPcmChannels != config.inputChannelCount ||
        caps.hostOutputPcmChannels != config.outputChannelCount) return false;
    const auto formation = std::ranges::find(config.rateFormationCandidates,
        config.currentSampleRate, &Runtime::RateFormation::sampleRateHz);
    if (formation != config.rateFormationCandidates.end() && formation->packedPcm) {
        return config.playbackStreams.size() == 1 && config.captureStreams.size() == 1 &&
            config.playbackStreams[0].am824Slots == Encoding::Motu::DataBlockQuadlets(caps.hostToDevicePcmChunks, formation->packedPlaybackMessageChunks) &&
            config.captureStreams[0].am824Slots == Encoding::Motu::DataBlockQuadlets(caps.deviceToHostPcmChunks, formation->packedCaptureMessageChunks) &&
            caps.hostToDevicePcmChunks == (config.playbackStreams[0].packedPcmChunks ? config.playbackStreams[0].packedPcmChunks : caps.hostOutputPcmChannels) &&
            caps.deviceToHostPcmChunks == (config.captureStreams[0].packedPcmChunks ? config.captureStreams[0].packedPcmChunks : caps.hostInputPcmChannels);
    }
    const auto matches = [](const auto& streams, uint32_t count, const auto& observed) {
        if (count != streams.size() || count > kMaxAudioStreamsPerDirection) return false;
        for (uint32_t i = 0; i < count; ++i)
            if (streams[i].pcmChannels != observed[i].pcmChannels ||
                streams[i].am824Slots != observed[i].am824Slots ||
                streams[i].midiPorts != observed[i].midiPorts) return false;
        return true;
    };
    return matches(config.playbackStreams, caps.hostToDeviceStreamCount, caps.hostToDeviceStreams) &&
        matches(config.captureStreams, caps.deviceToHostStreamCount, caps.deviceToHostStreams);
}

// Whether a hardware observation settles its rate for this endpoint: the clock
// is confirmed at a rate the catalog offers, and the registers match that
// rate's formation. A provisional formation has no layout to match yet; the
// registers read now are its layout (WithObservedRateCatalog).
[[nodiscard]] inline bool ObservationSettlesRate(const Model::ASFWAudioDevice& config,
                                                 const RateHardwareObservation& observed) noexcept {
    if (!observed.clockConfirmed) return false;
    const uint32_t rate = observed.caps.sampleRateHz;
    if (Model::LayoutIsProvisional(config, rate)) return true;
    const auto actual = Model::WithRateFormation(config, rate);
    return actual && RuntimeCapsMatchConfiguration(*actual, observed.caps);
}

[[nodiscard]] inline std::optional<AudioStreamRuntimeCaps> DiscoveredCaps(
    const Model::ASFWAudioDevice& config) noexcept {
    if (config.playbackStreams.size() != 1 || config.captureStreams.size() != 1 ||
        config.currentSampleRate == 0) {
        return std::nullopt;
    }
    const auto& playback = config.playbackStreams.front();
    const auto& capture = config.captureStreams.front();
    AudioStreamRuntimeCaps caps{};
    caps.sampleRateHz = config.currentSampleRate;
    caps.hostInputPcmChannels = capture.pcmChannels;
    caps.hostOutputPcmChannels = playback.pcmChannels;
    caps.deviceToHostPcmChunks = capture.packedPcmChunks;
    caps.hostToDevicePcmChunks = playback.packedPcmChunks;
    caps.deviceToHostAm824Slots = capture.am824Slots;
    caps.hostToDeviceAm824Slots = playback.am824Slots;
    caps.deviceToHostStreamCount = caps.hostToDeviceStreamCount = 1;
    caps.deviceToHostStreams[0] = {.pcmChannels = static_cast<uint16_t>(capture.pcmChannels),
                                   .am824Slots = static_cast<uint16_t>(capture.am824Slots)};
    caps.hostToDeviceStreams[0] = {.pcmChannels = static_cast<uint16_t>(playback.pcmChannels),
                                   .am824Slots = static_cast<uint16_t>(playback.am824Slots)};
    return caps;
}

}
