// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AVCAudioBackend.hpp
// AV/C audio backend (Music subunit discovery) with CMP/PCR always for audio.

#pragma once

#include "IAudioBackend.hpp"
#include "AudioDuplexCoordinator.hpp"
#include "IsochDuplexHostTransport.hpp"

#include "../../../Audio/Core/AudioNubPublisher.hpp"

#include "../../../Discovery/DeviceRegistry.hpp"
#include "../../../Hardware/HardwareInterface.hpp"
#include "../../../Isoch/IsochService.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSSharedPtr.h>

namespace ASFW::Audio {

class AudioRuntimeRegistry;

class AVCAudioBackend final : public IAudioBackend {
public:
    using HostTeardownRequest = std::function<kern_return_t()>;

    AVCAudioBackend(AudioNubPublisher& publisher,
                    Discovery::DeviceRegistry& registry,
                    AudioRuntimeRegistry& runtime,
                    IIsochDuplexHostTransport& hostTransport,
                    AudioDuplexCoordinator& duplexCoordinator,
                    Driver::HardwareInterface& hardware) noexcept;
    ~AVCAudioBackend() noexcept override;

    AVCAudioBackend(const AVCAudioBackend&) = delete;
    AVCAudioBackend& operator=(const AVCAudioBackend&) = delete;

    [[nodiscard]] const char* Name() const noexcept override { return "AV/C"; }

    void OnAudioConfigurationReady(uint64_t guid, const Model::ASFWAudioDevice& config) noexcept;
    void OnDeviceRemoved(uint64_t guid) noexcept;
    void OnDeviceResumed(uint64_t guid) noexcept;
    void BeginTeardown() noexcept;
    void SetHostTeardownRequest(HostTeardownRequest request) noexcept {
        hostTeardownRequest_ = std::move(request);
    }

    // Called by the backend-neutral AudioCoordinator transport callback.
    void HandleTimingLoss(uint64_t guid) noexcept;

    [[nodiscard]] IOReturn StartStreaming(uint64_t guid) noexcept override;
    [[nodiscard]] IOReturn StopStreaming(uint64_t guid) noexcept override;

private:
    // Clears the per-GUID in-flight recovery flag (recoveringGuids_). Shared exit
    // point for the timing-loss escalation block.
    void FinishRecovery(uint64_t guid) noexcept;

    // FW-144. A device-removal stop that reports failure must not strand the
    // nub: the CoreAudio device then outlives the hardware it represents until
    // the whole driver tears down. Record the removal as owed, re-attempt it,
    // and only tear the nub down once the stop actually succeeds.
    /// Whether `guid` is still the backend's active device. Debounced work must
    /// re-check this: the device can be removed during a settle window, and
    /// FinishDeviceRemoval clearing activeGuid_ is how that becomes observable.
    [[nodiscard]] bool IsActiveDevice(uint64_t guid) noexcept;
    /// True when the removal may proceed: either the stop succeeded, or the
    /// device record is already gone, in which case the device-side stages are
    /// moot and only the host-side cleanup (run here) still matters.
    [[nodiscard]] bool IsRemovalStopSettled(uint64_t guid, IOReturn stopStatus) noexcept;
    void DeferNubRemoval(uint64_t guid, IOReturn stopStatus) noexcept;
    void RetryPendingNubRemovals() noexcept;
    /// Nub teardown plus per-GUID state cleanup. Shared by the immediate and
    /// deferred removal paths so they cannot drift apart.
    void FinishDeviceRemoval(uint64_t guid) noexcept;

    AudioNubPublisher& publisher_;
    Discovery::DeviceRegistry& registry_;
    AudioRuntimeRegistry& runtime_;
    Driver::HardwareInterface& hardware_;
    IIsochDuplexHostTransport& hostTransport_;
    HostTeardownRequest hostTeardownRequest_{};
    std::atomic<bool> stopping_{false};
    AudioDuplexCoordinator& duplexCoordinator_;

    IOLock* lock_{nullptr};
    OSSharedPtr<IODispatchQueue> workQueue_{};
    std::unordered_map<uint64_t, Model::ASFWAudioDevice> configByGuid_{};
    std::unordered_set<uint64_t> recoveringGuids_{};
    // Consecutive timing-loss escalations without an observed recovery, per GUID.
    // Reset on self-heal or a successful restart; bounds a restart-loop against a
    // genuinely gone device. Guarded by lock_.
    std::unordered_map<uint64_t, uint8_t> timingLossAttempts_{};
    // Devices whose nub teardown is owed but was not yet safe. Guarded by lock_.
    std::unordered_set<uint64_t> pendingNubRemoval_{};
    uint64_t activeGuid_{0};

    // Debounce before escalating an RX timing-loss to a restart. AppleFWAudio
    // uses 80 ms × 2 consecutive late RX callbacks; we settle ~256 ms (≥ several
    // IO windows) so a host-side StartIO/StopIO gap that the RX epoch reset
    // self-heals is not mistaken for a device outage.
    static constexpr uint32_t kTimingLossSettleMs = 256;
    static constexpr uint32_t kTimingLossPollMs = 32;
    // Cap consecutive failed escalations so a device that comes back only
    // partially (re-establishes then dies) cannot restart-loop forever.
    static constexpr uint8_t kTimingLossMaxAttempts = 4;
};

} // namespace ASFW::Audio
