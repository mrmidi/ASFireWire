// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceRuntimeDeviceConfig.hpp - Publish discovered DICE geometry to the audio endpoint model

#pragma once

#include "../../Model/ASFWAudioDevice.hpp"
#include "../AudioTypes.hpp"
#include "../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"

#include <algorithm>

namespace ASFW::Audio {

// DICE stream geometry is discovered from the device's TX/RX sections. Keep
// that runtime result as the source of truth for the HAL-facing endpoint rather
// than replacing it with the generic profile's stereo fallback. A zero input
// count is valid when a profile deliberately hides a physical DICE return
// stream from CoreAudio (e.g. Weiss INT202); a zero output remains unusable.
[[nodiscard]] inline bool ApplyDiceRuntimeCapsToDeviceConfig(
    const AudioStreamRuntimeCaps& caps,
    Model::ASFWAudioDevice& config) {
    if (caps.sampleRateHz == 0 || caps.hostOutputPcmChannels == 0) {
        return false;
    }

    config.inputChannelCount = caps.hostInputPcmChannels;
    config.outputChannelCount = caps.hostOutputPcmChannels;
    config.channelCount = std::max(config.inputChannelCount, config.outputChannelCount);

    // Channel geometry is adopted from the device; the clock is NOT.
    //
    // `caps.sampleRateHz` is whatever rate the device happened to be sitting at
    // when its stream caps were read -- an observation, not a capability. Two
    // things used to be derived from it and both were wrong:
    //
    //   * `sampleRates.assign(1, ...)` collapsed the advertised list to that one
    //     rate. Since AudioNubPublisher::EnsureNub is create-once, whichever
    //     value reached the first publish became permanent, and every other rate
    //     was then refused by ASFWAudioDevice's rate validation. Whether that
    //     happened at all depended on a race with EnsureRuntimeStreamGeometry,
    //     so the same code produced a switchable device or a locked one
    //     depending on how quickly caps loaded.
    //
    //   * adopting it as `currentSampleRate` disagreed with bring-up, which
    //     programs 48 kHz internal (DICEDuplexBringupController). A device found
    //     at 44.1 kHz was published to CoreAudio as 44.1 kHz and then driven at
    //     48 kHz.
    //
    // The caller seeds both fields before this runs: the profile's supported
    // rate set, and 48 kHz as the DICE default that bring-up will actually
    // program. Leave them alone. Rate changes go through the HAL path
    // (HandleChangeSampleRate -> RequestSampleRateChange), which reprograms
    // CLOCK_SELECT and is the only place the device's clock should move.
    return true;
}

enum class DicePublicationConfigResult {
    kDefer,
    kProfileFallback,
    kRuntimeGeometry,
};

// Preserve established DICE publication on missing/failed discovery: available
// runtime caps enrich the profile, but a transient read failure must not prevent
// other DICE devices from publishing. Only the exact FireStudio Project profile
// requires successfully discovered wire geometry before its first publication.
// This prepares a candidate config; deferring does not remove an existing nub.
[[nodiscard]] inline DicePublicationConfigResult PrepareDiceDeviceConfigForPublication(
    const AudioStreamRuntimeCaps* caps,
    bool geometryReadSucceeded,
    Model::ASFWAudioDevice& config) {
    const bool requiresRuntimeGeometry =
        config.vendorId == DeviceProfiles::Audio::kPreSonusVendorId &&
        config.modelId == DeviceProfiles::Audio::kFireStudioProjectModelId;
    if (requiresRuntimeGeometry &&
        (!geometryReadSucceeded || !caps ||
         caps->deviceToHostAm824Slots == 0 || caps->hostToDeviceAm824Slots == 0 ||
         caps->deviceToHostStreamCount == 0 ||
         caps->deviceToHostStreamCount > kMaxAudioStreamsPerDirection ||
         caps->hostToDeviceStreamCount == 0 ||
         caps->hostToDeviceStreamCount > kMaxAudioStreamsPerDirection)) {
        return DicePublicationConfigResult::kDefer;
    }

    if (caps && ApplyDiceRuntimeCapsToDeviceConfig(*caps, config)) {
        return DicePublicationConfigResult::kRuntimeGeometry;
    }
    return requiresRuntimeGeometry ? DicePublicationConfigResult::kDefer
                                   : DicePublicationConfigResult::kProfileFallback;
}

} // namespace ASFW::Audio
