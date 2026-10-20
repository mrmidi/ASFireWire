// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
// Ported from midi:ADKVirtualAudioLab/Core/AudioModel (AUAA semantic core, FW-202).
#pragma once

#include <cstdint>

#include "Id.hpp"
#include "Parameter.hpp"

#include <string>
#include <variant>

namespace ASFW::AudioModel {

enum class MeterSemantic : uint8_t {
    Level,
    Peak,
    Unknown,
};

/// A Meter represents a single scalar telemetry point (yielding 1 double in DeviceState).
/// Multi-channel metering is modeled as multiple individual Meter objects.
struct Meter {
    MeterId id;

    std::variant<NodeId, PortId> target;

    MeterSemantic semantic;
    ScalarDomain domain;

    std::string name;
};

} // namespace ASFW::AudioModel
