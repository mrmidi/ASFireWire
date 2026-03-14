// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// DeviceProtocolFactory.hpp - Factory for creating device-specific protocol handlers

#pragma once

#include "IDeviceProtocol.hpp"
#include "../Ports/FireWireBusPort.hpp"
#include <cstdint>
#include <memory>
#include <optional>

namespace ASFW::Audio {

/// Integration mode for a recognized device profile.
enum class DeviceIntegrationMode : uint8_t {
    kNone = 0,
    kHardcodedNub,  // Legacy path using hardcoded ASFWAudioDevice profile.
    kAVCDriven,     // AV/C discovery path with vendor extension controls.
};

/// Factory for creating device-specific protocol handlers
///
/// Call Create() during device discovery to instantiate the appropriate
/// protocol handler for known devices. Returns nullptr for unknown devices.
class DeviceProtocolFactory {
public:
    static constexpr uint32_t kFocusriteVendorId = 0x00130e;
    static constexpr uint32_t kSPro24DspModelId = 0x000008;
    static constexpr uint32_t kApogeeVendorId = 0x0003db;
    static constexpr uint32_t kApogeeDuetModelId = 0x01dddd;
    static constexpr const char* kFocusriteVendorName = "Focusrite";
    static constexpr const char* kSPro24DspModelName = "Saffire Pro 24 DSP";
    static constexpr const char* kApogeeVendorName = "Apogee";
    static constexpr const char* kApogeeDuetModelName = "Duet";

    struct KnownIdentity {
        uint32_t vendorId{0};
        uint32_t modelId{0};
        DeviceIntegrationMode integrationMode{DeviceIntegrationMode::kNone};
        const char* vendorName{nullptr};
        const char* modelName{nullptr};
    };

    static constexpr std::optional<KnownIdentity> LookupKnownIdentity(
        uint32_t vendorId,
        uint32_t modelId
    ) noexcept {
        if (vendorId == kFocusriteVendorId && modelId == kSPro24DspModelId) {
            return KnownIdentity{vendorId, modelId, DeviceIntegrationMode::kHardcodedNub,
                                 kFocusriteVendorName, kSPro24DspModelName};
        }
        if (vendorId == kApogeeVendorId && modelId == kApogeeDuetModelId) {
            return KnownIdentity{vendorId, modelId, DeviceIntegrationMode::kAVCDriven,
                                 kApogeeVendorName, kApogeeDuetModelName};
        }
        return std::nullopt;
    }

    // Focusrite DICE devices encode the board model in GUID bits [27:22].
    // The legacy macOS driver uses the same field during probe.
    static constexpr std::optional<KnownIdentity> LookupKnownIdentityByGuid(
        uint64_t guid
    ) noexcept {
        constexpr uint64_t kOuiMask = 0x00FFFFFFULL;
        constexpr unsigned kOuiShift = 40;
        constexpr unsigned kFocusriteModelShift = 22;
        constexpr uint64_t kFocusriteModelMask = 0x3FULL;

        const auto vendorId = static_cast<uint32_t>((guid >> kOuiShift) & kOuiMask);
        if (vendorId == kFocusriteVendorId) {
            const auto modelId =
                static_cast<uint32_t>((guid >> kFocusriteModelShift) & kFocusriteModelMask);
            return LookupKnownIdentity(vendorId, modelId);
        }

        return std::nullopt;
    }

    /// Resolve integration mode for a known vendor/model pair.
    static constexpr DeviceIntegrationMode LookupIntegrationMode(
        uint32_t vendorId,
        uint32_t modelId
    ) noexcept {
        if (const auto known = LookupKnownIdentity(vendorId, modelId); known.has_value()) {
            return known->integrationMode;
        }
        return DeviceIntegrationMode::kNone;
    }

    /// Check if a device is recognized by the factory.
    static constexpr bool IsKnownDevice(uint32_t vendorId, uint32_t modelId) noexcept {
        return LookupIntegrationMode(vendorId, modelId) != DeviceIntegrationMode::kNone;
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
        uint16_t nodeId
    );
};

} // namespace ASFW::Audio
