// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// AudioNubPublisher.hpp
// Centralized creation/lookup/termination of ASFWAudioNub instances (per GUID).

#pragma once

#include "Model/ASFWAudioDevice.hpp"

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
