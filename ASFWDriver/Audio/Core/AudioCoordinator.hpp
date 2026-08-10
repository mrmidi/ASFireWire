// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioCoordinator.hpp
// Central audio control-plane entry point. Owns audio nubs and routes
// start/stop to explicit DICE vs AV/C backends.

#pragma once

#include "IAVCAudioConfigListener.hpp"
#include "AudioNubPublisher.hpp"
#include "../Protocols/Backends/AVCAudioBackend.hpp"
#include "../Protocols/Backends/DiceAudioBackend.hpp"
#include "../Protocols/Backends/IsochDuplexHostTransport.hpp"

#include "../../Logging/Logging.hpp"
#include "../Protocols/DeviceProtocolFactory.hpp"

#include "../../Discovery/IDeviceManager.hpp"

#include <DriverKit/IOLib.h>
#include <atomic>
#include <cstdint>
#include <optional>
#include <unordered_set>

class IOService;

namespace ASFW::Audio {

class AudioRuntimeRegistry;

class AudioCoordinator final : public Discovery::IDeviceObserver,
                               public IAVCAudioConfigListener {
public:
    AudioCoordinator(IOService* driver,
                     Discovery::IDeviceManager& deviceManager,
                     Discovery::DeviceRegistry& registry,
                     AudioRuntimeRegistry& runtime,
                     Driver::IsochService& isoch,
                     Driver::HardwareInterface& hardware) noexcept;
    ~AudioCoordinator() noexcept override;

    AudioCoordinator(const AudioCoordinator&) = delete;
    AudioCoordinator& operator=(const AudioCoordinator&) = delete;

    void SetCMPClient(ASFW::CMP::CMPClient* client) noexcept;
    void SetTxPreparationCallback(
        Driver::IsochService::TxPreparationCallback callback) noexcept;
    void SetClockAnchorReadyCallback(
        IsochDuplexHostTransport::ClockAnchorReadyCallback callback) noexcept;

    // IDeviceObserver
    void OnDeviceAdded(std::shared_ptr<Discovery::FWDevice> device) override;
    void OnDeviceResumed(std::shared_ptr<Discovery::FWDevice> device) override;
    void OnDeviceSuspended(std::shared_ptr<Discovery::FWDevice> device) override;
    void OnDeviceRemoved(Discovery::Guid64 guid) override;

    // IAVCAudioConfigListener
    void OnAVCAudioConfigurationReady(uint64_t guid,
                                      const Model::ASFWAudioDevice& config) noexcept override;
    void HandleCycleInconsistent() noexcept;

    [[nodiscard]] IOReturn StartStreaming(uint64_t guid) noexcept;
    [[nodiscard]] IOReturn StopStreaming(uint64_t guid) noexcept;
    [[nodiscard]] IOReturn RequestClockConfig(
        uint64_t guid,
        const AudioClockConfig& desiredClock,
        DuplexRestartReason reason) noexcept;
    void BeginTeardown() noexcept;

    [[nodiscard]] ASFWAudioNub* GetNub(uint64_t guid) const noexcept { return publisher_.GetNub(guid); }

    /// Debug helper: return the GUID if exactly one audio nub is published.
    [[nodiscard]] std::optional<uint64_t> GetSinglePublishedGuid() const noexcept;

private:
    [[nodiscard]] IAudioBackend* BackendForGuid(uint64_t guid) noexcept;
    [[nodiscard]] kern_return_t StopHostTransport(const char* reason,
                                                   bool generationInvalidated = false) noexcept;
    void HandleHostTimingLoss(uint64_t guid) noexcept;

    AudioNubPublisher publisher_;
    Discovery::IDeviceManager& deviceManager_;
    Discovery::DeviceRegistry& registry_;
    AudioRuntimeRegistry& runtime_;
    // The one controller-global isoch transport session. Backends borrow this
    // neutral interface; none owns a second wrapper around IsochService.
    IsochDuplexHostTransport hostTransport_;
    std::atomic<bool> teardownRequested_{false};
    AudioDuplexCoordinator duplexCoordinator_;
    DiceAudioBackend dice_;
    AVCAudioBackend avc_;

    IOLock* lock_{nullptr};
    uint64_t activeGuid_{0};
    // A CoreAudio StopIO can arrive after discovery has retired the GUID. Keep
    // that callback from re-entering a backend that now has no remote device.
    std::unordered_set<uint64_t> remoteLostGuids_{};
};

} // namespace ASFW::Audio
