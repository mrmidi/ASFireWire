// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// DeviceProtocolFactory.hpp - Factory for creating device-specific protocol handlers

#pragma once

#include "IDeviceProtocol.hpp"
#include "../../Protocols/Ports/FireWireBusPort.hpp"
#include "../../DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "../../DeviceProfiles/Audio/AudioProfileRegistry.hpp"
#include "../../DeviceProfiles/Audio/AudioProfileTypes.hpp"
#include "../../DeviceProfiles/Common/DeviceProfileTypes.hpp"
#include <cstdint>
#include <memory>
#include <optional>

namespace ASFW::IRM {
class IRMClient;
}

namespace ASFW::Audio {

/// Integration mode for a recognized device profile.
///
/// The canonical definition lives in DeviceProfiles; this alias keeps existing
/// audio-internal call sites (e.g. DeviceIntegrationMode::kHardcodedNub) unchanged while
/// DeviceProfiles owns the data.
using DeviceIntegrationMode = DeviceProfiles::Audio::AudioIntegrationMode;

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
    // Identity constants are defined in DeviceProfiles/Audio/AudioDeviceIds.hpp (single
    // source of truth) and re-exported here so existing DeviceProtocolFactory::kX call
    // sites keep resolving.
    static constexpr uint32_t kFocusriteVendorId = DeviceProfiles::Audio::kFocusriteVendorId;
    static constexpr uint32_t kSPro40ModelId = DeviceProfiles::Audio::kSPro40ModelId;
    static constexpr uint32_t kLiquidS56ModelId = DeviceProfiles::Audio::kLiquidS56ModelId;
    static constexpr uint32_t kSPro24ModelId = DeviceProfiles::Audio::kSPro24ModelId;
    static constexpr uint32_t kSPro24DspModelId = DeviceProfiles::Audio::kSPro24DspModelId;
    static constexpr uint32_t kSPro14ModelId = DeviceProfiles::Audio::kSPro14ModelId;
    static constexpr uint32_t kSPro26ModelId = DeviceProfiles::Audio::kSPro26ModelId;
    static constexpr uint32_t kSPro40Tcd3070ModelId = DeviceProfiles::Audio::kSPro40Tcd3070ModelId;
    static constexpr uint32_t kApogeeVendorId = DeviceProfiles::Audio::kApogeeVendorId;
    static constexpr uint32_t kApogeeDuetModelId = DeviceProfiles::Audio::kApogeeDuetModelId;
    static constexpr uint32_t kAlesisVendorId = DeviceProfiles::Audio::kAlesisVendorId;
    static constexpr uint32_t kAlesisMultiMixModelId = DeviceProfiles::Audio::kAlesisMultiMixModelId;
    static constexpr uint32_t kFocusriteGuidModelSPro40Tcd3070 =
        DeviceProfiles::Audio::kFocusriteGuidModelSPro40Tcd3070;
    static constexpr const char* kFocusriteVendorName = DeviceProfiles::Audio::kFocusriteVendorName;
    static constexpr const char* kSPro40ModelName = DeviceProfiles::Audio::kSPro40ModelName;
    static constexpr const char* kLiquidS56ModelName = DeviceProfiles::Audio::kLiquidS56ModelName;
    static constexpr const char* kSPro24ModelName = DeviceProfiles::Audio::kSPro24ModelName;
    static constexpr const char* kSPro24DspModelName = DeviceProfiles::Audio::kSPro24DspModelName;
    static constexpr const char* kSPro14ModelName = DeviceProfiles::Audio::kSPro14ModelName;
    static constexpr const char* kSPro26ModelName = DeviceProfiles::Audio::kSPro26ModelName;
    static constexpr const char* kSPro40Tcd3070ModelName =
        DeviceProfiles::Audio::kSPro40Tcd3070ModelName;
    static constexpr const char* kApogeeVendorName = DeviceProfiles::Audio::kApogeeVendorName;
    static constexpr const char* kApogeeDuetModelName = DeviceProfiles::Audio::kApogeeDuetModelName;
    static constexpr const char* kAlesisVendorName = DeviceProfiles::Audio::kAlesisVendorName;
    static constexpr const char* kAlesisMultiMixModelName =
        DeviceProfiles::Audio::kAlesisMultiMixModelName;

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
        uint32_t modelId
    ) noexcept {
        return Combine(DeviceProfiles::DeviceProfileQuery{.vendorId = vendorId, .modelId = modelId});
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
        uint32_t modelId
    ) noexcept {
        const auto profile = DeviceProfiles::Audio::AudioProfileRegistry{}.LookupBestAudioProfile(
            DeviceProfiles::DeviceProfileQuery{.vendorId = vendorId, .modelId = modelId});
        return profile.has_value() ? profile->mode : DeviceIntegrationMode::kNone;
    }

    /// Check if a device identity is recognized.
    static constexpr bool IsKnownDevice(uint32_t vendorId, uint32_t modelId) noexcept {
        return LookupKnownIdentity(vendorId, modelId).has_value();
    }

    /// Create a protocol handler for the given vendor/model
    /// @param vendorId   IEEE OUI vendor ID from Config ROM
    /// @param modelId    Model ID from Config ROM
    /// @param busOps     FireWire bus operations port
    /// @param busInfo    FireWire bus info port
    /// @param nodeId     Target device node ID
    /// @return Protocol handler, or nullptr if device is not recognized
    static std::unique_ptr<IDeviceProtocol> Create(
        uint32_t vendorId,
        uint32_t modelId,
        Protocols::Ports::FireWireBusOps& busOps,
        Protocols::Ports::FireWireBusInfo& busInfo,
        uint16_t nodeId,
        ::ASFW::IRM::IRMClient* irmClient = nullptr
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
