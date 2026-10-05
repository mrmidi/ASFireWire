// SPDX-License-Identifier: Apache-2.0
// Adapted from the ASFW midi configuration reducer; current-main ownership.
#pragma once

#include <cstdint>
#include <optional>
#include <memory>
#include "../ResolvedAudioConfiguration.hpp"

namespace ASFW::Configuration {

using SampleRate = uint32_t;

enum class OpticalMode : uint8_t {
    Adat,
    Spdif,
};

/// A hardware-facing audio configuration. It deliberately carries only
/// semantic controls; each endpoint resolves it into its own stream geometry.
struct DeviceConfiguration final {
    SampleRate sampleRate{48000};
    std::optional<OpticalMode> opticalInput;
    std::optional<OpticalMode> opticalOutput;
    // Set only by candidate resolution. Immutable across asynchronous stages.
    std::shared_ptr<const Audio::Runtime::ResolvedAudioConfiguration> resolved;
};

} // namespace ASFW::Configuration
