#pragma once

#include <cstdint>
#include "../Discovery/DiscoveryTypes.hpp"  // For Generation type
#include <DriverKit/IOLib.h>

namespace ASFW::IRM {

using Generation = ::ASFW::Discovery::Generation;

// ============================================================================
// IEEE 1394 IRM CSR Registers
// ============================================================================

/**
 * IRM Register Addresses (IEEE 1394-1995 §8.3.2.3.4)
 *
 * All IRM registers are in CSR space (0xFFFFF0000000 base).
 * CRITICAL: All IRM register accesses MUST use S100 speed per specification.
 *
 * Reference: Apple IOFireWireController.cpp:4752 - Forces S100 for IRM registers
 *            Linux firewire-core-cdev.c - Uses fw_run_transaction with TCODE_LOCK_COMPARE_SWAP
 */
namespace IRMRegisters {
    /// CSR space address high (constant for all CSR registers)
    constexpr uint16_t kAddressHi = 0xFFFF;

    /// IRM registers (all 4-byte quadlets, accessed at S100 only)
    constexpr uint32_t kBandwidthAvailable = 0xF0000220;      ///< Available isoch bandwidth units
    constexpr uint32_t kChannelsAvailable31_0 = 0xF0000224;   ///< Channels 0-31 availability mask
    constexpr uint32_t kChannelsAvailable63_32 = 0xF0000228;  ///< Channels 32-63 availability mask
    constexpr uint32_t kBroadcastChannel = 0xF0000234;        ///< Broadcast channel register
}

// ============================================================================
// Bandwidth Calculation (IEEE 1394-1995 §8.3.2.3.5)
// ============================================================================

/**
 * Maximum bandwidth units available at S400.
 * Per IEEE 1394, total bus bandwidth = 4915 allocation units at S400.
 *
 * Calculation: 400 Mbps / 196 KB/s per unit ≈ 4915 units
 *
 * Reference: Apple IOFireWireFamily/IOFireWireController.cpp
 *            Linux firewire-ohci.c BANDWIDTH_AVAILABLE_INITIAL
 */
constexpr uint32_t kMaxBandwidthUnitsS400 = 4915;

/**
 * Initial value for CHANNELS_AVAILABLE registers after bus reset.
 * Bit N set (1) = channel N available
 * Bit N clear (0) = channel N allocated
 *
 * Note: Some channels may be reserved by IRM (e.g., channel 31 for broadcast).
 */
constexpr uint32_t kChannelsAvailableInitial = 0xFFFFFFFF;  ///< All channels free

/**
 * Calculate bandwidth units needed for given bitrate and speed.
 *
 * Formula (from IEEE 1394-1995 Annex C):
 *   units = (bits_per_second * overhead_factor) / speed_mbps * max_units
 *
 * @param bitsPerSecond Required bandwidth in bits per second
 * @param speedMbps Bus speed in Mbps (100, 200, 400, 800)
 * @param overheadPercent Overhead factor (default 10% for CIP headers, retries, etc.)
 * @return Number of bandwidth units to allocate
 *
 * Example:
 *   Audio: 48kHz * 24-bit * 2ch = 2.304 Mbps
 *   At S400 with 10% overhead:
 *   units = (2.304 * 1.10) / 400 * 4915 ≈ 31 units
 *
 * Reference: Apple IOFireWireController.cpp bandwidth allocation
 *            IEC 61883 overhead calculations
 */
inline uint32_t CalculateBandwidthUnits(uint32_t bitsPerSecond,
                                        uint32_t speedMbps,
                                        uint32_t overheadPercent = 10)
{
    // Convert bits/sec to Mbits/sec (round up)
    uint32_t mbitsPerSec = (bitsPerSecond + 999999) / 1000000;

    // Add overhead
    mbitsPerSec = (mbitsPerSec * (100 + overheadPercent)) / 100;

    // Scale to S400 bandwidth units
    uint32_t units = (mbitsPerSec * kMaxBandwidthUnitsS400) / speedMbps;

    return units;
}

/**
 * Calculate bit position for channel in CHANNELS_AVAILABLE register.
 *
 * Bit mapping (IEEE 1394-1995):
 *   CHANNELS_AVAILABLE_31_0:  bit 31 = channel 0, bit 0 = channel 31
 *   CHANNELS_AVAILABLE_63_32: bit 31 = channel 32, bit 0 = channel 63
 *
 * @param channel Channel number (0-63)
 * @return Bit position (0-31)
 *
 * Example:
 *   Channel 5  → register 31_0, bit 26 → mask 0x04000000
 *   Channel 35 → register 63_32, bit 28 → mask 0x10000000
 */
inline uint32_t ChannelToBitMask(uint8_t channel) {
    if (channel < 32) {
        return 1u << (31 - channel);
    } else {
        return 1u << (63 - channel);
    }
}

/**
 * Determine which CHANNELS_AVAILABLE register for given channel.
 *
 * @param channel Channel number (0-63)
 * @return Register address (kChannelsAvailable31_0 or kChannelsAvailable63_32)
 */
inline uint32_t ChannelToRegisterAddress(uint8_t channel) {
    return (channel < 32) ? IRMRegisters::kChannelsAvailable31_0
                          : IRMRegisters::kChannelsAvailable63_32;
}

// ============================================================================
// Allocation Status and Result Types
// ============================================================================

/**
 * IRM allocation operation status.
 *
 * Design Philosophy (from IRM_FINAL_THOUGHTS.md §6):
 * - Small and explicit status codes
 * - No hidden meanings
 * - Generation mismatches expressed via status, not new types
 *
 * Reference: Apple IOFireWireController allocateIRMChannelInGeneration() return codes
 *            Linux firewire-core-cdev.c FW_CDEV_EVENT_ISO_RESOURCE_* events
 */
enum class AllocationStatus : uint8_t {
    /// Allocation succeeded (CAS lock succeeded)
    Success,

