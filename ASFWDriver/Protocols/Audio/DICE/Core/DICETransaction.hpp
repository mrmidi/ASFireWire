// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// DICETransaction.hpp - DICE async transaction helpers
// Reference: snd-firewire-ctl-services/protocols/dice/src/tcat.rs

#pragma once

#include "DICETypes.hpp"
#include "../../../../Async/AsyncTypes.hpp"
#include <DriverKit/IOReturn.h>
#include <cstdint>
#include <cstddef>
#include <functional>

// Forward declaration of AsyncSubsystem
namespace ASFW::Async {
    class AsyncSubsystem;
}

namespace ASFW::Audio::DICE {

// Use the existing async types
namespace Async = ::ASFW::Async;

/// Maximum frame size for a single DICE transaction (512 bytes per spec)
constexpr size_t kMaxFrameSize = 512;

/// Callback for DICE read operations
using DICEReadCallback = std::function<void(IOReturn status, const uint8_t* data, size_t size)>;

/// Callback for DICE write operations
using DICEWriteCallback = std::function<void(IOReturn status)>;

/// DICE transaction operations
/// 
/// Provides read/write operations to DICE address space.
/// All data is big-endian on the wire.
/// Uses the existing ASFW AsyncSubsystem for actual transactions.
class DICETransaction {
public:
    /// Construct with target node ID
    /// @param nodeId      Target node ID (from device discovery)
    explicit DICETransaction(uint16_t nodeId);
    
    /// Read a single quadlet from DICE address space
    /// @param subsystem  Async subsystem reference
    /// @param offset     Offset from DICE base address
    /// @param callback   Callback with result (value in host byte order)
    void ReadQuadlet(Async::AsyncSubsystem& subsystem, uint32_t offset,
                     std::function<void(IOReturn, uint32_t)> callback);
    
    /// Write a single quadlet to DICE address space
    /// @param subsystem  Async subsystem reference  
    /// @param offset     Offset from DICE base address
    /// @param value      Value to write (host byte order, will be converted to big-endian)
    /// @param callback   Callback with result
    void WriteQuadlet(Async::AsyncSubsystem& subsystem, uint32_t offset,
                      uint32_t value, DICEWriteCallback callback);
    
    /// Read a block of data from DICE address space
    /// @param subsystem   Async subsystem reference
    /// @param offset      Offset from DICE base address
    /// @param byteCount   Number of bytes to read (must be quadlet-aligned)
    /// @param callback    Callback with result (data in big-endian wire format)
    void ReadBlock(Async::AsyncSubsystem& subsystem, uint32_t offset,
                   size_t byteCount, DICEReadCallback callback);
    
    /// Write a block of data to DICE address space
    /// @param subsystem   Async subsystem reference
    /// @param offset      Offset from DICE base address
    /// @param buffer      Data to write (in big-endian wire format)
    /// @param byteCount   Number of bytes to write (must be quadlet-aligned)
    /// @param callback    Callback with result
    void WriteBlock(Async::AsyncSubsystem& subsystem, uint32_t offset,
                    const uint8_t* buffer, size_t byteCount, DICEWriteCallback callback);
    
    /// Read general sections layout from DICE device
    /// @param subsystem  Async subsystem reference
    /// @param callback   Callback with parsed sections
    void ReadGeneralSections(Async::AsyncSubsystem& subsystem,
                             std::function<void(IOReturn, GeneralSections)> callback);
    
    // ========================================================================
    // Capability Discovery
    // ========================================================================
    
    /// Read global section state (sample rate, clock capabilities, etc.)
    /// @param subsystem  Async subsystem reference
    /// @param sections   Previously read sections (for offset)
    /// @param callback   Callback with parsed global state
    void ReadGlobalState(Async::AsyncSubsystem& subsystem, const GeneralSections& sections,
                         std::function<void(IOReturn, GlobalState)> callback);
    
    /// Read TX stream configuration
    /// @param subsystem  Async subsystem reference
    /// @param sections   Previously read sections (for offset)
    /// @param callback   Callback with parsed stream config
    void ReadTxStreamConfig(Async::AsyncSubsystem& subsystem, const GeneralSections& sections,
                            std::function<void(IOReturn, StreamConfig)> callback);
    
    /// Read RX stream configuration
    /// @param subsystem  Async subsystem reference
    /// @param sections   Previously read sections (for offset)
    /// @param callback   Callback with parsed stream config
    void ReadRxStreamConfig(Async::AsyncSubsystem& subsystem, const GeneralSections& sections,
                            std::function<void(IOReturn, StreamConfig)> callback);
    
    /// Read all device capabilities (global + TX + RX streams)
    /// @param subsystem  Async subsystem reference
    /// @param callback   Callback with complete capabilities
    void ReadCapabilities(Async::AsyncSubsystem& subsystem,
                          std::function<void(IOReturn, DICECapabilities)> callback);
    
    // ========================================================================
    // Utility Functions (static)
    // ========================================================================
    
    /// Convert big-endian wire quadlet to host byte order
    static uint32_t QuadletFromWire(const uint8_t* data) {
        return (static_cast<uint32_t>(data[0]) << 24) |
               (static_cast<uint32_t>(data[1]) << 16) |
               (static_cast<uint32_t>(data[2]) << 8)  |
               (static_cast<uint32_t>(data[3]));
    }
    
    /// Convert host byte order quadlet to big-endian wire format
    static void QuadletToWire(uint32_t value, uint8_t* data) {
        data[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
        data[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
        data[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
        data[3] = static_cast<uint8_t>(value & 0xFF);
    }

private:
    uint16_t nodeId_;
    
    /// Scratch buffer for quadlet writes (DriverKit doesn't support thread_local)
    uint8_t writeQuadletBuffer_[4]{};
    
    /// Build ReadParams for DICE address
    Async::ReadParams MakeReadParams(uint32_t offset, uint32_t length) const;
    
    /// Build WriteParams for DICE address
    Async::WriteParams MakeWriteParams(uint32_t offset, const void* data, uint32_t length) const;
};

} // namespace ASFW::Audio::DICE
