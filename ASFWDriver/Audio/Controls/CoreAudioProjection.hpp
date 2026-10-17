// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../Model/Topology/State.hpp"
#include "../Model/Topology/Topology.hpp"
#include <optional>
#include <vector>

namespace ASFW::Audio::Controls {
enum class CoreAudioControlKind : uint8_t { Level, Boolean, Selector };
struct CoreAudioControl {
    AudioModel::ParameterId parameter;
    CoreAudioControlKind kind{};
    uint32_t classId{}, scope{}, element{};
    bool settable{};
    std::string name;
    AudioModel::ParameterDomain domain;
    AudioModel::ParameterValue value;
    bool normalized{false};
};
struct InputTypeChoice {
    int64_t value{};
    std::string name;
    AudioModel::RouteBundleId bundle;
    AudioModel::PortId source;
    std::optional<AudioModel::ParameterId> levelParameter;
    std::optional<int64_t> levelValue;
};
struct CompositeInputType {
    AudioModel::NodeId router;
    AudioModel::PortId output;
    uint32_t element{};
    std::vector<InputTypeChoice> choices;
    std::optional<int64_t> selected;
};
/// The upper half of the ID space is reserved for projection-owned controls.
[[nodiscard]] constexpr AudioModel::ParameterId InputTypeParameterId(AudioModel::NodeId router) {
    return AudioModel::ParameterId{router.value | 0x80000000u};
}
/// A choice is a transaction: set the inactive connector's nominal level,
/// then select its bundle. Provider callers preserve that order.
[[nodiscard]] std::vector<CompositeInputType> ProjectInputTypes(
    const AudioModel::Topology&, const AudioModel::DeviceState&);
/// Pure policy: no device addresses, protocol tokens, or DriverKit pointers.
[[nodiscard]] std::vector<CoreAudioControl> ProjectCoreAudioControls(
    const AudioModel::Topology&, const AudioModel::DeviceState&);
/// Linear 0..1 control position in the currently applicable scalar domain.
/// This is a position translator, not a dB-to-amplitude conversion.
[[nodiscard]] std::optional<double> ScalarToPosition(const AudioModel::ScalarDomain&, double);
[[nodiscard]] std::optional<double> PositionToScalar(const AudioModel::ScalarDomain&, double);
} // namespace ASFW::Audio::Controls
