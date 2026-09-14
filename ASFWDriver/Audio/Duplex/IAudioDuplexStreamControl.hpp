// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../Devices/AudioIdentity.hpp"
#include <DriverKit/IOReturn.h>

namespace ASFW::Audio {

class IAudioDuplexStreamControl {
public:
    virtual ~IAudioDuplexStreamControl() = default;

    [[nodiscard]] virtual IOReturn StartStreaming(
        Devices::AudioEndpointId endpointId) noexcept = 0;
    [[nodiscard]] virtual IOReturn StopStreaming(
        Devices::AudioEndpointId endpointId) noexcept = 0;
};

} // namespace ASFW::Audio
