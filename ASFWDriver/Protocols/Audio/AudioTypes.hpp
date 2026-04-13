// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// AudioTypes.hpp - Shared protocol-facing audio topology/runtime types

#pragma once

#include <cstdint>

namespace ASFW::Audio {

struct AudioStreamRuntimeCaps {
    // Host-facing channel counts (PCM only).
    uint32_t hostInputPcmChannels{0};   // Device -> host capture channels
    uint32_t hostOutputPcmChannels{0};  // Host -> device playback channels

    // Wire-slot counts (AM824 data block slots) when known.
    uint32_t deviceToHostAm824Slots{0}; // DICE TX stream slots (capture wire format)
    uint32_t hostToDeviceAm824Slots{0}; // DICE RX stream slots (playback wire format)

    uint32_t sampleRateHz{0};
};

struct AudioDuplexChannels {
    uint8_t deviceToHostIsoChannel{0};  // DICE TX / host IR
    uint8_t hostToDeviceIsoChannel{1};  // DICE RX / host IT
};

} // namespace ASFW::Audio
