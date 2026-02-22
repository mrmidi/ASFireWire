// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// DeviceProtocolFactory.hpp - Factory for creating device-specific protocol handlers

#pragma once

#include "IDeviceProtocol.hpp"
#include <cstdint>
#include <memory>

// Forward declaration
namespace ASFW::Async {
    class AsyncSubsystem;
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
    static constexpr uint32_t kSPro24DspModelId = 0x000008;
    static constexpr uint32_t kApogeeVendorId = 0x0003db;
    static constexpr uint32_t kApogeeDuetModelId = 0x01dddd;

    /// Resolve integration mode for a known vendor/model pair.
    static constexpr DeviceIntegrationMode LookupIntegrationMode(
        uint32_t vendorId,
        uint32_t modelId
    ) noexcept {
        if (vendorId == kFocusriteVendorId && modelId == kSPro24DspModelId) {
            return DeviceIntegrationMode::kHardcodedNub;
        }
        if (vendorId == kApogeeVendorId && modelId == kApogeeDuetModelId) {
            return DeviceIntegrationMode::kAVCDriven;
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
    /// @param subsystem  Async subsystem for FireWire operations
    /// @param nodeId     Target device node ID
    /// @return Protocol handler, or nullptr if device is not recognized
    static std::unique_ptr<IDeviceProtocol> Create(
        uint32_t vendorId,
        uint32_t modelId,
        Async::AsyncSubsystem& subsystem,
        uint16_t nodeId
    );
};

} // namespace ASFW::Audio
