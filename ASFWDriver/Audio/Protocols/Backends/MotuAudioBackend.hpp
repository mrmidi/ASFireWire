// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuAudioBackend.hpp - MOTU protocol-v2 audio backend.
//
// Much smaller than DiceAudioBackend, and deliberately so. Most of that backend's bulk
// is DICE-specific machinery MOTU has no equivalent for: a notification mailbox, the
// CLOCK_ACCEPTED handshake, clock-lock probing and the recovery state machine those
// notifications drive. MOTU v2 publishes no notification register at all -- its clock
// status word is the only health evidence it offers, which MotuV2Protocol already
// exposes through IDuplexDeviceControl::ReadDuplexHealth().
//
// So this backend does the part that is genuinely shared: gate on teardown, make sure a
// nub and a bound runtime endpoint exist, and hand streaming to AudioDuplexCoordinator,
// which drives the device through the IDuplexDeviceControl seam MotuV2Protocol
// implements.

#pragma once

#include "IAudioBackend.hpp"

#include <DriverKit/IOLib.h>

#include <atomic>
#include <cstdint>
#include <unordered_set>

namespace ASFW::Discovery {
class DeviceRegistry;
}

namespace ASFW::Driver {
class HardwareInterface;
}

namespace ASFW::Audio {

class AudioNubPublisher;
class AudioRuntimeRegistry;
class AudioDuplexCoordinator;

class MotuAudioBackend final : public IAudioBackend {
public:
    MotuAudioBackend(AudioNubPublisher& publisher,
                     Discovery::DeviceRegistry& registry,
                     AudioRuntimeRegistry& runtime,
                     AudioDuplexCoordinator& duplexCoordinator,
                     Driver::HardwareInterface& hardware) noexcept;
    ~MotuAudioBackend() noexcept override;

    MotuAudioBackend(const MotuAudioBackend&) = delete;
    MotuAudioBackend& operator=(const MotuAudioBackend&) = delete;

    [[nodiscard]] const char* Name() const noexcept override { return "MOTU"; }

    [[nodiscard]] IOReturn StartStreaming(uint64_t guid) noexcept override;
    [[nodiscard]] IOReturn StopStreaming(uint64_t guid) noexcept override;

    /// Quiesce before the core detaches hardware, mirroring DiceAudioBackend::
    /// BeginTeardown. MOTU owns no work queue of its own, so this is only a latch that
    /// makes subsequent Start/Stop refuse -- but it must still exist, because the
    /// coordinator can otherwise be asked to start a stream while the bus is going away.
    /// Idempotent.
    void BeginTeardown() noexcept;

private:
    void EnsureNubForGuid(uint64_t guid) noexcept;

    AudioNubPublisher& publisher_;
    Discovery::DeviceRegistry& registry_;
    AudioRuntimeRegistry& runtime_;
    Driver::HardwareInterface& hardware_;
    AudioDuplexCoordinator& coordinator_;

    std::atomic<bool> stopping_{false};
    IOLock* lock_{nullptr};
    std::unordered_set<uint64_t> activeStreamingGuids_{};
};

} // namespace ASFW::Audio
