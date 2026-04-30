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

namespace ASFW::IRM {
class IRMClient;
}

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
    static constexpr uint32_t kSPro40ModelId = 0x000005;
    static constexpr uint32_t kLiquidS56ModelId = 0x000006;
    static constexpr uint32_t kSPro24ModelId = 0x000007;
    static constexpr uint32_t kSPro24DspModelId = 0x000008;
    static constexpr uint32_t kSPro14ModelId = 0x000009;
    static constexpr uint32_t kSPro26ModelId = 0x000012;
    static constexpr uint32_t kSPro40Tcd3070ModelId = 0x0000de;
    static constexpr uint32_t kApogeeVendorId = 0x0003db;
    static constexpr uint32_t kApogeeDuetModelId = 0x01dddd;
    static constexpr uint32_t kAlesisVendorId = 0x000595;
    static constexpr uint32_t kAlesisMultiMixModelId = 0x000000;
    static constexpr uint32_t kMidasVendorId = 0x10c73f;
    static constexpr uint32_t kMidasVeniceF32ModelId = 0x000001;
    static constexpr uint32_t kFocusriteGuidModelSPro40Tcd3070 = 0x13;
    static constexpr const char* kFocusriteVendorName = "Focusrite";
    static constexpr const char* kSPro40ModelName = "Saffire Pro 40";
    static constexpr const char* kLiquidS56ModelName = "Liquid Saffire 56";
    static constexpr const char* kSPro24ModelName = "Saffire Pro 24";
    static constexpr const char* kSPro24DspModelName = "Saffire Pro 24 DSP";
    static constexpr const char* kSPro14ModelName = "Saffire Pro 14";
    static constexpr const char* kSPro26ModelName = "Saffire Pro 26";
    static constexpr const char* kSPro40Tcd3070ModelName = "Saffire Pro 40 (TCD3070)";
    static constexpr const char* kApogeeVendorName = "Apogee";
    static constexpr const char* kApogeeDuetModelName = "Duet";
    static constexpr const char* kAlesisVendorName = "Alesis";
    static constexpr const char* kAlesisMultiMixModelName = "MultiMix FireWire";
    static constexpr const char* kMidasVendorName = "Midas";
    static constexpr const char* kMidasVeniceF32ModelName = "Venice F32";
    static constexpr const char* kMidasVeniceGenericModelName = "Venice DICE";

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

    static constexpr std::optional<KnownIdentity> LookupKnownIdentity(
        uint32_t vendorId,
        uint32_t modelId
    ) noexcept {
        if (vendorId == kFocusriteVendorId) {
            switch (modelId) {
                case kSPro14ModelId:
                    return MakeKnownIdentity(vendorId, modelId, DeviceIntegrationMode::kHardcodedNub,
                                             kFocusriteVendorName, kSPro14ModelName);
                case kSPro24ModelId:
                    return MakeKnownIdentity(vendorId, modelId, DeviceIntegrationMode::kHardcodedNub,
                                             kFocusriteVendorName, kSPro24ModelName);
                case kSPro24DspModelId:
                    return MakeKnownIdentity(vendorId, modelId, DeviceIntegrationMode::kHardcodedNub,
                                             kFocusriteVendorName, kSPro24DspModelName);
                case kSPro40ModelId:
                    return MakeKnownIdentity(vendorId, modelId, DeviceIntegrationMode::kNone,
                                             kFocusriteVendorName, kSPro40ModelName);
                case kLiquidS56ModelId:
                    return MakeKnownIdentity(vendorId, modelId, DeviceIntegrationMode::kNone,
                                             kFocusriteVendorName, kLiquidS56ModelName);
                case kSPro26ModelId:
                    return MakeKnownIdentity(vendorId, modelId, DeviceIntegrationMode::kNone,
                                             kFocusriteVendorName, kSPro26ModelName);
                case kSPro40Tcd3070ModelId:
                    return MakeKnownIdentity(vendorId, modelId, DeviceIntegrationMode::kNone,
                                             kFocusriteVendorName, kSPro40Tcd3070ModelName);
                default:
                    break;
            }
        }
        if (vendorId == kApogeeVendorId && modelId == kApogeeDuetModelId) {
            return MakeKnownIdentity(vendorId, modelId, DeviceIntegrationMode::kAVCDriven,
                                     kApogeeVendorName, kApogeeDuetModelName);
        }
        if (vendorId == kAlesisVendorId && modelId == kAlesisMultiMixModelId) {
            return MakeKnownIdentity(vendorId, modelId, DeviceIntegrationMode::kHardcodedNub,
                                     kAlesisVendorName, kAlesisMultiMixModelName);
        }
        if (vendorId == kMidasVendorId) {
            return MakeKnownIdentity(vendorId,
                                     modelId,
                                     DeviceIntegrationMode::kHardcodedNub,
                                     kMidasVendorName,
                                     modelId == kMidasVeniceF32ModelId
                                         ? kMidasVeniceF32ModelName
                                         : kMidasVeniceGenericModelName);
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
            auto modelId =
                static_cast<uint32_t>((guid >> kFocusriteModelShift) & kFocusriteModelMask);
            if (modelId == kFocusriteGuidModelSPro40Tcd3070) {
                modelId = kSPro40Tcd3070ModelId;
            }
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

    /// Check if a device identity is recognized by the factory.
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
};

} // namespace ASFW::Audio
