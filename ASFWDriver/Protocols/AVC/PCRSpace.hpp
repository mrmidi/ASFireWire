//
// PCRSpace.hpp
// ASFWDriver - AV/C Protocol Layer
//
// PCR (Plug Control Register) Space - IEC 61883-1 plug management
// Handles PCR read/write and CMP (Connection Management Procedures)
//

#pragma once

#include <DriverKit/IOLib.h>
#include <functional>
#include <optional>
#include "AVCDefs.hpp"
#include "AVCUnit.hpp"
#include "../../IRM/IRMAllocationManager.hpp"

namespace ASFW::Protocols::AVC {

//==============================================================================
// PCR Value
//==============================================================================

/// PCR (Plug Control Register) value
///
/// Per IEC 61883-1 §10.7, PCR layout (32-bit register):
/// ```
/// bits 31:    online (1 = channel allocated)
/// bits 30-24: broadcast_connection_counter (7 bits)
/// bits 23-16: point_to_point_connection_counter (8 bits)
/// bits 15-10: channel_number (6 bits, 0-63)
/// bits 9-8:   reserved
/// bits 7-6:   data_rate (2 bits: 0=S100, 1=S200, 2=S400, 3=S800)
/// bits 5-0:   overhead_id (6 bits)
/// ```
struct PCRValue {
    bool online{false};              ///< Channel allocated
    uint8_t broadcastCount{0};       ///< Broadcast connections (0-127)
    uint8_t p2pCount{0};             ///< Point-to-point connections (0-255)
    uint8_t channel{63};             ///< Channel number (0-63, 63=none)
    SpeedCode dataRate{SpeedCode::kS400}; ///< Data rate
    uint8_t overhead{0};             ///< Overhead ID (0-63)

    /// Encode to 32-bit PCR value
    uint32_t Encode() const {
        uint32_t value = 0;

        if (online)
            value |= (1u << 31);

        value |= (static_cast<uint32_t>(broadcastCount & 0x7F) << 24);
        value |= (static_cast<uint32_t>(p2pCount) << 16);
        value |= (static_cast<uint32_t>(channel & 0x3F) << 10);
        value |= (static_cast<uint32_t>(dataRate) << 6);
        value |= (static_cast<uint32_t>(overhead & 0x3F));

        return value;
    }

    /// Decode from 32-bit PCR value
    static PCRValue Decode(uint32_t raw) {
        PCRValue pcr;

        pcr.online = (raw & (1u << 31)) != 0;
        pcr.broadcastCount = (raw >> 24) & 0x7F;
        pcr.p2pCount = (raw >> 16) & 0xFF;
        pcr.channel = (raw >> 10) & 0x3F;
        pcr.dataRate = static_cast<SpeedCode>((raw >> 6) & 0x03);
        pcr.overhead = raw & 0x3F;

        return pcr;
    }

    /// Check if valid
    bool IsValid() const {
        return channel < 64 &&
               broadcastCount < 128 &&
               static_cast<uint8_t>(dataRate) <= 3 &&
               overhead < 64;
    }
};

//==============================================================================
// PCR Space
//==============================================================================

/// PCR Space - manages plug control registers and connections
///
/// Provides high-level API for:
/// - Reading PCR values (async quadlet read)
/// - Updating PCR values (async lock compare-swap)
/// - Creating P2P connections (allocate IRM resources + update PCRs)
/// - Destroying connections (update PCRs + free IRM resources)
///
/// **Usage**:
/// ```cpp
/// PCRSpace pcrSpace(avcUnit, irmManager);
///
/// // Create connection
/// pcrSpace.CreateConnection(0, PlugType::kOutput,
///     [](std::optional<uint8_t> channel) {
///         if (channel) {
///             os_log_info(..., "Connected on channel %u", *channel);
///         }
///     });
/// ```
class PCRSpace {
public:
    /// Constructor
    ///
    /// @param unit Associated AV/C unit
    /// @param irmManager IRM allocation manager for bandwidth/channels
    explicit PCRSpace(AVCUnit& unit, IRM::IRMAllocationManager& irmManager)
        : unit_(unit),
          irmManager_(irmManager),
          asyncSubsystem_(unit.GetAsyncSubsystem()) {}

    /// Read PCR value from device
    ///
    /// Performs async quadlet read to PCR CSR address.
    ///
    /// @param type Plug type (input/output)
    /// @param plugNum Plug number (0-30)
    /// @param completion Callback with PCR value (or nullopt on error)
    void ReadPCR(PlugType type,
                 uint8_t plugNum,
                 std::function<void(std::optional<PCRValue>)> completion);

    /// Update PCR value (atomic compare-swap)
    ///
    /// Performs async lock operation to atomically update PCR.
    ///
    /// @param type Plug type
    /// @param plugNum Plug number
    /// @param oldValue Expected current value
    /// @param newValue New value to write
    /// @param completion Callback with success flag
    void UpdatePCR(PlugType type,
                   uint8_t plugNum,
                   const PCRValue& oldValue,
                   const PCRValue& newValue,
                   std::function<void(bool success)> completion);

    /// Create P2P connection
    ///
    /// Steps:
    /// 1. Read current oPCR value
    /// 2. Allocate IRM channel + bandwidth
    /// 3. Lock-update oPCR (set online, channel, increment p2pCount)
    /// 4. On failure, rollback IRM allocation
    ///
    /// @param plugNum Output plug number
    /// @param plugType Plug type (usually kOutput for local device)
    /// @param completion Callback with allocated channel (or nullopt on failure)
    void CreateConnection(uint8_t plugNum,
                          PlugType plugType,
                          std::function<void(std::optional<uint8_t>)> completion);

    /// Destroy P2P connection
    ///
    /// Steps:
    /// 1. Read current PCR value
    /// 2. Lock-update PCR (decrement p2pCount, clear channel if count==0)
    /// 3. Free IRM channel + bandwidth
    ///
    /// @param plugNum Plug number
    /// @param plugType Plug type
    /// @param channel Channel to free
    /// @param completion Callback with success flag
    void DestroyConnection(uint8_t plugNum,
                           PlugType plugType,
                           uint8_t channel,
                           std::function<void(bool success)> completion);

private:
    /// Get PCR CSR address
    uint64_t GetPCRAddress(PlugType type, uint8_t plugNum) const {
        if (type == PlugType::kOutput) {
            return GetOPCRAddress(plugNum);
        } else {
            return GetIPCRAddress(plugNum);
        }
    }

    /// Calculate bandwidth requirement from PCR payload
    ///
    /// For now, use conservative estimate.
    /// TODO: Extract from oPCR payload field or query device.
    uint32_t CalculateBandwidth() const {
        // Conservative: 512 quadlets per packet @ S400
        // Bandwidth units are in allocation units (1 AU = 1 quadlet @ base rate)
        return 512;
    }

    AVCUnit& unit_;
    IRM::IRMAllocationManager& irmManager_;
    Async::AsyncSubsystem& asyncSubsystem_;
};

} // namespace ASFW::Protocols::AVC
