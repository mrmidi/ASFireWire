// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// IDeviceProtocol.hpp - Interface for device-specific protocol handlers

#pragma once

#include <DriverKit/IOReturn.h>
#include <cstdint>

namespace ASFW::Protocols::AVC {
    class FCPTransport;
}

namespace ASFW::Audio {

struct AudioStreamRuntimeCaps {
    // Host-facing channel counts (PCM only).
    uint32_t hostInputPcmChannels{0};   // Device -> host capture channels
    uint32_t hostOutputPcmChannels{0};  // Host -> device playback channels

    // Wire-slot counts (AM824 data block slots) when known.
    uint32_t deviceToHostAm824Slots{0}; // DICE TX stream slots (capture wire format)
    uint32_t hostToDeviceAm824Slots{0}; // DICE RX stream slots (playback wire format)

    uint32_t sampleRateHz{0};
};

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

    /// Query runtime-discovered audio stream capabilities.
    /// Returns true when the protocol has authoritative stream caps (e.g. DICE TX/RX stream formats).
    virtual bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const {
        (void)outCaps;
        return false;
    }

    /// Optional bring-up hook to configure device-side duplex streaming at 48kHz.
    /// Drivers can call this before starting host IR/IT contexts.
    /// Implementations should be idempotent and return quickly.
    virtual IOReturn StartDuplex48k() { return kIOReturnUnsupported; }

    /// Update volatile runtime context that can change across bus resets.
    virtual void UpdateRuntimeContext(uint16_t nodeId,
                                      Protocols::AVC::FCPTransport* transport) {
        (void)nodeId;
        (void)transport;
    }

    /// Check if protocol can expose/control a boolean control.
    virtual bool SupportsBooleanControl(uint32_t classIdFourCC,
                                        uint32_t element) const {
        (void)classIdFourCC;
        (void)element;
        return false;
    }

    /// Read protocol-backed boolean control value.
    virtual IOReturn GetBooleanControlValue(uint32_t classIdFourCC,
                                            uint32_t element,
                                            bool& outValue) {
        (void)classIdFourCC;
        (void)element;
        (void)outValue;
        return kIOReturnUnsupported;
    }

    /// Write protocol-backed boolean control value.
    virtual IOReturn SetBooleanControlValue(uint32_t classIdFourCC,
                                            uint32_t element,
                                            bool value) {
        (void)classIdFourCC;
        (void)element;
        (void)value;
        return kIOReturnUnsupported;
    }
};

} // namespace ASFW::Audio
