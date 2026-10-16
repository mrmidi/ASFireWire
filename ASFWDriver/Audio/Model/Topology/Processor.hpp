// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
// Ported from midi:ADKVirtualAudioLab/Core/AudioModel (AUAA semantic core, FW-202).
#pragma once

#include <cstdint>

#include "Id.hpp"

#include <vector>

namespace ASFW::AudioModel {

struct ProcessorNode {
    std::vector<PortId> inputs;
    std::vector<PortId> outputs;
};

} // namespace ASFW::AudioModel
