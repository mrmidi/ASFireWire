// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "AudioDeviceCatalog.hpp"
#include "../../Discovery/DeviceRouteToken.hpp"

namespace ASFW::DeviceProfiles::Audio {

/// One catalog decision bound to the physical route observed during ROM scan.
/// It owns no protocol, probe, nub, or duplex lifetime. The static plan remains
/// independent of mutable route state and of later validated wire geometry.
struct ResolvedDevicePolicy final {
    StaticAudioEndpointPlan plan;
    Discovery::DeviceRouteToken route;

    [[nodiscard]] bool AppliesTo(const Discovery::DeviceRecord& record) const noexcept {
        return static_cast<bool>(route) && record.audioPolicy.get() == this &&
               record.instanceId == plan.unit.device &&
               record.guid == route.guid &&
               record.deviceIncarnation == route.deviceIncarnation &&
               record.routeEpoch == route.routeEpoch &&
               record.gen == route.generation &&
               record.nodeId == route.nodeId &&
               record.quarantineReason == Discovery::QuarantineReason::None &&
               record.state != Discovery::LifeState::Lost &&
               record.state != Discovery::LifeState::Quarantined;
    }
};

[[nodiscard]] inline const ResolvedDevicePolicy*
CurrentAudioPolicy(const Discovery::DeviceRecord& record) noexcept {
    const auto* policy = record.audioPolicy.get();
    return policy != nullptr && policy->AppliesTo(record) ? policy : nullptr;
}

} // namespace ASFW::DeviceProfiles::Audio
