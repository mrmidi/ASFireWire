// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include <DriverKit/IOService.h>
#include <cstdint>

class ASFWAudioNub : public IOService {
public:
    virtual ~ASFWAudioNub() = default;
    void SetStreamMode(uint32_t) {}
    void SetGuid(uint64_t) {}
    [[nodiscard]] uint32_t GetCurrentSampleRateHz() const noexcept { return 48000; }
    void NotifyDeviceClockChanged(uint32_t) noexcept {}
};
