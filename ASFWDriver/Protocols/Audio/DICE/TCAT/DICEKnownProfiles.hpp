// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// DICEKnownProfiles.hpp - Temporary known-profile fallbacks for generic DICE devices

#pragma once

#include "../../IDeviceProtocol.hpp"

#include <cstdint>

namespace ASFW::Audio::DICE::TCAT {

[[nodiscard]] constexpr bool TryGetKnownDICEProfile(uint32_t vendorId,
                                                    uint32_t modelId,
                                                    AudioStreamRuntimeCaps& outCaps) noexcept {
    // Focusrite Saffire Pro 14
    if (vendorId == 0x00130eU && modelId == 0x000009U) {
        outCaps.sampleRateHz = 48000;
        outCaps.hostInputPcmChannels = 8;
        outCaps.hostOutputPcmChannels = 12;
        outCaps.deviceToHostAm824Slots = 9;
        outCaps.hostToDeviceAm824Slots = 13;
        outCaps.deviceToHostActiveStreams = 1;
        outCaps.hostToDeviceActiveStreams = 1;
        outCaps.deviceToHostIsoChannel = 1;
        outCaps.hostToDeviceIsoChannel = 0;
        return true;
    }

    // Focusrite Saffire Pro 24
    if (vendorId == 0x00130eU && modelId == 0x000007U) {
        outCaps.sampleRateHz = 48000;
        outCaps.hostInputPcmChannels = 16;
        outCaps.hostOutputPcmChannels = 8;
        outCaps.deviceToHostAm824Slots = 17;
        outCaps.hostToDeviceAm824Slots = 9;
        outCaps.deviceToHostActiveStreams = 1;
        outCaps.hostToDeviceActiveStreams = 1;
        outCaps.deviceToHostIsoChannel = 1;
        outCaps.hostToDeviceIsoChannel = 0;
        return true;
    }

    // Focusrite Saffire Pro 24 DSP
    if (vendorId == 0x00130eU && modelId == 0x000008U) {
        outCaps.sampleRateHz = 48000;
        outCaps.hostInputPcmChannels = 16;
        outCaps.hostOutputPcmChannels = 8;
        outCaps.deviceToHostAm824Slots = 17;
        outCaps.hostToDeviceAm824Slots = 9;
        outCaps.deviceToHostActiveStreams = 1;
        outCaps.hostToDeviceActiveStreams = 1;
        outCaps.deviceToHostIsoChannel = 1;
        outCaps.hostToDeviceIsoChannel = 0;
        return true;
    }

    // Alesis MultiMix FireWire local recording-stability profile.
    // The observed stream table exposes one active device-to-host stream with
    // 12 PCM/AM824 slots; a second 2-channel stream is present but disabled
    // with iso=-1, so it must not be advertised to CoreAudio.
    if (vendorId == 0x000595U && modelId == 0x000000U) {
        outCaps.sampleRateHz = 48000;
        outCaps.hostInputPcmChannels = 12;
        outCaps.hostOutputPcmChannels = 2;
        outCaps.deviceToHostAm824Slots = 12;
        outCaps.hostToDeviceAm824Slots = 2;
        outCaps.deviceToHostActiveStreams = 1;
        outCaps.hostToDeviceActiveStreams = 1;
        outCaps.deviceToHostIsoChannel = 1;
        outCaps.hostToDeviceIsoChannel = 0;
        return true;
    }

    return false;
}

} // namespace ASFW::Audio::DICE::TCAT