    /// Insufficient resources
    /// - Channel: Bit already clear (channel allocated by another node)
    /// - Bandwidth: Insufficient units available
    NoResources,

    /// Generation mismatch
    /// - Caller's generation != IRMClient's internal generation, OR
    /// - Bus ops report bus reset / stale generation
    GenerationMismatch,

    /// IRM node didn't respond within timeout
    Timeout,

    /// No IRM node on bus, or CSR access returns address_error
    NotFound,

    /// Generic failure (unexpected state, hardware error, etc.)
    Failed
};

[[nodiscard]] constexpr const char* ToString(AllocationStatus status) noexcept {
    switch (status) {
        case AllocationStatus::Success:
            return "success";
        case AllocationStatus::NoResources:
            return "no_resources";
        case AllocationStatus::GenerationMismatch:
            return "generation_mismatch";
        case AllocationStatus::Timeout:
            return "timeout";
        case AllocationStatus::NotFound:
            return "not_found";
        case AllocationStatus::Failed:
            return "failed";
    }
    return "unknown";
}

/**
 * Result of channel allocation operation.
 *
 * Usage:
 *   ChannelAllocation result = irmClient.AllocateChannel(5, generation);
 *   if (result.status == AllocationStatus::Success) {
 *       // Use result.channel for isochronous transmission
 *   }
 */
struct ChannelAllocation {
    uint8_t channel{0xFF};              ///< Allocated channel (0xFF = no channel)
    AllocationStatus status{AllocationStatus::Failed};
    Generation generation{0};           ///< Generation when allocation succeeded
};

/**
 * Result of bandwidth allocation operation.
 *
 * Usage:
 *   BandwidthAllocation result = irmClient.AllocateBandwidth(100, generation);
 *   if (result.status == AllocationStatus::Success) {
 *       // Bandwidth reserved, proceed with isochronous setup
 *   }
 */
struct BandwidthAllocation {
    uint32_t units{0};                  ///< Allocated bandwidth units
    AllocationStatus status{AllocationStatus::Failed};
    Generation generation{0};           ///< Generation when allocation succeeded
};

/**
 * Combined channel + bandwidth allocation result.
 *
 * Used by AllocateResources() which performs two-phase commit:
 * 1. Allocate channel
 * 2. Allocate bandwidth
 * 3. If bandwidth fails, release channel (rollback)
 *
 * Usage:
 *   ResourceAllocation result = irmClient.AllocateResources(5, 100, generation);
 *   if (result.status == AllocationStatus::Success) {
 *       // Both channel and bandwidth reserved
 *       StartIsochTransmission(result.channel, result.bandwidthUnits);
 *   }
 */
struct ResourceAllocation {
    uint8_t channel{0xFF};              ///< Allocated channel (0xFF = no channel)
    uint32_t bandwidthUnits{0};         ///< Allocated bandwidth units
    AllocationStatus status{AllocationStatus::Failed};
    Generation generation{0};           ///< Generation when allocation succeeded
};

// ============================================================================
// Retry Configuration
// ============================================================================

/**
 * Retry policy for IRM allocation operations.
 *
 * IRM operations may fail due to contention (another node modified register
 * between read and CAS). Retry policy controls how many times to retry.
 *
 * Reference: Apple IOFireWireIRM.cpp:197 - Uses 8 retries for broadcast channel
 *            Apple IOFireWireController.cpp:6391 - Uses 2 retries for channel allocation
 */
struct RetryPolicy {
    uint8_t maxRetries{2};       ///< Max retry attempts (Apple default: 2)
    uint64_t retryDelayUsec{0};  ///< Delay between retries (0 = immediate)

    /// Default policy: 2 retries, no delay (Apple standard)
    static RetryPolicy Default() { return {2, 0}; }

    /// Aggressive policy: 8 retries (for broadcast channel allocation)
    static RetryPolicy Aggressive() { return {8, 0}; }

    /// No retries (single attempt)
    static RetryPolicy None() { return {0, 0}; }
};

} // namespace ASFW::IRM
