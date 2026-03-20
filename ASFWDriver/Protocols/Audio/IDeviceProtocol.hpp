// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// IDeviceProtocol.hpp - Interface for device-specific protocol handlers

#pragma once

#include <DriverKit/IOReturn.h>
#include <cstdint>
#include <functional>

namespace ASFW::Protocols::AVC {
    class FCPTransport;
}

namespace ASFW::IRM {
    class IRMClient;
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

struct AudioDuplexChannels {
    uint8_t deviceToHostIsoChannel{0};  // DICE TX / host IR
    uint8_t hostToDeviceIsoChannel{1};  // DICE RX / host IT
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

    using VoidCallback = std::function<void(IOReturn)>;

    /// Optional bring-up hook to prepare device-side duplex state at 48kHz.
    /// Drivers can call this before any IRM reservation or host IR/IT startup.
    /// Implementations should be idempotent.
    virtual void PrepareDuplex48k(const AudioDuplexChannels& channels, VoidCallback callback) {
        (void)channels;
        callback(kIOReturnUnsupported);
    }

    /// Optional hook to program the device-side RX leg after playback IRM allocation.
    virtual void ProgramRxForDuplex48k(VoidCallback callback) {
        callback(kIOReturnUnsupported);
    }

    /// Optional hook to program the device-side TX leg and enable duplex streaming.
    virtual void ProgramTxAndEnableDuplex48k(VoidCallback callback) {
        callback(kIOReturnUnsupported);
    }

    /// Optional completion hook after host IR/IT contexts are running.
    /// DICE devices can use this to verify stream lock/state.
    virtual void ConfirmDuplex48kStart(VoidCallback callback) {
        callback(kIOReturnUnsupported);
    }

    /// Optional teardown hook to stop device-side duplex state.
    virtual IOReturn StopDuplex() {
        return kIOReturnUnsupported;
    }

    /// Optional internal hook for backends that need the protocol's IRM client.
    virtual ::ASFW::IRM::IRMClient* GetIRMClient() const {
        return nullptr;
    }

    /// Update volatile runtime context that can change across bus resets.
    virtual void UpdateRuntimeContext(uint16_t nodeId,
                                      Protocols::AVC::FCPTransport* transport) {
        (void)nodeId;
        (void)transport;
    }

    /// Check if protocol can expose/control a boolean control.
    // These virtuals intentionally match the host-facing `(class, element[, value])` contract.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    virtual bool SupportsBooleanControl(uint32_t classIdFourCC,
                                        uint32_t element) const {
        (void)classIdFourCC;
        (void)element;
        return false;
    }

    /// Read protocol-backed boolean control value.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    virtual IOReturn GetBooleanControlValue(uint32_t classIdFourCC,
                                            uint32_t element,
                                            bool& outValue) {
        (void)classIdFourCC;
        (void)element;
        (void)outValue;
        return kIOReturnUnsupported;
    }

    /// Write protocol-backed boolean control value.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
