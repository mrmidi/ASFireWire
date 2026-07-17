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
#include <unordered_map>
#include <unordered_set>

#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSSharedPtr.h>

namespace ASFW::Audio {

class AudioRuntimeRegistry;

class AVCAudioBackend final : public IAudioBackend {
public:
    AVCAudioBackend(AudioNubPublisher& publisher,
                    Discovery::DeviceRegistry& registry,
                    AudioRuntimeRegistry& runtime,
                    Driver::IsochService& isoch,
                    Driver::HardwareInterface& hardware) noexcept;
    ~AVCAudioBackend() noexcept override;

    AVCAudioBackend(const AVCAudioBackend&) = delete;
    AVCAudioBackend& operator=(const AVCAudioBackend&) = delete;

    [[nodiscard]] const char* Name() const noexcept override { return "AV/C"; }

    void OnAudioConfigurationReady(uint64_t guid, const Model::ASFWAudioDevice& config) noexcept;
    void OnDeviceRemoved(uint64_t guid) noexcept;
    void OnDeviceResumed(uint64_t guid) noexcept;
    void BeginTeardown() noexcept;

    [[nodiscard]] IOReturn StartStreaming(uint64_t guid) noexcept override;
    [[nodiscard]] IOReturn StopStreaming(uint64_t guid) noexcept override;

private:
    // RX timing-loss escalation (doc AVC_STREAM_HEALTH_AND_RECOVERY.md §6). The
    // transport fires this once when an established replay cadence dies. AV/C has
    // no health register, so the verdict is the RX cadence itself: debounce a
    // settle window (let the [TxAlign] self-heal absorb host-side StartIO/StopIO
    // gaps), then if replay has NOT re-established, escalate to a coordinator
    // restart (CMP break/re-establish) — matching bebob/FFADO/AppleFWAudio.
    void HandleTimingLoss(uint64_t guid) noexcept;
    // Clears the per-GUID in-flight recovery flag (recoveringGuids_). Shared exit
    // point for the timing-loss escalation block.
    void FinishRecovery(uint64_t guid) noexcept;

    AudioNubPublisher& publisher_;
    Discovery::DeviceRegistry& registry_;
    AudioRuntimeRegistry& runtime_;
    Driver::HardwareInterface& hardware_;
    IsochDuplexHostTransport hostTransport_;
    std::atomic<bool> stopping_{false};
    AudioDuplexCoordinator duplexCoordinator_;

    IOLock* lock_{nullptr};
    OSSharedPtr<IODispatchQueue> workQueue_{};
    std::unordered_map<uint64_t, Model::ASFWAudioDevice> configByGuid_{};
    std::unordered_set<uint64_t> recoveringGuids_{};
    // Consecutive timing-loss escalations without an observed recovery, per GUID.
    // Reset on self-heal or a successful restart; bounds a restart-loop against a
    // genuinely gone device. Guarded by lock_.
    std::unordered_map<uint64_t, uint8_t> timingLossAttempts_{};
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
