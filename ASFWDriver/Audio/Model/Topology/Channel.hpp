// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
// Ported from midi:ADKVirtualAudioLab/Core/AudioModel (AUAA semantic core, FW-202).
#pragma once

#include <cstdint>

#include "Id.hpp"
#include "Port.hpp"

#include <compare>
#include <string>
#include <vector>

namespace ASFW::AudioModel {

using ChannelId = Id<struct ChannelTag>;

struct Channel {
    ChannelId id;
    std::string name;
    std::vector<PortId> ports;

    auto operator<=>(const Channel&) const = default;
};

} // namespace ASFW::AudioModel
