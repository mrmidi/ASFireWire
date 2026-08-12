// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Control-plane ownership for resolved audio endpoints. Runtime instance IDs,
// not observed Config-ROM GUIDs, are the only lookup keys.

#pragma once

#include "../Devices/ResolvedAudioEndpointProfile.hpp"
#include "../Runtime/AudioTelemetrySnapshot.hpp"

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
