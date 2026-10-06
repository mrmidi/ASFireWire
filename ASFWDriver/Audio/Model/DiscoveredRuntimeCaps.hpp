// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ASFWAudioDevice.hpp"
#include "../Protocols/AudioTypes.hpp"
#include <optional>
namespace ASFW::Audio {
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
