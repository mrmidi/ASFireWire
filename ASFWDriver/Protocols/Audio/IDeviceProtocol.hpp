// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// IDeviceProtocol.hpp - Interface for device-specific protocol handlers

#pragma once

#include <DriverKit/IOReturn.h>

namespace ASFW::Audio {

/// Interface for device-specific protocol handlers
///
/// Device protocols are instantiated by DeviceProtocolFactory when a
/// known device is detected during discovery. Each protocol handler
/// encapsulates vendor-specific control logic (DSP, routing, etc.).
class IDeviceProtocol {
public:
    virtual ~IDeviceProtocol() = default;
    
    /// Initialize the protocol (read device state, cache parameters)
    /// @return kIOReturnSuccess on success
    virtual IOReturn Initialize() = 0;
    
    /// Shutdown the protocol (release resources)
    /// @return kIOReturnSuccess on success
    virtual IOReturn Shutdown() = 0;
    
    /// Get human-readable device name
    virtual const char* GetName() const = 0;
    
    /// Check if device supports DSP effects
    virtual bool HasDsp() const { return false; }
    
    /// Check if device supports hardware mixer
    virtual bool HasMixer() const { return false; }

    /// Optional bring-up hook to configure device-side duplex streaming at 48kHz.
    /// Drivers can call this before starting host IR/IT contexts.
    /// Implementations should be idempotent and return quickly.
    virtual IOReturn StartDuplex48k() { return kIOReturnUnsupported; }
};

} // namespace ASFW::Audio
