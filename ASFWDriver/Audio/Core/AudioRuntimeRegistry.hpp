// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// AudioRuntimeRegistry.hpp
// Owns live device-specific IDeviceProtocol instances (guid -> shared_ptr).
//
// This is the runtime counterpart to the metadata-only
// DeviceProfiles::Audio::AudioProfileRegistry: the former answers "what is this
// device", this one owns the booted protocol object that talks to it. It is a
// control-plane lookup (read by AudioDriverKit callbacks that may run off the
// Default queue), so it takes a small IOLock and hands back shared_ptr COPIES;
// the cadence-critical audio packet path never touches it.

#pragma once

#include <DriverKit/IOLib.h>

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace ASFW::Async {
class IFireWireBusOps;
class IFireWireBusInfo;
} // namespace ASFW::Async

namespace ASFW::IRM {
class IRMClient;
}

namespace ASFW::Discovery {
struct DeviceRecord;
}

namespace ASFW::Audio {

class IDeviceProtocol;
class AudioEndpointRuntime;

class AudioRuntimeRegistry final {
public:
    AudioRuntimeRegistry() noexcept;
    ~AudioRuntimeRegistry() noexcept;

    AudioRuntimeRegistry(const AudioRuntimeRegistry&) = delete;
    AudioRuntimeRegistry& operator=(const AudioRuntimeRegistry&) = delete;

    // Control-plane lookup. Returns a shared_ptr COPY so the caller keeps the
    // protocol alive for the duration of its use even if Remove() runs
    // concurrently. Returns nullptr when no protocol is registered for `guid`.
    [[nodiscard]] std::shared_ptr<IDeviceProtocol> FindShared(uint64_t guid) noexcept;
    [[nodiscard]] std::shared_ptr<AudioEndpointRuntime> FindEndpointRuntime(uint64_t guid) noexcept;
    [[nodiscard]] std::shared_ptr<AudioEndpointRuntime> EnsureEndpointRuntime(uint64_t guid) noexcept;

    // Create-on-demand for a known device. Idempotent: an existing instance is
    // returned without re-creating (covers re-scan on device resume). Mirrors the
    // former DeviceRegistry::MaybeCreateKnownProtocol gate + DeviceProtocolFactory
    // ::Create + Initialize; only its *home* has moved out of Discovery. Returns
    // nullptr for unknown devices (Create returns nullptr) or when bus ports / a
    // valid operational node id are unavailable.
    std::shared_ptr<IDeviceProtocol> EnsureForDevice(const Discovery::DeviceRecord& record,
                                                     Async::IFireWireBusOps* busOps,
                                                     Async::IFireWireBusInfo* busInfo,
                                                     IRM::IRMClient* irmClient) noexcept;

    // Registers an already-constructed protocol for `guid`, replacing any existing entry.
    // For external creators/tests that build the protocol themselves rather than going
    // through EnsureForDevice + DeviceProtocolFactory.
    void Insert(uint64_t guid, std::shared_ptr<IDeviceProtocol> protocol) noexcept;

    void Remove(uint64_t guid) noexcept;
    void Clear() noexcept;

private:
    IOLock* lock_{nullptr};
    std::unordered_map<uint64_t, std::shared_ptr<IDeviceProtocol>> protocolsByGuid_;
    std::unordered_map<uint64_t, std::shared_ptr<AudioEndpointRuntime>> endpointsByGuid_;
};

} // namespace ASFW::Audio
