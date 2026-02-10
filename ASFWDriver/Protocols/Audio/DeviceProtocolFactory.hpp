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

/// Factory for creating device-specific protocol handlers
///
/// Call Create() during device discovery to instantiate the appropriate
/// protocol handler for known devices. Returns nullptr for unknown devices.
class DeviceProtocolFactory {
public:
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
    
    /// Check if a device is recognized by the factory
    /// @param vendorId  IEEE OUI vendor ID
    /// @param modelId   Model ID
    /// @return true if a protocol handler exists for this device
    static bool IsKnownDevice(uint32_t vendorId, uint32_t modelId);
};

} // namespace ASFW::Audio
