#pragma once

#include <cstdint>
#include "../../Discovery/DiscoveryTypes.hpp"  // For Generation type
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
// Bandwidth Calculation (IEEE 1394-1995 / IEEE 1394a-2000 §8.4.2.2)
// ============================================================================

/**
 * Maximum bandwidth allocation units available per 125 µs cycle (IEEE 1394 §8.4.2.2).
 *
 * The IRM BANDWIDTH_AVAILABLE register (CSR offset 0x220) specifies the remaining
 * isochronous bandwidth allocation units on the bus.
 *
 * Specification & Timing Derivation:
 * - 1 bandwidth allocation unit = time to transmit 1 quadlet (32 bits) at S1600
 *   with base transmission clock 49.152 MHz:
 *     t_unit = 1 / 49.152 MHz = 20.34505 ns.
 * - Total allocation units in a nominal 8 kHz (125 µs) isochronous cycle:
 *     125 µs × 49.152 MHz = 6,144 allocation units.
 * - IEEE 1394 §8.4.2.2 strictly caps isochronous transmissions to at most 100 µs
 *   (80% of the 125 µs cycle) to guarantee bus availability for asynchronous traffic:
 *     100 µs × 49.152 MHz = 4,915.2 ≈ 4,915 allocation units (0x1333).
 * - The remaining 25 µs (1,228.8 ≈ 1,229 units, or 20% of the cycle) is the mandatory
 *   async cycle remainder, dedicated to cycle start packets, arbitration gaps,
 *   and asynchronous transaction requests and responses.
 *
 * Reference:
 *   Apple IOFireWireController.cpp:6302 - Initial bandwidth 0x1333 (4915)
 *   Linux core.h:46 - BANDWIDTH_AVAILABLE_INITIAL 4915
 */
constexpr uint32_t kMaxBandwidthUnitsS400 = 4915;

/**
 * Initial value for CHANNELS_AVAILABLE registers after bus reset.
 * Bit N set (1) = channel N available
 * Bit N clear (0) = channel N allocated
 *
 * Note: Some channels may be reserved by IRM (e.g., channel 31 for broadcast).
 * cross-validated with Linux: ohci.c:2492 Apple: IOFireWireIRM.cpp:238
 */
constexpr uint32_t kChannelsAvailableInitial = 0xFFFFFFFF;  ///< All channels free

/**
 * Isochronous packet cost, in IEEE 1394 bandwidth allocation units.
 *
 * Specification: IEEE 1394-1995 / IEEE 1394a-2000 Clause 8.4.2.2.
 *
 * Formula:
 *   units = (ceil(payloadBytes / 4) + 3) * 16 / (1 << speedCode)
 *
 * Component Breakdown:
 * - payloadBytes: Isochronous data payload including IEC 61883 CIP header,
 *   excluding the 1394 isochronous packet header.
 * - quadlets = (payloadBytes + 3) / 4: Payload bytes rounded up to quadlets.
 * - +3 quadlets: Mandatory 1394 isochronous packet framing overhead:
 *     1 quadlet: Isochronous packet header (data_length:16, tag:2, channel:6, tcode:4, sy:4)
 *     1 quadlet: Header CRC
 *     1 quadlet: Data CRC
 * - * 16: Scales transmission time at S100 to S1600 allocation units
 *   (since S100 is 16× slower than S1600).
 * - >> speedCode: Divides duration by 2^speedCode (1 for S100, 2 for S200, 4 for S400, 8 for S800).
 *
 * Apple and Linux Parity:
 * - Apple IOFWIsochChannel.cpp:664:
 *     bandwidth = (fPacketSize / 4 + 3) * 16 / (1 << inSpeed);
 * - Linux sound/firewire/iso-resources.c:48-61:
 *     packet_bandwidth(max_payload_bytes, speed);
 *
 * Apple charges strictly this packet term against BANDWIDTH_AVAILABLE. Zero gap
 * overhead is subtracted from the IRM ledger because the 25 µs (1,229 units)
 * cycle remainder already accommodates isochronous arbitration gaps.
 *
 * @param payloadBytes Packet payload including CIP headers, excluding the
 *                     1394 isochronous header.
 * @param speedCode    0=S100, 1=S200, 2=S400, 3=S800.
 */
