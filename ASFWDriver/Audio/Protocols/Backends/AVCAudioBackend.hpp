// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AVCAudioBackend.hpp
// AV/C audio backend (Music subunit discovery) with CMP/PCR always for audio.

#pragma once

#include "IAudioBackend.hpp"
#include "../../Session/AudioSessions.hpp"
#include "IsochDuplexHostTransport.hpp"
#include "PublicationGate.hpp"

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
                    IIsochDuplexHostTransport& hostTransport,
                    Session::AudioSessions& sessions,
                    Driver::HardwareInterface& hardware) noexcept;
    ~AVCAudioBackend() noexcept override;

    AVCAudioBackend(const AVCAudioBackend&) = delete;
    AVCAudioBackend& operator=(const AVCAudioBackend&) = delete;

    [[nodiscard]] const char* Name() const noexcept override { return "AV/C"; }

    void OnAudioConfigurationReady(uint64_t guid, const Model::ASFWAudioDevice& config) noexcept;
    // The coordinator owns remote-device teardown. AV/C only drops queued
    // recovery/configuration work for the retired GUID.
    void CancelRemoteDeviceWork(uint64_t guid) noexcept override;
    void OnDeviceResumed(uint64_t guid) noexcept override;
    void HandleHostTimingLoss(uint64_t guid) noexcept override { HandleTimingLoss(guid); }
    void BeginTeardown() noexcept override;

    // Called by the backend-neutral AudioCoordinator transport callback.
    void HandleTimingLoss(uint64_t guid) noexcept;

    [[nodiscard]] IOReturn StartStreaming(uint64_t guid) noexcept override;
    [[nodiscard]] IOReturn StopStreaming(uint64_t guid) noexcept override;

private:
    // Clears the per-GUID in-flight recovery flag (recoveringGuids_). Shared exit
    // point for the timing-loss escalation block.
    void FinishRecovery(uint64_t guid) noexcept;

    [[nodiscard]] bool IsActiveDevice(uint64_t guid) noexcept;

    AudioNubPublisher& publisher_;
    Discovery::DeviceRegistry& registry_;
    AudioRuntimeRegistry& runtime_;
    Driver::HardwareInterface& hardware_;
    IIsochDuplexHostTransport& hostTransport_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> teardownStarted_{false};
    std::atomic<bool> teardownComplete_{false};
    PublicationGate publicationGate_{};
    Session::AudioSessions& sessions_;

#ifdef ASFW_HOST_TEST
public:
    void SetBeforePublishHookForTesting(std::function<void()> hook) noexcept {
        beforePublishHookForTesting_ = std::move(hook);
    }
    void SetOnTeardownDrainStartedHookForTesting(std::function<void()> hook) noexcept {
        onTeardownDrainStartedHookForTesting_ = std::move(hook);
    }
    void SetOnTeardownGateClosedHookForTesting(std::function<void()> hook) noexcept {
        publicationGate_.SetOnGateClosedForTesting(std::move(hook));
    }
    void SetOnSecondaryTeardownWaitingHookForTesting(std::function<void()> hook) noexcept {
        onSecondaryTeardownWaitingHookForTesting_ = std::move(hook);
    }
    [[nodiscard]] IODispatchQueue* WorkQueueForTesting() const noexcept {
        return workQueue_.get();
    }
    [[nodiscard]] uint64_t PublicationRejectCountForTesting() const noexcept {
        return publicationGate_.RejectCount();
    }
    [[nodiscard]] bool IsTeardownCompleteForTesting() const noexcept {
        return teardownComplete_.load(std::memory_order_acquire);
    }
private:
    std::function<void()> beforePublishHookForTesting_{};
    std::function<void()> onTeardownDrainStartedHookForTesting_{};
    std::function<void()> onSecondaryTeardownWaitingHookForTesting_{};
#endif

    IOLock* lock_{nullptr};
    OSSharedPtr<IODispatchQueue> workQueue_{};
    std::unordered_map<uint64_t, Model::ASFWAudioDevice> configByGuid_{};
    // GUIDs with a recovery block queued or settling; a second event for the
    // same GUID joins it instead of queueing another. Guarded by lock_.
    std::unordered_set<uint64_t> recoveringGuids_{};
    uint64_t activeGuid_{0};

    // Debounce before escalating an RX timing-loss to a restart. AppleFWAudio
    // uses 80 ms × 2 consecutive late RX callbacks; we settle ~256 ms (≥ several
    // IO windows) so a host-side StartIO/StopIO gap that the RX epoch reset
    // self-heals is not mistaken for a device outage.
    static constexpr uint32_t kTimingLossSettleMs = 256;
    static constexpr uint32_t kTimingLossPollMs = 32;
    // A device that comes back only partially cannot restart-loop forever: the
    // session stops recovering after repeated failures
    // (SessionScheduler::kMaxFaultRestartFailures).
};

} // namespace ASFW::Audio
