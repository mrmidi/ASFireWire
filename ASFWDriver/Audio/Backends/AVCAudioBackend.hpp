// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// AVCAudioBackend.hpp
// AV/C audio backend (Music subunit discovery) with CMP/PCR always for audio.

#pragma once

#include "IAudioBackend.hpp"

#include "../AudioNubPublisher.hpp"

#include "../../Discovery/DeviceRegistry.hpp"
#include "../../Hardware/HardwareInterface.hpp"
#include "../../Isoch/IsochService.hpp"
#include "../../Protocols/AVC/CMP/CMPClient.hpp"

#include <cstdint>
#include <unordered_map>

namespace ASFW::Audio {

class AVCAudioBackend final : public IAudioBackend {
public:
    AVCAudioBackend(AudioNubPublisher& publisher,
                    Discovery::DeviceRegistry& registry,
                    Driver::IsochService& isoch,
                    Driver::HardwareInterface& hardware) noexcept;
    ~AVCAudioBackend() noexcept override;

    AVCAudioBackend(const AVCAudioBackend&) = delete;
    AVCAudioBackend& operator=(const AVCAudioBackend&) = delete;

    [[nodiscard]] const char* Name() const noexcept override { return "AV/C"; }

    void SetCMPClient(ASFW::CMP::CMPClient* client) noexcept { cmpClient_ = client; }

    void OnAudioConfigurationReady(uint64_t guid, const Model::ASFWAudioDevice& config) noexcept;
    void OnDeviceRemoved(uint64_t guid) noexcept;

    [[nodiscard]] IOReturn StartStreaming(uint64_t guid) noexcept override;
    [[nodiscard]] IOReturn StopStreaming(uint64_t guid) noexcept override;

private:
    [[nodiscard]] bool WaitForCMP(std::atomic<bool>& done,
                                  std::atomic<ASFW::CMP::CMPStatus>& status,
                                  uint32_t timeoutMs) noexcept;

    AudioNubPublisher& publisher_;
    Discovery::DeviceRegistry& registry_;
    Driver::IsochService& isoch_;
    Driver::HardwareInterface& hardware_;

    ASFW::CMP::CMPClient* cmpClient_{nullptr};

    IOLock* lock_{nullptr};
    std::unordered_map<uint64_t, Model::ASFWAudioDevice> configByGuid_{};
};

} // namespace ASFW::Audio
