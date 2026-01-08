// BlockingDbcGenerator.hpp
// ASFW - Phase 1.5 Encoding Layer
//
// Tracks Data Block Counter (DBC) per IEC 61883-1 blocking mode rules.
//
// DBC Rules for blocking mode:
//   - DATA → DATA: DBC += samples_in_packet (8)
//   - DATA → NO-DATA: NO-DATA uses next expected DBC
//   - NO-DATA → DATA: DATA reuses the NO-DATA's DBC
//   - NO-DATA → NO-DATA: Share same DBC
//
// Reference: docs/Isoch/PHASE_1_5_ENCODING.md
// Verified against: 000-48kORIG.txt FireBug capture
//

#pragma once

#include <cstdint>

namespace ASFW {
namespace Encoding {

/// Manages Data Block Counter (DBC) for IEC 61883-1 blocking mode.
///
/// In blocking mode, the DBC tracks the number of data blocks transmitted.
/// Special rules apply for NO-DATA packets:
///   - NO-DATA packet uses the same DBC as the following DATA packet
///   - DATA packets increment DBC by the number of samples (8 at 48 kHz)
///
/// Verified sequence from 000-48kORIG.txt:
///   Cycle 977 (NO-DATA): DBC = 0xC0
///   Cycle 978 (DATA):    DBC = 0xC0 (reuses NO-DATA's DBC)
///   Cycle 979 (DATA):    DBC = 0xC8 (+8)
///   Cycle 980 (DATA):    DBC = 0xD0 (+8)
///   Cycle 981 (NO-DATA): DBC = 0xD8 (next value)
///   Cycle 982 (DATA):    DBC = 0xD8 (reuses NO-DATA's DBC)
///   ...
///
class BlockingDbcGenerator {
public:
    /// Construct with initial DBC value.
    explicit BlockingDbcGenerator(uint8_t initial = 0) noexcept
        : nextDataDbc_(initial) {}
    
    /// Get the DBC value for the current packet.
    ///
    /// @param isDataPacket True if generating DBC for a DATA packet
    /// @param samplesInPacket Number of samples in the packet (typically 8)
    /// @return The DBC value to use for this packet
    ///
    /// For DATA packets: returns current value, then increments by samplesInPacket
    /// For NO-DATA packets: returns current value without incrementing
    ///
    uint8_t getDbc(bool isDataPacket, uint8_t samplesInPacket = 8) noexcept {
        uint8_t dbc = nextDataDbc_;
        
        if (isDataPacket) {
            // Increment for next DATA packet (wraps at 256)
            nextDataDbc_ = static_cast<uint8_t>(nextDataDbc_ + samplesInPacket);
        }
        // NO-DATA: return current value without incrementing
        
        return dbc;
    }
    
    /// Get the next DBC value that would be used (without consuming it).
    uint8_t peekNextDbc() const noexcept {
        return nextDataDbc_;
    }
    
    /// Reset the DBC to a specific value.
    void reset(uint8_t initial = 0) noexcept {
        nextDataDbc_ = initial;
    }
    
private:
    uint8_t nextDataDbc_;  ///< Next DBC value for DATA packets
};

} // namespace Encoding
} // namespace ASFW
