// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcControlProjection.hpp - the AV/C discovery graph as semantic topology (FW-204).
//
// A pure function of one immutable DeviceGraph. The graph stays the only
// authority: a confirmed write republishes the graph (AVCUnit::
// RememberConfirmedFeature), and the next projection reads the new value. No
// control state is copied anywhere else.
//
// Mapping (AUAA §8-§15):
//   feature function block  -> ProcessorNode, one input and one output port per
//                              channel; master (channel 0) parameters target the
//                              node, channel parameters target that channel's
//                              output port
//   feature volume / mute   -> Level (dB scalar) / Mute (boolean) parameters,
//                              range and resolution from the graph
//   selector function block -> RouterNode, one input port per declared input,
//                              one bundle per input; the STATUS-confirmed
//                              current input is the active bundle
//   STATUS-confirmed values -> StateOrigin::Observed
//
// Confirmed stream boundaries and descriptor-declared feature/selector links
// are projected when their channel geometry agrees. Unknown physical endpoints
// and undecoded processing blocks remain absent rather than guessed.
#pragma once

#include "../Model/AvcVolumeMapping.hpp"
#include "../Model/Topology/State.hpp"
#include "../Model/Topology/Topology.hpp"
#include "../../Protocols/AVC/Graph/AvcDeviceGraph.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace ASFW::Audio::Controls {

enum class AvcBindingKind : uint8_t {
    kFeatureVolume,
    kFeatureMute,
};

/// How one parameter reaches the hardware. Not part of the published topology.
struct AvcParameterBinding {
    AudioModel::ParameterId parameter;
    AvcBindingKind kind{AvcBindingKind::kFeatureVolume};
    uint8_t subunit{0};
    uint8_t block{0};
    uint8_t channel{0};
    Model::AvcVolumeRange range{}; ///< Volume only.

    /// The token the CoreAudio control path already uses for this channel.
    [[nodiscard]] uint32_t Token() const noexcept {
        return Model::AvcControlToken(subunit, block, channel);
    }
};

struct AvcControlModel {
    AudioModel::Topology topology;
    AudioModel::DeviceState state;
    std::vector<AvcParameterBinding> bindings;

    [[nodiscard]] const AvcParameterBinding* FindBinding(AudioModel::ParameterId id) const noexcept {
        for (const auto& binding : bindings)
            if (binding.parameter == id) return &binding;
        return nullptr;
    }
};

/// Parameter ids are derived from the AV/C address, so they stay stable for
/// as long as the graph describes the same function block and channel.
[[nodiscard]] constexpr AudioModel::ParameterId AvcParameterId(uint8_t subunit, uint8_t block,
                                                               uint8_t channel,
                                                               AvcBindingKind kind) noexcept {
    return AudioModel::ParameterId{(Model::AvcControlToken(subunit, block, channel) << 1) |
                                   static_cast<uint32_t>(kind)};
}

/// `revision` scopes every id in the result: callers pass the discovery
/// session, so a rediscovered graph produces a new revision.
[[nodiscard]] AvcControlModel ProjectAvcControls(const Protocols::AVC::Graph::DeviceGraph& graph,
                                                 uint64_t revision);

} // namespace ASFW::Audio::Controls