[[nodiscard]] constexpr uint32_t PacketBandwidthUnits(uint32_t payloadBytes,
                                                      uint8_t speedCode) noexcept {
    const uint32_t quadlets = (payloadBytes + 3U) / 4U;
    const uint32_t unitsAtS1600 = (quadlets + 3U) * 16U;
    return speedCode >= 4U ? unitsAtS1600 : unitsAtS1600 >> speedCode;
}

/**
 * Bus arbitration gap overhead for IEC 61883-1 Connection Management Protocol (CMP).
 *
 * Specification: IEC 61883-1:2001 Clause 5.3.2 Table 5 (Plug Control Registers: oPCR/iPCR).
 *
 * Architecture Distinction (IEEE 1394 IRM vs. IEC 61883 CMP):
 * - IEEE 1394 IRM (CSR 0xFFFFF0000220 BANDWIDTH_AVAILABLE):
 *   A bus-wide shared allocation counter. Apple IOFWIsochChannel.cpp:664 does NOT
 *   charge gap overhead against BANDWIDTH_AVAILABLE because arbitration gaps are
 *   subsumed in the 25 µs (1,229 units) async cycle remainder.
 *
 * - IEC 61883-1 CMP (CSR 0xFFFFF0000900 oPCR[n] bits [13:10] overhead_id):
 *   The overhead_id field in output plug control registers communicates the expected
 *   arbitration and packet gap delay across hops (in quanta of 32 allocation units)
 *   to receiving nodes for buffer dimensioning and media clock synchronization.
 *
 * Formula:
 *   For gapCount < 63: overhead ≈ (gapCount * 9.7) + 89 units.
 *   For unoptimized bus (gapCount = 63): worst-case 512 units (overhead_id = 0, or 16 * 32 units).
 */
[[nodiscard]] constexpr uint32_t BandwidthOverheadForGapCount(uint8_t gapCount) noexcept {
    return gapCount < 63U ? (static_cast<uint32_t>(gapCount) * 97U) / 10U + 89U : 512U;
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

    /// A lock lost its race and ran out of retries. The ledger moved between
    /// our read and our compare-swap, so ownership of the resource is unknown
    /// rather than known-denied.
    NoResources,

    /// The requested channel's bit was already clear: another node owns it.
    ChannelBusy,

    /// BANDWIDTH_AVAILABLE held fewer units than the request needed.
    BandwidthShort,

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
            return "lock_contention";
        case AllocationStatus::ChannelBusy:
            return "channel_busy";
        case AllocationStatus::BandwidthShort:
            return "bandwidth_short";
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
    uint8_t maxRetries{2};       ///< Max retry attempts on lock contention (Apple default: 2)
    uint64_t retryDelayUsec{0};  ///< Delay between retries (0 = immediate)

    /// Re-issues of a read or lock the IRM never answered (async timeout).
    /// Apple retries every timed-out async command this many times
    /// (IOFWCommand.h:37 kFWCmdDefaultRetries, IOFWAsyncCommand.cpp:425-461);
    /// Linux loops its IRM compare-swap up to 5 times on any non-generation
    /// failure (core-iso.c:296-320).
    uint8_t maxTimeoutRetries{3};

    /// Default policy: 2 contention retries, 3 timeout re-issues, no delay
    static RetryPolicy Default() { return {2, 0}; }

    /// Aggressive policy: 8 contention retries (for broadcast channel allocation)
    static RetryPolicy Aggressive() { return {8, 0}; }

    /// No retries of any kind (single attempt)
    static RetryPolicy None() { return {0, 0, 0}; }
};

} // namespace ASFW::IRM
