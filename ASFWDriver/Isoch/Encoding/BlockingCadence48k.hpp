// BlockingCadence48k.hpp
// ASFW - Phase 1.5 Encoding Layer
//
// Implements the 48 kHz blocking cadence pattern per IEC 61883-6.
// Pattern: 6 DATA + 2 NO-DATA per 8 cycles (N-D-D-D repeating)
//
// Reference: docs/Isoch/PHASE_1_5_ENCODING.md
// Verified against: 000-48kORIG.txt FireBug capture
//

#pragma once

#include <cstdint>

namespace ASFW {
namespace Encoding {

/// Samples per DATA packet at 48 kHz (SYT interval)
constexpr uint32_t kSamplesPerPacket48k = 8;

/// DATA packets per 8 cycles at 48 kHz
constexpr uint32_t kDataPacketsPer8Cycles = 6;

/// NO-DATA packets per 8 cycles at 48 kHz
constexpr uint32_t kNoDataPacketsPer8Cycles = 2;

/// Manages the 48 kHz blocking cadence pattern.
///
/// At 48 kHz:
///   - 48,000 samples/sec ÷ 8,000 cycles/sec = 6.0 samples/cycle average
///   - SYT interval = 8 samples per DATA packet
///   - Pattern: N-D-D-D repeating (1 NO-DATA + 3 DATA per 4 cycles)
///   - Equivalently: 6 DATA + 2 NO-DATA per 8 cycles
///
/// The pattern positions NO-DATA packets at cycles 0 and 4 in each 8-cycle group:
///   Cycle 0: NO-DATA
///   Cycle 1: DATA (8 samples)
///   Cycle 2: DATA (8 samples)
///   Cycle 3: DATA (8 samples)
///   Cycle 4: NO-DATA
///   Cycle 5: DATA (8 samples)
///   Cycle 6: DATA (8 samples)
///   Cycle 7: DATA (8 samples)
///   Total: 48 samples per 8 cycles = 48,000 samples/sec
///
class BlockingCadence48k {
public:
    /// Construct a new cadence generator, starting at cycle 0.
    BlockingCadence48k() noexcept = default;
    
    /// Check if the current cycle should transmit a DATA packet.
    /// @return true if DATA packet, false if NO-DATA packet
    bool isDataPacket() const noexcept {
        // NO-DATA at cycle positions 0 and 4 (mod 4 == 0)
        return (cycleIndex_ % 4) != 0;
    }
    
    /// Get the number of samples to transmit in the current cycle.
    /// @return 8 for DATA packets, 0 for NO-DATA packets
    uint32_t samplesThisCycle() const noexcept {
        return isDataPacket() ? kSamplesPerPacket48k : 0;
    }
    
    /// Get the current cycle index (within the 8-cycle pattern).
    uint32_t getCycleIndex() const noexcept {
        return cycleIndex_ % 8;
    }
    
    /// Get the total cycle count since reset.
    uint64_t getTotalCycles() const noexcept {
        return cycleIndex_;
    }
    
    /// Advance to the next cycle.
    void advance() noexcept {
        cycleIndex_++;
    }
    
    /// Advance by multiple cycles.
    void advanceBy(uint32_t cycles) noexcept {
        cycleIndex_ += cycles;
    }
    
    /// Reset the cadence to the starting position.
    void reset() noexcept {
        cycleIndex_ = 0;
    }
    
private:
    uint64_t cycleIndex_ = 0;  ///< Running cycle counter
};

} // namespace Encoding
} // namespace ASFW
