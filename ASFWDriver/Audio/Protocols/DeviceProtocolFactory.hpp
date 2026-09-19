// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// DeviceProtocolFactory.hpp - Factory for creating device-specific protocol handlers

#pragma once

#include "IDeviceProtocol.hpp"
#include "../../Protocols/Ports/FireWireBusPort.hpp"
#include "../../DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "../../DeviceProfiles/Audio/AudioProfileRegistry.hpp"
#include "../../DeviceProfiles/Audio/AudioProfileTypes.hpp"
#include "DeviceProtocolChoice.hpp"
#include "../../DeviceProfiles/Common/DeviceProfileTypes.hpp"
#include "../../Discovery/DiscoveryTypes.hpp"
#include <cstdint>
#include <memory>
#include <optional>

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

/// Integration mode for a recognized device profile.
///
/// The canonical definition lives in DeviceProfiles; this alias keeps existing
/// audio-internal call sites (e.g. DeviceIntegrationMode::kHardcodedNub) unchanged while
/// DeviceProfiles owns the data.
using DeviceIntegrationMode = DeviceProfiles::Audio::AudioIntegrationMode;

/// Unit directory identity, for families that model_id cannot discriminate.
/// Defaulted at every call site so existing (vendorId, modelId) callers are unaffected;
/// MOTU is the first family that requires it (model_id is 0, the version carries the
/// model). Namespace-scope rather than nested in DeviceProtocolFactory so it can be used
/// as a defaulted argument in that class's own member declarations.
struct DeviceUnitIdentity {
    uint32_t specId{0};
    uint32_t swVersion{0};
};

/// Factory for creating device-specific protocol handlers
///
/// Call Create() during device discovery to instantiate the appropriate
/// protocol handler for known devices. Returns nullptr for unknown devices.
///
/// Identity/profile metadata (which vendor/model is known, its display names, its
/// integration mode) is owned by ASFW::DeviceProfiles::Audio. The lookup helpers below
/// delegate to it so there is a single source of truth; this factory's remaining job is
/// runtime instantiation (Create).
class DeviceProtocolFactory {
public:
    using UnitIdentity = DeviceUnitIdentity;

    struct KnownIdentity {
        uint32_t vendorId{0};
        uint32_t modelId{0};
        DeviceIntegrationMode integrationMode{DeviceIntegrationMode::kNone};
        const char* vendorName{nullptr};
        const char* modelName{nullptr};
    };

    static constexpr KnownIdentity MakeKnownIdentity(uint32_t vendorId,
                                                     uint32_t modelId,
                                                     DeviceIntegrationMode integrationMode,
                                                     const char* vendorName,
                                                     const char* modelName) noexcept {
        return KnownIdentity{vendorId, modelId, integrationMode, vendorName, modelName};
    }

    /// Resolve a known device identity by vendor/model. Delegates to DeviceProfiles.
    static constexpr std::optional<KnownIdentity> LookupKnownIdentity(
        uint32_t vendorId,
        uint32_t modelId,
        UnitIdentity unit = {}
    ) noexcept {
        return Combine(DeviceProfiles::DeviceProfileQuery{.vendorId = vendorId,
                                                          .modelId = modelId,
                                                          .unitSpecId = unit.specId,
                                                          .unitSwVersion = unit.swVersion});
    }

    // Focusrite DICE devices encode the board model in GUID bits [27:22]. The legacy
    // macOS driver uses the same field during probe. Delegates to DeviceProfiles.
    static constexpr std::optional<KnownIdentity> LookupKnownIdentityByGuid(
        uint64_t guid
    ) noexcept {
        const auto identity = DeviceProfiles::Audio::AudioProfileRegistry::LookupIdentity(
            DeviceProfiles::DeviceProfileQuery{.guid = guid});
        if (!identity.has_value()) {
            return std::nullopt;
        }
        return Combine(DeviceProfiles::DeviceProfileQuery{.vendorId = identity->vendorId,
                                                          .modelId = identity->modelId});
    }

    /// Resolve integration mode for a known vendor/model pair. Delegates to DeviceProfiles.
    static constexpr DeviceIntegrationMode LookupIntegrationMode(
        uint32_t vendorId,
        uint32_t modelId,
        UnitIdentity unit = {}
    ) noexcept {
        const auto profile = DeviceProfiles::Audio::AudioProfileRegistry{}.LookupBestAudioProfile(
            DeviceProfiles::DeviceProfileQuery{.vendorId = vendorId,
                                               .modelId = modelId,
                                               .unitSpecId = unit.specId,
                                               .unitSwVersion = unit.swVersion});
        return profile.has_value() ? profile->mode : DeviceIntegrationMode::kNone;
    }

    /// Check if a device identity is recognized.
    static constexpr bool IsKnownDevice(uint32_t vendorId,
                                        uint32_t modelId,
                                        UnitIdentity unit = {}) noexcept {
        return LookupKnownIdentity(vendorId, modelId, unit).has_value();
    }

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

private:
    // Assemble a legacy KnownIdentity from the DeviceProfiles identity + profile hints
    // for a resolved (vendorId, modelId) query.
    static constexpr std::optional<KnownIdentity> Combine(
        const DeviceProfiles::DeviceProfileQuery& query
    ) noexcept {
        const DeviceProfiles::Audio::AudioProfileRegistry registry{};
        const auto identity = registry.LookupIdentity(query);
        if (!identity.has_value()) {
            return std::nullopt;
        }
        const auto profile = registry.LookupBestAudioProfile(query);
        const auto mode = profile.has_value() ? profile->mode : DeviceIntegrationMode::kNone;
        return MakeKnownIdentity(identity->vendorId, identity->modelId, mode, identity->vendorName,
                                 identity->modelName);
    }
};

} // namespace ASFW::Audio
