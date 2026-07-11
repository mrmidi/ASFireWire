// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AVCAudioBackend.hpp
// AV/C audio backend (Music subunit discovery) with CMP/PCR always for audio.

#pragma once

#include "IAudioBackend.hpp"
#include "DiceDuplexRestartCoordinator.hpp"
#include "DiceHostTransport.hpp"

#include "../../../Audio/Core/AudioNubPublisher.hpp"

#include "../../../Discovery/DeviceRegistry.hpp"
#include "../../../Hardware/HardwareInterface.hpp"
#include "../../../Isoch/IsochService.hpp"

#include <atomic>
#include <cstdint>
#include <unordered_map>

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
    void BeginTeardown() noexcept;

    [[nodiscard]] IOReturn StartStreaming(uint64_t guid) noexcept override;
    [[nodiscard]] IOReturn StopStreaming(uint64_t guid) noexcept override;

private:
    AudioNubPublisher& publisher_;
    Discovery::DeviceRegistry& registry_;
    AudioRuntimeRegistry& runtime_;
    Driver::HardwareInterface& hardware_;
    DiceIsochHostTransport hostTransport_;
    std::atomic<bool> stopping_{false};
    AudioDuplexCoordinator duplexCoordinator_;

    IOLock* lock_{nullptr};
    std::unordered_map<uint64_t, Model::ASFWAudioDevice> configByGuid_{};
    uint64_t activeGuid_{0};
};

} // namespace ASFW::Audio
