// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
// Ported from midi:ADKVirtualAudioLab/Core/AudioModel (AUAA semantic core, FW-202).
#pragma once

#include <cstdint>

#include "Id.hpp"

#include <compare>

namespace ASFW::AudioModel {

struct FixedLink {
    PortId source;
    PortId destination;

    constexpr auto operator<=>(const FixedLink&) const = default;
};

} // namespace ASFW::AudioModel
