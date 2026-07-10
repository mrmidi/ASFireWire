// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// DICETransaction.hpp - DICE section/capability reader
// Reference: snd-firewire-ctl-services/protocols/dice/src/tcat.rs

#pragma once

#include "DICETypes.hpp"
#include "../../../../Protocols/Ports/ProtocolRegisterIO.hpp"
#include "../../../../Common/WireFormat.hpp"
#include <DriverKit/IOReturn.h>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace ASFW::Audio::DICE {

/// Split a decoded DICE channel-name blob (e.g. StreamFormatEntry::labels) into
/// per-channel names. The device lists names separated by a single backslash and
/// terminates the list with a double backslash. Empty tokens between separators
/// are preserved so the returned index aligns with the channel index.
/// Mirrors FFADO Device::splitNameString.
/// cross-validated with FFADO dice_avdevice.cpp (splitNameString) + ffadotypes.h.
std::vector<std::string> SplitDiceLabels(const char* labels);

/// Maximum frame size for a single DICE transaction (512 bytes per spec)
constexpr size_t kMaxFrameSize = 512;

/// Callback for DICE read operations
using DICEReadCallback = std::function<void(IOReturn status, const uint8_t* data, size_t size)>;

/// Callback for DICE write operations
using DICEWriteCallback = std::function<void(IOReturn status)>;

/// Callback for octlet-sized DICE lock/read operations
using DICEOctletCallback = std::function<void(IOReturn status, uint64_t value)>;

/// DICE section and capability reader.
///
/// Uses shared protocol-side register I/O for transport, while keeping DICE
/// parsing and address knowledge in the DICE layer.
class DICETransaction {
public:
    explicit DICETransaction(Protocols::Ports::ProtocolRegisterIO& io);
    
    /// Read general sections layout from DICE device
    /// @param callback   Callback with parsed sections
    void ReadGeneralSections(std::function<void(IOReturn, GeneralSections)> callback);

    /// Read TCAT extension sections layout from DICE device.
    /// @param callback   Callback with parsed extension sections
    void ReadExtensionSections(std::function<void(IOReturn, ExtensionSections)> callback);
    
    // ========================================================================
    // Capability Discovery
    // ========================================================================
    
    /// Read global section state (sample rate, clock capabilities, etc.)
    /// @param sections   Previously read sections (for offset)
    /// @param callback   Callback with parsed global state
    void ReadGlobalState(const GeneralSections& sections,
                         std::function<void(IOReturn, GlobalState)> callback);

    /// Read the full global section for raw reference-parity analysis.
    /// @param sections   Previously read sections (for offset)
    /// @param callback   Callback with parsed global state
    void ReadGlobalStateFull(const GeneralSections& sections,
                             std::function<void(IOReturn, GlobalState)> callback);
    
    /// Read TX stream configuration
    /// @param sections   Previously read sections (for offset)
    /// @param callback   Callback with parsed stream config
    void ReadTxStreamConfig(const GeneralSections& sections,
                            std::function<void(IOReturn, StreamConfig)> callback);
    
    /// Read RX stream configuration
    /// @param sections   Previously read sections (for offset)
    /// @param callback   Callback with parsed stream config
    void ReadRxStreamConfig(const GeneralSections& sections,
                            std::function<void(IOReturn, StreamConfig)> callback);
    
    /// Read all device capabilities (global + TX + RX streams)
    /// @param callback   Callback with complete capabilities
    void ReadCapabilities(std::function<void(IOReturn, DICECapabilities)> callback);

private:
    Protocols::Ports::ProtocolRegisterIO& io_;
    void ReadGlobalStateSized(const GeneralSections& sections,
                              size_t readSize,
                              std::function<void(IOReturn, GlobalState)> callback);
};

} // namespace ASFW::Audio::DICE
