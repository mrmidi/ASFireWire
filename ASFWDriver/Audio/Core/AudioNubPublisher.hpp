// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioNubPublisher.hpp
// Centralized creation/lookup/termination of ASFWAudioNub instances. Runtime
// endpoint IDs are the only keys; an observed EUI-64 is diagnostics metadata.

#pragma once

#include "../Devices/ResolvedAudioEndpointProfile.hpp"

#include <DriverKit/IOLib.h>
#include <cstdint>
#include <optional>
#include <unordered_map>

class IOService;
class IOLock;
class ASFWAudioNub;

namespace ASFW::Audio {

class AudioNubPublisher {
public:
    explicit AudioNubPublisher(IOService* driver) noexcept;
    ~AudioNubPublisher() noexcept;

    AudioNubPublisher(const AudioNubPublisher&) = delete;
    AudioNubPublisher& operator=(const AudioNubPublisher&) = delete;

    /// Create an ASFWAudioNub for `profile.endpointId` if missing and publish a
    /// value-only, versioned profile snapshot before the nub starts.
    /// Returns true on success or if already present.
    [[nodiscard]] bool EnsureNub(const Devices::ResolvedAudioEndpointProfile& profile,
                                 const char* sourceTag) noexcept;

    /// Return the nub pointer if present (not retained). Valid only while published.
    [[nodiscard]] ASFWAudioNub* GetNub(Devices::AudioEndpointId endpointId) const noexcept;

    /// Return the endpoint if exactly one nub is published (debug helper).
    [[nodiscard]] std::optional<Devices::AudioEndpointId> GetSingleEndpointId() const noexcept;

    /// Terminate and forget a nub if present.
    void TerminateNub(Devices::AudioEndpointId endpointId,
                      const char* reasonTag) noexcept;

private:
    [[nodiscard]] bool ReserveEndpointLocked(Devices::AudioEndpointId endpointId) noexcept;

    IOService* driver_{nullptr};
    IOLock* lock_{nullptr};
    std::unordered_map<Devices::AudioEndpointId, ASFWAudioNub*,
                       Devices::AudioEndpointIdHash> nubsByEndpoint_{};
};

} // namespace ASFW::Audio
