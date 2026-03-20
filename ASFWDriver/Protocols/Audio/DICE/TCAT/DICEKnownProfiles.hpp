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
    // Focusrite Saffire Pro 24 DSP
    if (vendorId == 0x00130eU && modelId == 0x000008U) {
        outCaps.sampleRateHz = 48000;
        outCaps.hostInputPcmChannels = 16;
        outCaps.hostOutputPcmChannels = 8;
        outCaps.deviceToHostAm824Slots = 17;
        outCaps.hostToDeviceAm824Slots = 9;
        return true;
    }

    return false;
}

} // namespace ASFW::Audio::DICE::TCAT
