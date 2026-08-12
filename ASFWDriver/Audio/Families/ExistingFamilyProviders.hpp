// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../Devices/AudioFamilyProvider.hpp"
#include "../../Protocols/Ports/FireWireBusPort.hpp"

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

namespace ASFW::Protocols::AVC {
class IAVCDiscovery;
}

namespace ASFW::Scheduling {
class ITimerScheduler;
}

namespace ASFW::Audio::Families {

// Driver-lifetime ports borrowed by the production family providers. Provider
// registration is explicit at the service composition root; no static/global
// registry participates in construction.
struct ExistingFamilyProviderDependencies final {
    Protocols::Ports::FireWireBusOps& busOps;
    Protocols::Ports::FireWireBusInfo& busInfo;
    Discovery::DeviceRegistry& routes;
    Protocols::AVC::IAVCDiscovery& avc;
    IRM::IRMClient* irm{nullptr};
    CMP::CMPClient* cmp{nullptr};
    Scheduling::ITimerScheduler& scheduler;
};

[[nodiscard]] std::unique_ptr<Devices::IAudioFamilyProvider>
MakeGenericAvcFamilyProvider(ExistingFamilyProviderDependencies dependencies);

[[nodiscard]] std::unique_ptr<Devices::IAudioFamilyProvider>
MakeBeBoBFamilyProvider(ExistingFamilyProviderDependencies dependencies);

[[nodiscard]] std::unique_ptr<Devices::IAudioFamilyProvider>
MakeDiceFamilyProvider(ExistingFamilyProviderDependencies dependencies);

[[nodiscard]] std::unique_ptr<Devices::IAudioFamilyProvider>
MakeOxfwFamilyProvider(ExistingFamilyProviderDependencies dependencies);

} // namespace ASFW::Audio::Families
