#pragma once

#include <cstdint>
#include <functional>
#include <span>

namespace ASFW::Isoch {

// ============================================================================
// OHCI Isochronous Context Control Registers
// Reference: OHCI 1.1 Specification § 10.2
// ============================================================================

namespace ContextControl {
    // Bits [31:28] - Indicator (Status)
    constexpr uint32_t kEventCodeMask       = 0x1F000000;
    constexpr uint32_t kEventCodeShift      = 24;
    // Common event codes:
    // 0x00: NO_ERROR (evt_no_status)
    // 0x02: LONG_PACKET (evt_long_packet)
    // 0x0C: MISSING_HEADER (evt_missing_header) - for IT
    // 0x0E: UNDERRUN / OVERRUN (evt_underrun / evt_overrun)
    // 0x11: DESCRIPTOR_READ (evt_descriptor_read)
    // 0x12: DATA_READ (evt_data_read)
    // 0x13: DATA_WRITE (evt_data_write)
    
    // Bits [15:15] - Run
    constexpr uint32_t kRun                 = (1 << 15);
    
    // Bits [12:12] - Wake
    constexpr uint32_t kWake                = (1 << 12);
    
    // Bits [11:11] - Dead (Read-only)
    constexpr uint32_t kDead                = (1 << 11);
    
    // Bits [10:10] - Active (Read-only)
    constexpr uint32_t kActive              = (1 << 10);
    
    // BufferFill (IR Only) - Bits [31:31] ? No, this is different for IT/IR
    // IR: bufferFill (bit 31) - Indicates packet fitting
    // IT: cycleMatch (bit 29) - Sync to cycle timer
    
    // IT Specific (OHCI §9.2):
    constexpr uint32_t kCycleMatchEnable    = (1 << 30); // cycleMatchEnable (stall until cycle match)

    // IR Specific (OHCI §10.2.2):
    constexpr uint32_t kIsochHeader         = (1 << 30); // isochHeader (preserve isochronous headers in buffer)
    constexpr uint32_t kBufferFill          = (1 << 31); // bufferFill mode

    // Mask of all writable bits (for safe clearing)
    constexpr uint32_t kWritableBits        = kRun | kWake | kCycleMatchEnable;
}

// ============================================================================
// OHCI Command Pointer Format
// Reference: OHCI 1.1 Specification § 3.2.1
// ============================================================================

namespace CommandPtr {
    constexpr uint32_t kZMask               = 0x0000000F;
    constexpr uint32_t kDescriptorAddressMask = 0xFFFFFFF0;
    
    // Z-value: Number of descriptors in the block - 1
    // e.g. Z=0 means 1 descriptor, Z=1 means 2 descriptors.
    // For simple INPUT_MORE/INPUT_LAST rings, typically Z=0 (1 descriptor per branch).
}

// ============================================================================
// Data Types
// ============================================================================

// Callback for received packets
// @param data: Span containing packet data (header + payload)
// @param status: Status bits from descriptor
// @param timestamp: Timestamp of reception
using IsochReceiveCallback = std::function<void(std::span<const uint8_t> data, uint32_t status, uint64_t timestamp)>;

} // namespace ASFW::Isoch
