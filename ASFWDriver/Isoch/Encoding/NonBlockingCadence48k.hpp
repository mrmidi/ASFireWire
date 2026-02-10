// NonBlockingCadence48k.hpp
// ASFW - Isoch Encoding Layer
//
// Implements the 48 kHz non-blocking cadence pattern per IEC 61883-6.
// Pattern: DATA every cycle, 6 data blocks per packet.
//
// 48,000 samples/sec / 8,000 cycles/sec = 6 samples/cycle.
//

#pragma once

#include <cstdint>

namespace ASFW {
namespace Encoding {

/// Samples per DATA packet at 48 kHz non-blocking mode.
constexpr uint32_t kNonBlockingSamplesPerPacket48k = 6;

/// DATA packets per 8 cycles at 48 kHz non-blocking mode.
constexpr uint32_t kNonBlockingDataPacketsPer8Cycles = 8;

/// NO-DATA packets per 8 cycles at 48 kHz non-blocking mode.
constexpr uint32_t kNonBlockingNoDataPacketsPer8Cycles = 0;

class NonBlockingCadence48k {
public:
    NonBlockingCadence48k() noexcept = default;

    /// Non-blocking mode sends DATA every cycle at 48 kHz.
    bool isDataPacket() const noexcept {
        return true;
    }

    /// 6 samples per cycle at 48 kHz.
    uint32_t samplesThisCycle() const noexcept {
        return kNonBlockingSamplesPerPacket48k;
    }

    uint32_t getCycleIndex() const noexcept {
        return cycleIndex_ % 8;
    }

    uint64_t getTotalCycles() const noexcept {
        return cycleIndex_;
    }

    void advance() noexcept {
        ++cycleIndex_;
    }

    void advanceBy(uint32_t cycles) noexcept {
        cycleIndex_ += cycles;
    }

    void reset() noexcept {
        cycleIndex_ = 0;
    }

private:
    uint64_t cycleIndex_{0};
};

} // namespace Encoding
} // namespace ASFW
