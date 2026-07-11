// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceAudioBackend.hpp
// DICE/TCAT-controlled audio backend (no AV/C, no CMP/PCR).

#pragma once

#include "IAudioBackend.hpp"
#include "AudioDuplexCoordinator.hpp"
#include "IsochDuplexHostTransport.hpp"

#include "../../../Audio/Core/AudioNubPublisher.hpp"

#include "../../../Discovery/DeviceRegistry.hpp"
#include "../../../Hardware/HardwareInterface.hpp"
#include "../../../Isoch/IsochService.hpp"

#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSSharedPtr.h>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace ASFW::Audio {

class AudioRuntimeRegistry;

class DiceAudioBackend final : public IAudioBackend {
public:
    DiceAudioBackend(AudioNubPublisher& publisher,
                     Discovery::DeviceRegistry& registry,
                     AudioRuntimeRegistry& runtime,
                     Driver::IsochService& isoch,
                     Driver::HardwareInterface& hardware) noexcept;
    ~DiceAudioBackend() noexcept override;

    DiceAudioBackend(const DiceAudioBackend&) = delete;
    DiceAudioBackend& operator=(const DiceAudioBackend&) = delete;

    [[nodiscard]] const char* Name() const noexcept override { return "DICE"; }

    void OnDeviceRecordUpdated(uint64_t guid) noexcept;
    void OnDeviceRemoved(uint64_t guid) noexcept;
    void HandleRecoveryEvent(uint64_t guid, DuplexRestartReason reason) noexcept;

    [[nodiscard]] IOReturn StartStreaming(uint64_t guid) noexcept override;
    [[nodiscard]] IOReturn StopStreaming(uint64_t guid) noexcept override;
    [[nodiscard]] IOReturn RequestClockConfig(uint64_t guid,
                                              const AudioClockConfig& desiredClock,
                                              DuplexRestartReason reason) noexcept;

    // FW-61: quiesce the dice queue before the core detaches hardware. Sets the stop flag,
    // cancels in-flight recovery (coordinator), then drains the work queue (synchronous
    // barrier) so no recovery/probe block issues MMIO after ASFWDriver::Stop's Detach.
    // Idempotent; must be called before HardwareInterface::Detach().
    void BeginTeardown() noexcept;

private:
    void EnsureNubForGuid(uint64_t guid) noexcept;
    void HandleDeviceNotification(uint32_t bits) noexcept;
    void ProbeDuplexHealth(uint64_t guid, uint32_t notificationBits) noexcept;
    [[nodiscard]] bool TryBeginRecovery(uint64_t guid) noexcept;
    void FinishRecovery(uint64_t guid) noexcept;
    static void NotificationObserverThunk(void* context, uint32_t bits) noexcept;


    AudioNubPublisher& publisher_;
    Discovery::DeviceRegistry& registry_;
    AudioRuntimeRegistry& runtime_;
    Driver::HardwareInterface& hardware_;
    IsochDuplexHostTransport hostTransport_;
    std::atomic<bool> stopping_{false}; // FW-61 teardown latch
    AudioDuplexCoordinator restartCoordinator_;

    IOLock* lock_{nullptr};
    OSSharedPtr<IODispatchQueue> workQueue_{};
    std::unordered_map<uint64_t, uint8_t> attemptsByGuid_{};
    std::unordered_set<uint64_t> retryOutstanding_{};
    std::unordered_set<uint64_t> activeStreamingGuids_{};
    std::unordered_set<uint64_t> recoveringGuids_{};
    std::atomic<uint64_t> recoveryRejectCount_{0};
    std::atomic<uint64_t> probeRejectCount_{0};
    std::atomic<uint64_t> probeAbortCount_{0};

    static constexpr uint32_t kCapsRetryDelayMs = 50;
    static constexpr uint8_t kCapsRetryMaxAttempts = 40; // 2s @ 50ms
    static constexpr uint32_t kHealthBridgeTimeoutMs = 1000;
    static constexpr uint32_t kHealthBridgePollMs = 10;
};

} // namespace ASFW::Audio
