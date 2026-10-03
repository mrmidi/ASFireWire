// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuAudioBackend.hpp - MOTU audio backend, protocol v2 and v3 (828 Mk3).
//
// Much smaller than DiceAudioBackend, and deliberately so. Most of that backend's bulk
// is DICE-specific machinery MOTU has no equivalent for: a notification mailbox, the
// CLOCK_ACCEPTED handshake, clock-lock probing and the recovery state machine those
// notifications drive. MOTU v2 publishes no notification register at all -- its clock
// status word is the only health evidence it offers, which MotuV2Protocol already
// exposes through FamilyDriver::ReadHealth().
//
// So this backend does the part that is genuinely shared: gate on teardown, make sure a
// nub and a bound runtime endpoint exist, and hand streaming to the audio session,
// which drives the device through its FamilyDriver (MotuV2Protocol or
// MOTU828Mk3Protocol, chosen by the catalog's protocol implementation).
//
// Protocol v3 differs in two places here. Its nub is published from MOTU828Mk3Profile
// until the device's registers have been read. And the 828 Mk3 writes a status word to
// the host's notification address; a buffer fault in it restarts the streams, as the
// vendor driver does (MotuStatusWord.hpp).

#pragma once

#include "IAudioBackend.hpp"
#include "PublicationGate.hpp"
#include "../Duplex/DuplexControlTypes.hpp"

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
namespace Session {
class AudioSessions;
}

class MotuAudioBackend final : public IAudioBackend {
public:
    MotuAudioBackend(AudioNubPublisher& publisher,
                     Discovery::DeviceRegistry& registry,
                     AudioRuntimeRegistry& runtime,
                     Session::AudioSessions& sessions,
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
    void OnStreamsRestarted(uint64_t guid) noexcept override { EnsureNubForGuid(guid); }
    // The 828 Mk3's status word (protocol v3 only; other MOTU families lay the
    // bits out differently). Bit 31 discards the word; either buffer-fault bit
    // queues a kRecoverAfterDeviceBufferFault restart of the running streams.
    void HandleDeviceNotification(uint64_t guid, uint32_t bits) noexcept override;
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
    [[nodiscard]] bool QueueRecovery(uint64_t guid, DuplexRestartReason reason,
                                     const char* what) noexcept;

    AudioNubPublisher& publisher_;
    Discovery::DeviceRegistry& registry_;
    AudioRuntimeRegistry& runtime_;
    Driver::HardwareInterface& hardware_;
    Session::AudioSessions& sessions_;

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
