// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Control-plane ownership for resolved audio endpoints. Runtime instance IDs,
// not observed Config-ROM GUIDs, are the only lookup keys.

#pragma once

#include "../Devices/ResolvedAudioEndpointProfile.hpp"
#include "../Runtime/AudioTelemetrySnapshot.hpp"
#include "../Shared/AudioRuntimeTuning.hpp"
#include "../Shared/Configuration/DeviceConfigurationSnapshot.hpp"
#include "../Shared/Topology/IAudioSemanticMatrix.hpp"
#include "../Shared/Topology/IAudioSemanticTopology.hpp"

#include <DriverKit/IOLib.h>

#include <map>
#include <memory>

namespace ASFW::Audio {

class IDeviceProtocol;
class AudioEndpointRuntime;

class AudioRuntimeRegistry final {
public:
    AudioRuntimeRegistry() noexcept;
    ~AudioRuntimeRegistry() noexcept;

    AudioRuntimeRegistry(const AudioRuntimeRegistry&) = delete;
    AudioRuntimeRegistry& operator=(const AudioRuntimeRegistry&) = delete;

    [[nodiscard]] std::shared_ptr<IDeviceProtocol>
    FindShared(Devices::AudioEndpointId endpointId) noexcept;
    [[nodiscard]] std::shared_ptr<AudioEndpointRuntime>
    FindEndpointRuntime(Devices::AudioEndpointId endpointId) noexcept;
    [[nodiscard]] std::shared_ptr<const Devices::ResolvedAudioEndpointProfile>
    FindProfile(Devices::AudioEndpointId endpointId) noexcept;

    // Atomically installs the immutable profile and optional protocol hold,
    // returning the endpoint-owned neutral binding source.
    [[nodiscard]] std::shared_ptr<AudioEndpointRuntime> InsertResolved(
        std::shared_ptr<const Devices::ResolvedAudioEndpointProfile> profile,
        std::shared_ptr<IDeviceProtocol> protocol) noexcept;

    [[nodiscard]] uint32_t CopyAudioTelemetrySnapshots(
        Runtime::AudioTelemetrySnapshot& out) noexcept;
    // Lists only endpoints that advertise semantic configuration capabilities.
    // This is intentionally independent from streaming telemetry: an endpoint
    // can be configurable before any stream binding or telemetry is active.
    [[nodiscard]] uint32_t CopyConfigurationEndpointIds(
        std::array<Devices::AudioEndpointId,
                   Configuration::kMaxConfigurationSnapshotCapabilities>& out) noexcept;
    /// Lists every endpoint the driver has resolved a runtime for, which is the
    /// set that has runtime tuning and geometry to report. Deliberately not the
    /// configuration list: that one additionally requires rate/optical
    /// capabilities, so a device with none -- a Saffire Pro 24 DSP, say --
    /// enumerated empty and the geometry panel said no endpoint was published
    /// while its telemetry was streaming beside it.
    [[nodiscard]] uint32_t CopyRuntimeTuningEndpointIds(
        std::array<Devices::AudioEndpointId,
                   Shared::kMaxAudioRuntimeTuningEndpoints>& out) noexcept;
    /// Lists only endpoints whose protocol publishes a semantic topology. This
    /// is deliberately separate from configuration-capability discovery: a
    /// mixer-only device such as Duet need not expose rate/optical controls.
    [[nodiscard]] uint32_t CopySemanticTopologyEndpointIds(
        std::array<Devices::AudioEndpointId,
                   kMaxAudioSemanticTopologyEndpoints>& out) noexcept;
    /// Lists only endpoints whose protocol publishes a semantic mixer matrix.
    /// A matrix-only device does not need rate/optical configuration support
    /// and may not publish a full signal graph.
    [[nodiscard]] uint32_t CopySemanticMatrixEndpointIds(
        std::array<Devices::AudioEndpointId,
                   kMaxAudioSemanticMatrixEndpoints>& out) noexcept;

    void Remove(Devices::AudioEndpointId endpointId) noexcept;
    void Clear() noexcept;

private:
    struct Entry final {
        std::shared_ptr<IDeviceProtocol> protocol;
        std::shared_ptr<AudioEndpointRuntime> runtime;
        std::shared_ptr<const Devices::ResolvedAudioEndpointProfile> profile;
    };

    IOLock* lock_{nullptr};
    std::map<Devices::AudioEndpointId, Entry> endpoints_;
};

} // namespace ASFW::Audio
