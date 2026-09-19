// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// DeviceProtocolFactory.hpp - Factory for creating device-specific protocol handlers

#pragma once

#include "IDeviceProtocol.hpp"
#include "../../Protocols/Ports/FireWireBusPort.hpp"
#include "DeviceProtocolChoice.hpp"
#include "../../Discovery/DiscoveryTypes.hpp"
#include <cstdint>
#include <memory>

namespace ASFW::CMP {
class CMPClient;
}

namespace ASFW::Discovery {
class DeviceRegistry;
}

namespace ASFW::IRM {
class IRMClient;
}

namespace ASFW::Scheduling {
class ITimerScheduler;
} // namespace ASFW::Scheduling

namespace ASFW::Audio {

/// Factory for creating device-specific protocol handlers.
///
/// Call Create() during device discovery to instantiate the appropriate
/// protocol handler for known devices. Returns nullptr for unknown or unsupported devices.
class DeviceProtocolFactory {
public:
    /// Create a protocol handler for a discovered device.
    /// @param record   The device's registry record, carrying its Config-ROM
    ///                 identity evidence -- the catalog decides from it which
    ///                 protocol, if any, this device gets.
    /// @param busOps   FireWire bus operations port
    /// @param busInfo  FireWire bus info port
    /// @param route    Current, registry-issued route token
    /// @return Protocol handler, or nullptr if this driver does not stream it
    static std::unique_ptr<IDeviceProtocol> Create(
        const Discovery::DeviceRecord& record,
        Protocols::Ports::FireWireBusOps& busOps,
        Protocols::Ports::FireWireBusInfo& busInfo,
        Discovery::DeviceRegistry& routeRegistry,
        const Discovery::DeviceRouteToken& route,
        ::ASFW::IRM::IRMClient* irmClient = nullptr,
        ::ASFW::CMP::CMPClient* cmpClient = nullptr,
        Scheduling::ITimerScheduler* timerScheduler = nullptr
    );
};

} // namespace ASFW::Audio
