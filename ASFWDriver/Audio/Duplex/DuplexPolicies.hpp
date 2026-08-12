// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include <array>
#include <cstdint>

namespace ASFW::Audio::Duplex {

enum class HostDirection : uint8_t { kReceive, kTransmit };

struct StartPolicy final {
    bool startReceiveBeforeDeviceRx{false};
    bool startTransmitBeforeDeviceTx{false};
    bool requiresPreStreamClockLock{true};
    std::array<HostDirection, 2> prepareOrder{
        HostDirection::kReceive,
        HostDirection::kTransmit,
    };
    std::array<HostDirection, 2> startOrder{
        HostDirection::kReceive,
        HostDirection::kTransmit,
    };
    uint32_t postDeviceEnableDelayMs{2};
};

struct StopPolicy final {
    bool disconnectPlaybackThenStopTransmitThenDisconnectCaptureThenStopReceive{false};
};

enum class IsoChannelPolicy : uint8_t {
    Fixed,
    IRMSelectable,
};

} // namespace ASFW::Audio::Duplex
