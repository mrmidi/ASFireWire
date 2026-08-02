// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// TerraTecAudioProfiles.hpp - OU-level hook for TerraTec-branded BeBoB devices.
//
// Phase88 authoritative matching moved to BeBoBDeviceProfiles.hpp (the protocol-family
// owner). This file retains the TerraTec OU hook as a thin forwarder so that any
// TerraTec-branded BeBoB device not yet in the BeBoB table still resolves by OU.
// Support-level decisions live in BeBoBDeviceProfiles.

#pragma once

#include "../../Common/DeviceProfileTypes.hpp"
#include "../AudioDeviceIds.hpp"
#include "../AudioProfileTypes.hpp"
#include "BeBoBDeviceProfiles.hpp"

#include <optional>

namespace ASFW::DeviceProfiles::Audio::TerraTec {

[[nodiscard]] constexpr std::optional<DeviceIdentityHint>
LookupIdentity(const DeviceProfileQuery& query) noexcept {
    // Phase88 and any future TerraTec BeBoB device: authoritative match in BeBoB.
    if (auto hint = BeBoB::LookupIdentity(query)) { return hint; }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<AudioProfileHint>
LookupAudioProfile(const DeviceProfileQuery& query) noexcept {
    // Phase88 and any future TerraTec BeBoB device: authoritative match in BeBoB.
    if (auto hint = BeBoB::LookupAudioProfile(query)) { return hint; }
    return std::nullopt;
}

} // namespace ASFW::DeviceProfiles::Audio::TerraTec
