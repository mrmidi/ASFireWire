// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FamilyProtocolConstruction.hpp - Family-keyed device protocol construction

#pragma once

#include "IDeviceProtocol.hpp"
#include "../../Protocols/Ports/FireWireBusPort.hpp"
#include "../../Discovery/DiscoveryTypes.hpp"

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
}

namespace ASFW::Audio {

/// Constructs a device-specific protocol handler keyed on the family and profile builder
/// resolved from the device record's Config ROM evidence.
///
/// Returns nullptr for unsupported devices or devices that do not use a family protocol.
[[nodiscard]] std::unique_ptr<IDeviceProtocol> CreateFamilyDeviceProtocol(
    const Discovery::DeviceRecord& record,
    Protocols::Ports::FireWireBusOps& busOps,
    Protocols::Ports::FireWireBusInfo& busInfo,
    Discovery::DeviceRegistry& routeRegistry,
    const Discovery::DeviceRouteToken& route,
    ::ASFW::IRM::IRMClient* irmClient = nullptr,
    ::ASFW::CMP::CMPClient* cmpClient = nullptr,
    Scheduling::ITimerScheduler* timerScheduler = nullptr
);

} // namespace ASFW::Audio
