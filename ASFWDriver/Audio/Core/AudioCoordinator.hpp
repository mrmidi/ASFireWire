// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioCoordinator.hpp
// Central audio control-plane entry point. Owns audio nubs and routes
// start/stop to the explicit DICE, MOTU and AV/C backends.

#pragma once

#include "IAVCAudioConfigListener.hpp"
#include "AudioNubPublisher.hpp"
#include "../Protocols/Backends/IsochDuplexHostTransport.hpp"
#include "../Session/AudioSessions.hpp"
#include "../Host/AudioDeviceHost.hpp"
#include "../Host/AvcFamilyAdapter.hpp"
#include "../Host/DiceFamilyAdapter.hpp"
#include "../Host/MotuFamilyAdapter.hpp"
#include "../Host/RmeFamilyAdapter.hpp"

#include "../../Logging/Logging.hpp"

#include "../../Discovery/IDeviceManager.hpp"

#include <DriverKit/IOLib.h>
#include <atomic>
#include <cstdint>
#include <optional>
#include <unordered_map>

class IOService;

namespace ASFW::CMP {
class CMPClient;
}

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
                     Driver::HardwareInterface& hardware,
                     DICE::DiceNotificationRouter& diceNotifications) noexcept;
    ~AudioCoordinator() noexcept override;

    AudioCoordinator(const AudioCoordinator&) = delete;
    AudioCoordinator& operator=(const AudioCoordinator&) = delete;

    void SetCMPClient(ASFW::CMP::CMPClient* client) noexcept;
    // The bus's IRM client; the audio session reserves stream resources with it.
    void SetIRMClient(ASFW::IRM::IRMClient* client) noexcept { sessions_.SetIrmClient(client); }

    // IDeviceObserver
    void OnDeviceAdded(std::shared_ptr<Discovery::FWDevice> device) override;
    void OnDeviceResumed(std::shared_ptr<Discovery::FWDevice> device) override;
    void OnDeviceSuspended(std::shared_ptr<Discovery::FWDevice> device) override;
    void OnDeviceRemoved(Discovery::Guid64 guid) override;

    // IAVCAudioConfigListener
    void OnAVCAudioConfigurationReady(uint64_t guid,
                                      const Model::ASFWAudioDevice& config) noexcept override;
    [[nodiscard]] bool IsAudioActive(uint64_t guid) const noexcept override {
        return sessions_.IsStreaming(guid) || sessions_.IsReconciling(guid);
    }
    void HandleCycleInconsistent() noexcept;
    // After async generation work has been aborted, before ROM discovery.
    void HandleBusReset() noexcept;

    [[nodiscard]] IOReturn StartStreaming(uint64_t guid, AudioClockConfig clock = {}) noexcept;
    [[nodiscard]] IOReturn StopStreaming(uint64_t guid) noexcept;
    [[nodiscard]] IOReturn RequestClockConfig(
        uint64_t guid,
        const AudioClockConfig& desiredClock,
        DuplexRestartReason reason) noexcept;
    void BeginTeardown() noexcept;
    // The timer that keeps device-event quiet periods; installed by
    // composition before device callbacks begin.
    void SetSessionTimer(Scheduling::ITimerScheduler* timer) noexcept { sessions_.SetTimerScheduler(timer); }
    void HandleHostTimingLoss(uint64_t guid) noexcept;
    [[nodiscard]] IOReturn MotuCaptureCommand(uint64_t guid, uint32_t stream,
                                            uint32_t command, std::string& output) noexcept;

    [[nodiscard]] ASFWAudioNub* GetNub(uint64_t guid) const noexcept { return publisher_.GetNub(guid); }

#ifdef ASFW_HOST_TEST
    [[nodiscard]] Host::AudioDeviceHost& HostForTesting() noexcept { return host_; }
#endif

    /// Debug helper: return the GUID if exactly one audio nub is published.
    [[nodiscard]] std::optional<uint64_t> GetSinglePublishedGuid() const noexcept;

private:
    // True when `guid`'s current policy names a family the host serves.
    [[nodiscard]] bool ServedByHost(uint64_t guid) const noexcept;
    [[nodiscard]] kern_return_t StopHostTransport(const char* reason,
                                                   bool generationInvalidated = false) noexcept;


    AudioNubPublisher publisher_;
    Discovery::IDeviceManager& deviceManager_;
    Discovery::DeviceRegistry& registry_;
    AudioRuntimeRegistry& runtime_;
    // The one controller-global isoch transport session. Backends borrow this
    // neutral interface; none owns a second wrapper around IsochService.
    IsochDuplexHostTransport hostTransport_;
    std::atomic_flag captureCommandBusy_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> teardownRequested_{false};
    Session::AudioSessions sessions_;
    // Every family runs on the host (documentation/AUDIO_DEVICE_HOST.md §6).
    // Adapters are declared before the host so they outlive its teardown.
    Host::DiceFamilyAdapter diceAdapter_;
    Host::RmeFamilyAdapter rmeAdapter_;
    Host::AvcFamilyAdapter avcAdapter_;
    Host::MotuFamilyAdapter motuAdapter_;
    Host::AudioDeviceHost host_;

    IOLock* lock_{nullptr};
    uint64_t activeGuid_{0};
    // A CoreAudio StopIO can arrive after discovery has retired the GUID. Keep
    // that callback from re-entering a backend that now has no remote device.
    std::unordered_map<uint64_t, IOReturn> remoteLostStopResults_{};
};

} // namespace ASFW::Audio
