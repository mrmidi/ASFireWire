// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
// Ported from midi:ADKVirtualAudioLab/Core/AudioModel (AUAA semantic core, FW-202).
#pragma once

#include <cstdint>

#include "Id.hpp"

#include <compare>

namespace ASFW::AudioModel {

struct MixerCrosspoint {
    CrosspointId id;
    PortId input;
    PortId output;

    constexpr auto operator<=>(const MixerCrosspoint&) const = default;
};

} // namespace ASFW::AudioModel
