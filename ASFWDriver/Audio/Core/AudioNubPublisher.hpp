// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioNubPublisher.hpp
// Centralized creation/lookup/termination of ASFWAudioNub instances (per GUID).

#pragma once

#include "../Model/ASFWAudioDevice.hpp"

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

    /// Create an ASFWAudioNub for `guid` if missing, and populate its properties from `config`.
    /// Returns true on success or if already present.
    [[nodiscard]] bool EnsureNub(uint64_t guid,
                                 const Model::ASFWAudioDevice& config,
                                 const char* sourceTag) noexcept;

    /// Return the nub pointer if present (not retained). Valid only while published.
    [[nodiscard]] ASFWAudioNub* GetNub(uint64_t guid) const noexcept;

    /// Re-publish properties onto a nub that already exists.
    ///
    /// EnsureNub is create-once, so a device re-resolved after recovery or a
    /// configuration change would otherwise keep serving whatever geometry it
    /// was first published with. That is safe only while the geometry cannot
    /// change underneath a live nub; this is what makes it safe when it can.
    ///
    /// Returns false when there is no nub for the GUID, or when the properties
    /// could not be built -- in which case the nub keeps its previous ones,
    /// because a half-applied description is worse than a stale one.
    [[nodiscard]] bool RefreshNubProperties(uint64_t guid,
                                            const Model::ASFWAudioDevice& config,
                                            const char* sourceTag) noexcept;

    /// Return the GUID if exactly one nub is published (debug/bring-up helper).
    [[nodiscard]] std::optional<uint64_t> GetSingleGuid() const noexcept;

    /// Terminate and forget a nub if present.
    void TerminateNub(uint64_t guid, const char* reasonTag) noexcept;

private:
    [[nodiscard]] bool ReserveGuidLocked(uint64_t guid) noexcept;

    IOService* driver_{nullptr};
    IOLock* lock_{nullptr};
    std::unordered_map<uint64_t, ASFWAudioNub*> nubsByGuid_{};
};

} // namespace ASFW::Audio
