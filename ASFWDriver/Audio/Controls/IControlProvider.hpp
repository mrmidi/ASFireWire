// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../Model/Topology/State.hpp"
#include "../Model/Topology/Topology.hpp"
#include <DriverKit/IOReturn.h>
#include <functional>
#include <optional>
#include <string>

namespace ASFW::Audio::Controls {
struct ControlSnapshot {
    AudioModel::Topology topology;
    AudioModel::DeviceState state;
    std::string family;
    uint64_t sequence{};
};
/// Controller-queue interface. The protocol owns its provider; every async
/// caller retains the protocol until completion. CopySnapshot never does I/O.
class IControlProvider {
public:
    using Completion = std::function<void(IOReturn, AudioModel::ParameterValue)>;
    virtual ~IControlProvider() = default;
    virtual void Prepare(std::function<void(IOReturn)> done) { done(kIOReturnSuccess); }
    [[nodiscard]] virtual std::optional<ControlSnapshot> CopySnapshot() const = 0;
    virtual void SetParameter(AudioModel::ParameterId, const AudioModel::ParameterValue&, Completion) = 0;
    virtual void SetRoute(AudioModel::NodeId, AudioModel::PortId, std::optional<AudioModel::PortId>, Completion done) {
        done(kIOReturnUnsupported, false);
    }
};
} // namespace ASFW::Audio::Controls
