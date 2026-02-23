// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// AudioCoordinator.hpp
// Central audio control-plane entry point. Owns audio nubs and routes
// start/stop to explicit DICE vs AV/C backends.

#pragma once

#include "IAVCAudioConfigListener.hpp"
#include "AudioNubPublisher.hpp"
#include "Backends/AVCAudioBackend.hpp"
#include "Backends/DiceAudioBackend.hpp"

#include "../Logging/Logging.hpp"
#include "../Protocols/Audio/DeviceProtocolFactory.hpp"

#include "../Discovery/IDeviceManager.hpp"

#include <DriverKit/IOLib.h>
#include <cstdint>
#include <optional>

class IOService;

namespace ASFW::Audio {

class AudioCoordinator final : public Discovery::IDeviceObserver,
                               public IAVCAudioConfigListener {
public:
    AudioCoordinator(IOService* driver,
                     Discovery::IDeviceManager& deviceManager,
                     Discovery::DeviceRegistry& registry,
                     Driver::IsochService& isoch,
                     Driver::HardwareInterface& hardware) noexcept;
    ~AudioCoordinator() noexcept override;

    AudioCoordinator(const AudioCoordinator&) = delete;
    AudioCoordinator& operator=(const AudioCoordinator&) = delete;

    void SetCMPClient(ASFW::CMP::CMPClient* client) noexcept;

    // IDeviceObserver
    void OnDeviceAdded(std::shared_ptr<Discovery::FWDevice> device) override;
    void OnDeviceResumed(std::shared_ptr<Discovery::FWDevice> device) override;
    void OnDeviceSuspended(std::shared_ptr<Discovery::FWDevice> device) override;
    void OnDeviceRemoved(Discovery::Guid64 guid) override;

    // IAVCAudioConfigListener
    void OnAVCAudioConfigurationReady(uint64_t guid,
                                      const Model::ASFWAudioDevice& config) noexcept override;

    [[nodiscard]] IOReturn StartStreaming(uint64_t guid) noexcept;
    [[nodiscard]] IOReturn StopStreaming(uint64_t guid) noexcept;

    [[nodiscard]] ASFWAudioNub* GetNub(uint64_t guid) const noexcept { return publisher_.GetNub(guid); }

    /// Debug helper: return the GUID if exactly one audio nub is published.
    [[nodiscard]] std::optional<uint64_t> GetSinglePublishedGuid() const noexcept;

private:
    [[nodiscard]] IAudioBackend* BackendForGuid(uint64_t guid) noexcept;

    AudioNubPublisher publisher_;
    DiceAudioBackend dice_;
    AVCAudioBackend avc_;

    Discovery::IDeviceManager& deviceManager_;
    Discovery::DeviceRegistry& registry_;

    IOLock* lock_{nullptr};
    uint64_t activeGuid_{0};
};

} // namespace ASFW::Audio
