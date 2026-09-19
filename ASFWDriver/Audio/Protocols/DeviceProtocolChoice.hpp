// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DeviceProtocolChoice.hpp — which protocol a device's identity selects.
//
// Split from DeviceProtocolFactory deliberately. The factory constructs
// DriverKit objects, so linking it pulls in every protocol class and it can
// only really run on hardware; this half is pure metadata and is checkable on
// the host for every identity in the catalog.
//
// That split is not cosmetic. Issue #115 lived entirely on this side: the
// StudioLive 24.4.2 had a profile and a DiceProfileRegistry entry but no
// clause in the factory's vendor/model if-chain, so it published a nub,
// allocated isochronous bandwidth, and then failed every StartIO with nothing
// logged. The decision was untestable, so nothing caught it.

#pragma once

#include "../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../../Discovery/DiscoveryTypes.hpp"

#include <cstdint>
#include <optional>

namespace ASFW::Audio {

/// The protocol a device's Config-ROM identity selects, and the few facts from
/// the matched unit directory that constructing it needs.
struct DeviceProtocolChoice final {
    DeviceProfiles::Audio::DeviceDefinitionId definition{
        DeviceProfiles::Audio::DeviceDefinitionId::Unknown};
    DeviceProfiles::Audio::ProfileBuilderId builder{
        DeviceProfiles::Audio::ProfileBuilderId::None};
    /// The matched unit's Unit_Sw_Version. MOTU needs it: it is the only model
    /// discriminator that family publishes, since its root model_id is 0.
    uint32_t unitVersion{0};
    uint32_t unitDirectoryOffset{0};
};

/// Walks the device's unit directories in ROM order and returns the first that
/// resolves to a definition naming a profile builder. nullopt for a device this
/// driver does not stream — including one it recognises but cannot play, which
/// is a different and deliberate state from not recognising it at all.
[[nodiscard]] std::optional<DeviceProtocolChoice>
ChooseDeviceProtocol(const Discovery::DeviceRecord& record) noexcept;

/// Which audio backend drives this device's nub.
///
/// This is the question AudioIntegrationMode::kHardcodedNub used to answer:
/// a vendor protocol owns the nub (DICE or MOTU), or the AV/C stack does.
/// Expressed here in terms of the catalog so there is one table behind it.
///
/// Rejection is represented explicitly: nullopt is returned when resolution
/// fails, the device is quarantined, or the device is recognized unsupported.
enum class AudioBackendKind : uint8_t {
    Avc = 0,
    Dice,
    MotuRegister,
};

[[nodiscard]] std::optional<AudioBackendKind>
ChooseAudioBackend(const Discovery::DeviceRecord& record) noexcept;

} // namespace ASFW::Audio
