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
#include "PublicationGate.hpp"

#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSSharedPtr.h>

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
    /// BeginTeardown. Close recovery admission before draining the work queue.
    /// Idempotent.
    void BeginTeardown() noexcept override;
    void OnDeviceRecordUpdated(uint64_t guid) noexcept override;
    void CancelRemoteDeviceWork(uint64_t guid) noexcept override;
    void HandleHostTimingLoss(uint64_t guid) noexcept override { (void)QueueTimingRecovery(guid); }
    [[nodiscard]] bool QueueTimingRecovery(uint64_t guid) noexcept;

#ifdef ASFW_HOST_TEST
    IODispatchQueue* WorkQueueForTesting() { return workQueue_.get(); }
    void SetTeardownHooksForTesting(std::function<void()> drain, std::function<void()> secondary) {
        onTeardownDrain_ = std::move(drain);
        onSecondaryTeardown_ = std::move(secondary);
    }
#endif
private:
#ifdef ASFW_HOST_TEST
    std::function<void()> onTeardownDrain_{};
    std::function<void()> onSecondaryTeardown_{};
#endif
    void EnsureNubForGuid(uint64_t guid) noexcept;

    AudioNubPublisher& publisher_;
    Discovery::DeviceRegistry& registry_;
    AudioRuntimeRegistry& runtime_;
    Driver::HardwareInterface& hardware_;
    AudioDuplexCoordinator& coordinator_;

    OSSharedPtr<IODispatchQueue> workQueue_{};
    std::atomic<bool> recoveryInFlight_{false};
    std::atomic<uint64_t> recoveryRejectCount_{0};

    PublicationGate recoveryAdmission_{};
    std::atomic<bool> teardownStarted_{false};
    std::atomic<bool> teardownComplete_{false};
    std::atomic<bool> stopping_{false};
    // Publication attempts refused because teardown already latched (I3: late
    // work counts, never acts).
    std::atomic<uint64_t> publicationRejectCount_{0};
    IOLock* lock_{nullptr};
    std::unordered_set<uint64_t> activeStreamingGuids_{};
};

} // namespace ASFW::Audio
