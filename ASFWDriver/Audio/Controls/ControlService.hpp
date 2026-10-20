// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "IControlProvider.hpp"
#include <cstdint>
#include <memory>
namespace ASFW::Protocols::AVC { class IAVCDiscovery; }
namespace ASFW::Audio { class IDeviceProtocol; }
namespace ASFW::Audio::Controls {
/// The shared binding entry point for CoreAudio and user-client writes.
/// Call only on the controller queue. No transport/AudioDriverKit knowledge.
class ControlService {
public:
    using ProtocolLookup = std::function<std::shared_ptr<IDeviceProtocol>(uint64_t)>;
    ControlService(Protocols::AVC::IAVCDiscovery* discovery, ProtocolLookup lookup = {})
        : discovery_(discovery), lookup_(std::move(lookup)) {}
    [[nodiscard]] std::optional<ControlSnapshot> CopySnapshot(uint64_t guid) const;
    [[nodiscard]] std::optional<AudioModel::ParameterValue> ReadParameter(uint64_t guid, AudioModel::ParameterId) const;
    void SetRoute(uint64_t guid, uint64_t revision, AudioModel::NodeId, AudioModel::PortId,
                  std::optional<AudioModel::PortId>, IControlProvider::Completion) const;
    /// `active` reports whether the caller still waits for the answer. A write
    /// still queued when it turns false is not sent (a CoreAudio wait that
    /// timed out must not reach the device late).
    void SetParameter(uint64_t guid, uint64_t revision, AudioModel::ParameterId,
                      const AudioModel::ParameterValue&, IControlProvider::Completion,
                      std::function<bool()> active = {}) const;
private:
    Protocols::AVC::IAVCDiscovery* discovery_;
    ProtocolLookup lookup_;
};
} // namespace ASFW::Audio::Controls
