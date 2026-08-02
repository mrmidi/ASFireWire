// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BeBoBMixerMap.hpp - Declarative BeBoB Function Block mixer description types.
//
// A MixerMap describes a device's internal audio routing (selector blocks) and
// gain staging (feature blocks) as static data. The BeBoBProtocol base executes
// selectors first, then features, to respect FB ordering dependencies. Phase88
// ships with its mixer muted at minimum volume; this data drives the unmute +
// max-volume workaround on stream start.

#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace ASFW::Audio::BeBoB {

struct SelectorRoute {
    uint8_t fbId;
    uint8_t value;
};

struct ChannelMute {
    uint8_t fbId;
    uint8_t channel;
    bool unmute;
};

struct ChannelVolume {
    uint8_t fbId;
    uint8_t channel;
    uint16_t value;
};

struct MixerMap {
    std::span<const SelectorRoute> selectors;
    std::span<const ChannelMute> mutes;
    std::span<const ChannelVolume> volumes;
};

} // namespace ASFW::Audio::BeBoB
