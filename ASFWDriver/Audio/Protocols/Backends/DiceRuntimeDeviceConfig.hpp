// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceRuntimeDeviceConfig.hpp - Publish discovered DICE geometry to the audio endpoint model

#pragma once

#include "../../Model/ASFWAudioDevice.hpp"
#include "../AudioTypes.hpp"

#include <algorithm>

namespace ASFW::Audio {

// DICE stream geometry is discovered from the device's TX/RX sections. Keep
// that runtime result as the source of truth for the HAL-facing endpoint rather
// than replacing it with the generic profile's stereo fallback.
[[nodiscard]] inline bool ApplyDiceRuntimeCapsToDeviceConfig(
    const AudioStreamRuntimeCaps& caps,
    Model::ASFWAudioDevice& config) {
    if (caps.sampleRateHz == 0 || caps.hostInputPcmChannels == 0 ||
        caps.hostOutputPcmChannels == 0) {
        return false;
    }

    config.inputChannelCount = caps.hostInputPcmChannels;
    config.outputChannelCount = caps.hostOutputPcmChannels;
    config.channelCount = std::max(config.inputChannelCount, config.outputChannelCount);
    config.currentSampleRate = caps.sampleRateHz;
    config.sampleRates.assign(1, caps.sampleRateHz);
    return true;
}

} // namespace ASFW::Audio
