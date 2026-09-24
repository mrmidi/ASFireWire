// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FamilyProtocolConstruction.hpp - Family-keyed device protocol construction

#pragma once

#include "IDeviceProtocol.hpp"
#include "../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
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

/// Constructs a device-specific protocol handler from an already resolved
/// catalog decision. The concrete class is independent of endpoint profile.
[[nodiscard]] std::unique_ptr<IDeviceProtocol> CreateFamilyDeviceProtocol(
    const DeviceProfiles::Audio::StaticAudioEndpointPlan& plan,
    Protocols::Ports::FireWireBusOps& busOps,
    Protocols::Ports::FireWireBusInfo& busInfo,
    Discovery::DeviceRegistry& routeRegistry,
    const Discovery::DeviceRouteToken& route,
    ::ASFW::IRM::IRMClient* irmClient = nullptr,
    ::ASFW::CMP::CMPClient* cmpClient = nullptr,
    Scheduling::ITimerScheduler* timerScheduler = nullptr
);

} // namespace ASFW::Audio
