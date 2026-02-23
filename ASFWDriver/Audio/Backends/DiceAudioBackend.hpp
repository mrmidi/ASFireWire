// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// DiceAudioBackend.hpp
// DICE/TCAT-controlled audio backend (no AV/C, no CMP/PCR).

#pragma once

#include "IAudioBackend.hpp"

#include "../AudioNubPublisher.hpp"

#include "../../Discovery/DeviceRegistry.hpp"
#include "../../Hardware/HardwareInterface.hpp"
#include "../../Isoch/IsochService.hpp"

#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSSharedPtr.h>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace ASFW::Audio {

class DiceAudioBackend final : public IAudioBackend {
public:
    DiceAudioBackend(AudioNubPublisher& publisher,
                     Discovery::DeviceRegistry& registry,
                     Driver::IsochService& isoch,
                     Driver::HardwareInterface& hardware) noexcept;
    ~DiceAudioBackend() noexcept override;

    DiceAudioBackend(const DiceAudioBackend&) = delete;
    DiceAudioBackend& operator=(const DiceAudioBackend&) = delete;

    [[nodiscard]] const char* Name() const noexcept override { return "DICE"; }

    void OnDeviceRecordUpdated(uint64_t guid) noexcept;
    void OnDeviceRemoved(uint64_t guid) noexcept;

    [[nodiscard]] IOReturn StartStreaming(uint64_t guid) noexcept override;
    [[nodiscard]] IOReturn StopStreaming(uint64_t guid) noexcept override;

private:
    void EnsureNubForGuid(uint64_t guid) noexcept;

    AudioNubPublisher& publisher_;
    Discovery::DeviceRegistry& registry_;
    Driver::IsochService& isoch_;
    Driver::HardwareInterface& hardware_;

    IOLock* lock_{nullptr};
    OSSharedPtr<IODispatchQueue> workQueue_{};
    std::unordered_map<uint64_t, uint8_t> attemptsByGuid_{};
    std::unordered_set<uint64_t> retryOutstanding_{};

    static constexpr uint32_t kCapsRetryDelayMs = 50;
    static constexpr uint8_t kCapsRetryMaxAttempts = 40; // 2s @ 50ms
};

} // namespace ASFW::Audio

