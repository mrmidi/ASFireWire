// OHCI_HW_Specs.hpp

#pragma once

#include <cstdint>

// New split headers for descriptors and IEEE1394 wire-format helpers
#include "OHCIDescriptors.hpp"
#include "IEEE1394.hpp"

// Generic OHCI hardware helpers (protocol-agnostic)
#include "../Shared/Hardware/OHCIHelpers.hpp"

// This header is a compatibility umbrella for the legacy `OHCI_HW_Specs.hpp` and
// includes the new smaller headers. Gradually update files to reference the
// specific smaller headers to reduce compile times and increase modularity.
///
/// CRITICAL ENDIANNESS REQUIREMENTS:
/// - OHCI Descriptors (OHCIDescriptor, OHCIDescriptorImmediate): Host byte order (little-endian on x86/ARM)
///   Per OHCI §7: "Descriptors are fetched via PCI in the host's native byte order"
/// - 1394 Packet Headers (AsyncRequestHeader, AsyncReceiveHeader): Big-endian (IEEE 1394 wire format)
///   Per IEEE 1394-1995 §6.2: "All multi-byte fields transmitted MSB first"
///
/// ALIGNMENT REQUIREMENTS:
/// - All descriptors MUST be 16-byte aligned (OHCI §7.1, Table 7-3: branchAddress field)
/// - Descriptor chains MUST start with an *-Immediate descriptor (OHCI §7.1.5.1, Table 7-5)

namespace ASFW::Async::HW {

// Import generic OHCI constants and endianness helpers from Shared
// (These are protocol-agnostic and apply to all OHCI controllers)
using ASFW::Shared::kOHCIDmaAddressBits;
using ASFW::Shared::kOHCIBranchAddressBits;
using ASFW::Shared::ToBigEndian16;
using ASFW::Shared::ToBigEndian32;
using ASFW::Shared::ToBigEndian64;
using ASFW::Shared::FromBigEndian16;
using ASFW::Shared::FromBigEndian32;
using ASFW::Shared::FromBigEndian64;

/// Constructs an OHCI AT (Asynchronous Transmit) descriptor branchWord.
///
/// Spec References:
/// - OHCI §7.1.5.1 "Command.Z": Defines Z value encoding (Table 7-5)
/// - OHCI Table 7-3: branchWord format = physAddr[31:4] | Z[3:0]
/// - OHCI Table 7-5: Z valid range is 1-15 for AT descriptors (1 block = 16 bytes)
///   Z=0 means end-of-list, hardware stops fetching
///
/// @param physAddr 16-byte aligned physical address (bits [3:0] must be zero)
/// @param Zblocks Block count of next descriptor chain (1-15 units of 16 bytes; 0=end-of-list)
/// @return Packed 32-bit branchWord = physAddr[31:4] | Z[3:0], or 0 if invalid
[[nodiscard]] constexpr uint32_t MakeBranchWordAT(uint64_t physAddr, uint8_t Zblocks) noexcept {
    // Validate per OHCI Table 7-3: "16-byte aligned address"
    // Validate per OHCI Table 7-5: Z must be 0 (EOL) or 2-8 (valid descriptor block counts)
    // Z=1 is reserved (all descriptor blocks must start with *-Immediate = minimum 2 blocks)
    // Z=9-15 are reserved
    if ((physAddr & 0xFULL) != 0 || physAddr > 0xFFFFFFFFu) {
        return 0;
    }
    if (Zblocks != 0 && (Zblocks < 2 || Zblocks > 8)) {
        return 0;  // Reject reserved Z values (1, 9-15)
    }
    // CommandPtr/branchWord format: physAddr[31:4] in bits[31:4], Z in bits[3:0]
    return (static_cast<uint32_t>(physAddr) & 0xFFFFFFF0u) | (static_cast<uint32_t>(Zblocks) & 0xFu);
}

/// Constructs an OHCI AR (Asynchronous Receive) descriptor branchWord.
///
/// Spec References:
/// - OHCI Figure 8-1 / Table 8-1: "Z may be set to 0 or 1"
/// - "If this is the last descriptor in the context program, Z must be 0, otherwise it must be 1"
/// - OHCI Table 8-1: branchAddress field = bits[31:4] of physical address, Z = bit[0]
///
/// CRITICAL DIFFERENCE FROM AT DESCRIPTORS:
/// - AT (Table 7-3): branchWord = physAddr[31:4] | Z[3:0] (Z is 4 bits in lower nibble)
/// - AR (Table 8-1): branchWord = branchAddress[31:4] | reserved[3:1] | Z[0] (Z is 1 bit in LSB)
///
/// Linux Reference: drivers/firewire/ohci.c:747 — d->branch_address |= cpu_to_le32(1)
///
/// @param physAddr 16-byte aligned physical address (bits [3:0] must be zero)
/// @param continueFlag true if more descriptors follow (Z=1), false if last descriptor (Z=0)
/// @return Packed 32-bit branchWord = branchAddress[31:4] | Z[0], or 0 if invalid
[[nodiscard]] constexpr uint32_t MakeBranchWordAR(uint64_t physAddr, bool continueFlag) noexcept {
    // Validate per OHCI Table 8-1: "16-byte aligned address"
    if ((physAddr & 0xFULL) != 0 || physAddr > 0xFFFFFFFFu) {
        return 0;
    }
    // For AR: Z is a single bit in position [0]
    // branchWord format: physAddr[31:4] (upper 28 bits) | reserved[3:1] | Z[0]
    const uint32_t Z = continueFlag ? 1u : 0u;
    return (static_cast<uint32_t>(physAddr) & 0xFFFFFFF0u) | Z;
}

/// Decodes the next descriptor physical address from an AT (Asynchronous Transmit) branchWord.
///
/// Spec: OHCI Table 7-3: branchWord = physAddr[31:4] | Z[3:0]
///       Simply mask out Z bits to recover physical address
///
/// @param branchWord The AT descriptor's branchWord field (host byte order)
/// @return 32-bit physical address of next descriptor, or 0 if branchWord indicates end-of-chain
[[nodiscard]] constexpr uint32_t DecodeBranchPhys32_AT(uint32_t branchWord) noexcept {
    return branchWord & 0xFFFFFFF0u;  // Mask out Z[3:0], leaving physAddr[31:4]
}

/// Decodes the next descriptor physical address from an AR (Asynchronous Receive) branchWord.
///
/// Spec: OHCI Table 8-1: branchAddress = bits[31:4], Z = bit[0]
///       Address format: physAddr[31:4] | reserved[3:1] | Z[0] (not shifted)
///
/// @param branchWord The AR descriptor's branchWord field (host byte order)
/// @return 32-bit physical address of next descriptor, or 0 if branchWord indicates end-of-chain
[[nodiscard]] constexpr uint32_t DecodeBranchPhys32_AR(uint32_t branchWord) noexcept {
    return branchWord & 0xFFFFFFF0u;  // Mask out Z[0] and reserved[3:1], leaving physAddr[31:4]
}


/// Standard 16-byte OHCI Asynchronous Transmit DMA descriptor.
///
/// Spec: OHCI §7.1.1 "OUTPUT_MORE descriptor" (Figure 7-1, Table 7-1)
///       OHCI §7.1.3 "OUTPUT_LAST descriptor" (Figure 7-3, Table 7-3)
///
/// Memory Layout (16 bytes, 4 quadlets). Each quadlet is exposed both as a 32-bit word and,
/// where convenient, as structured aliases for individual fields. No padding is permitted.
///
/// ALIGNMENT: MUST be 16-byte aligned (OHCI §7.1, Table 7-3)
/// ENDIANNESS: Fields stored in HOST byte order (little-endian on macOS x86/ARM)
struct alignas(16) OHCIDescriptor {
    // Quadlet 0: Control word (packed bitfields per OHCI Table 7-1/7-3)
    union {
        uint32_t control{0};        ///< cmd[31:28] | key[27:25] | p[24] | i[23:22] | b[21:20] | reserved[19:16] | reqCount[15:0]
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        struct {
            uint16_t reqCount;      ///< Lower 16 bits of control word (host order)
            uint16_t controlUpper;  ///< Upper control bits (cmd/key/p/i/b)
        };
#else
        struct {
            uint16_t controlUpper;
            uint16_t reqCount;
        };
#endif
    };

    // Quadlet 1: Data address (Table 7-1/7-3: "dataAddress has no alignment restrictions")
    uint32_t dataAddress{0};        ///< Physical address of transmit data buffer

    // Quadlet 2: Branch word (Z + 16-byte aligned next descriptor address)
    uint32_t branchWord{0};         ///< AT: physAddr[31:4] | Z[3:0] per OHCI Table 7-3

    // Quadlet 3: Status written by hardware — INTERPRETATION DEPENDS ON CONTEXT!
    // - AT (OUTPUT): Host byte order [xferStatus:16][timeStamp:16] (OHCI §7.1.5.2, §7.1.5.3)
    // - AR (INPUT):  Host byte order [xferStatus:16][resCount:16] (OHCI §8.4.2, Table 8-1)
    // USE ACCESSORS: AT_xferStatus/AT_timeStamp for AT, AR_xferStatus/AR_resCount for AR
    union {
        uint32_t statusWord{0};     ///< Full 32-bit status written by hardware (see context-specific accessors)
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        struct {
            uint16_t xferStatus;    ///< AT ONLY: ContextControl[15:0] after completion (host order)
            uint16_t timeStamp;     ///< AT ONLY: cycleSeconds[15:13] | cycleCount[12:0] (host order)
        };
#else
        struct {
            uint16_t timeStamp;
            uint16_t xferStatus;
        };
#endif
        uint32_t softwareTag;       ///< Software-only overlay (e.g., slot handle before submission)
    };

    // Control word is composed of a 16-bit "control hi" field (bits[31:16]) and reqCount (bits[15:0])
    static constexpr uint32_t kControlHighShift = 16;

    // Bitfield shifts WITHIN the 16-bit control hi field (OHCI 1.2 positions)
    //
    // CRITICAL: These positions match OHCI 1.2 draft (not OHCI 1.1 spec!).
    // Validated against:
    //   1. Linux firewire-ohci driver (drivers/firewire/ohci.c lines 56-68)
    //   2. Apple's AppleFWOHCI kext (IDA decompilation, control word 0x123C000C)
    //
    // OHCI 1.2 moved several fields compared to OHCI 1.1:
    //   - key field: bits[10:8] (was [11:9] in OHCI 1.1)
    //   - ping bit: bit[7] (was bit[8] in OHCI 1.1)
    //   - interrupt: bits[5:4] (was [7:6] in OHCI 1.1)
    //   - branch: bits[3:2]
    //
    // These positions produce Apple's exact control word 0x123C000C:
    //   (1<<12) | (2<<8) | (3<<4) | (3<<2) = 0x123C
    //
    // Verified by Python analysis tool working backwards from Apple's binary.
    static constexpr uint32_t kCmdShift = 12;      ///< cmd field: bits[15:12]
    static constexpr uint32_t kStatusShift = 11;   ///< STATUS bit: bit[11] (reserved/unused)
    static constexpr uint32_t kKeyShift = 8;       ///< key field: bits[10:8] (3 bits)
    static constexpr uint32_t kPingShift = 7;      ///< p (ping) bit: bit[7] (unused in our context)
    static constexpr uint32_t kYYShift = 6;        ///< YY bit: bit[6] (unused)
    static constexpr uint32_t kIntShift = 4;       ///< i (interrupt): bits[5:4] (2 bits)
    static constexpr uint32_t kBranchShift = 2;    ///< b (branch): bits[3:2] (2 bits)
    static constexpr uint32_t kWaitShift = 0;      ///< wait field: bits[1:0]

    static constexpr uint32_t kZShift = 28;        ///< Z field in branchWord: bits[31:28] (Table 7-3)

    // Command values (OHCI Table 7-1, 7-3, 8-1)
    static constexpr uint8_t kCmdOutputMore = 0x0;  ///< OUTPUT_MORE: cmd=0x0 (Table 7-1)
    static constexpr uint8_t kCmdOutputLast = 0x1;  ///< OUTPUT_LAST: cmd=0x1 (Table 7-3)
    static constexpr uint8_t kCmdInputMore  = 0x2;  ///< INPUT_MORE: cmd=0x2 (Table 8-1)

    // Key values (OHCI Tables 7-1, 7-2, 7-3, 7-4)
    static constexpr uint8_t kKeyStandard = 0x0;    ///< Standard descriptor: key=0x0 (Table 7-1, 7-3)
    static constexpr uint8_t kKeyImmediate = 0x2;   ///< Immediate descriptor: key=0x2 (Table 7-2, 7-4)

    // Interrupt control values (OHCI Table 7-3)
    static constexpr uint8_t kIntNever = 0b00;      ///< i=00: Never interrupt
    static constexpr uint8_t kIntOnError = 0b01;    ///< i=01: Interrupt if NOT ack_complete/ack_pending
    static constexpr uint8_t kIntAlways = 0b11;     ///< i=11: Always interrupt on completion

    // Branch control values (OHCI Table 7-1, 7-3)
    static constexpr uint8_t kBranchNever = 0b00;   ///< b=00: No branch (OUTPUT_MORE, Table 7-1)
    static constexpr uint8_t kBranchAlways = 0b11;  ///< b=11: Always branch (OUTPUT_LAST, Table 7-3)
    
    // ========================================================================
    // Control Word Construction - Single Source of Truth
    // Matches Apple's 0x123C0000 pattern (DecompilationAnalysis.md Line 87)
    // Per OHCI 1.2 draft spec (Apple implementation)
    // ========================================================================
    
    /// Build complete OHCI 1.2 control word
    /// @param reqCount Request count field [15:0]
    /// @param cmd Command field [31:28]: 0=OUTPUT_MORE, 1=OUTPUT_LAST, 3=OUTPUT_LAST_Immediate
    /// @param key Key field [26:24]: 0=standard, 2=immediate, 4=Apple extension
    /// @param i Interrupt field [23:22]: 0=never, 1=on_error(<8), 2=on_error(>=8), 3=always
    /// @param b Branch field [21:20]: 0-2=reserved, 3=always
    /// @param ping Ping bit [24]: true for ping packets
    /// @return Complete control word ready for descriptor
    [[nodiscard]] static constexpr uint32_t BuildControl(
        uint16_t reqCount,
        uint8_t cmd,
        uint8_t key,
        uint8_t i,
        uint8_t b,
        bool ping = false
    ) noexcept {
        // Mask inputs per OHCI 1.2 field widths
        const uint8_t cmd_masked = cmd & 0xF;
        const uint8_t key_masked = key & 0x7;
        const uint8_t i_masked = i & 0x3;
        const uint8_t b_masked = b & 0x3;
        
        const uint32_t high =
            (static_cast<uint32_t>(cmd_masked) << kCmdShift) |
            (static_cast<uint32_t>(key_masked) << kKeyShift) |
            (static_cast<uint32_t>(i_masked) << kIntShift) |
            (static_cast<uint32_t>(b_masked) << kBranchShift) |
            (ping ? (1u << kPingShift) : 0);
        
        return ((high & 0xFFFFu) << kControlHighShift) | (reqCount & 0xFFFFu);
    }
    
    /// Atomically patch branch field in existing control word
    /// Preserves cmd/key/i/ping fields, only modifies b field
    /// Used when linking descriptors in append path (Path 2)
    /// @param desc Descriptor to modify
    /// @param b New branch value (0-3)
    static inline void PatchBranch(OHCIDescriptor& desc, uint8_t b) noexcept {
        const uint32_t mask = 0x3u << (kBranchShift + kControlHighShift);
        const uint32_t val = (b & 0x3u) << (kBranchShift + kControlHighShift);
        desc.control = (desc.control & ~mask) | val;
    }
    
    /// Clear branch control bits (set b=0) for end-of-list descriptors
    /// CRITICAL: EOL descriptors with branchWord=0 MUST have b=0
    /// Setting b=BranchAlways on EOL leaves context in state that won't resume on WAKE
    /// @param desc Descriptor to modify
    static inline void ClearBranchBits(OHCIDescriptor& desc) noexcept {
        const uint32_t mask = 0x3u << (kBranchShift + kControlHighShift);
        desc.control = desc.control & ~mask;
    }
};
static_assert(sizeof(OHCIDescriptor) == 16, "OHCIDescriptor must be 16 bytes per OHCI §7.1");

//
// Safe Accessors for AR vs AT Descriptor Status Interpretation
//

/// AR (Asynchronous Receive) statusWord accessors — HOST byte order: [xferStatus:16][resCount:16]
/// Spec: OHCI §8.4.2, Table 8-1 — Hardware writes in PCI native byte order (little-endian on x86/ARM)
///
/// CRITICAL: Like AT descriptors, AR descriptors are in host memory and use NATIVE byte order.
/// Hardware writes statusWord in native format with xferStatus in upper 16 bits, resCount in lower 16 bits.

/// Extract xferStatus from AR descriptor (contains ACTIVE bit and event codes)
[[nodiscard]] inline uint16_t AR_xferStatus(const OHCIDescriptor& d) noexcept {
    return static_cast<uint16_t>(d.statusWord >> 16);  // Already in host byte order
}

/// Extract resCount from AR descriptor (bytes remaining/written in buffer)
[[nodiscard]] inline uint16_t AR_resCount(const OHCIDescriptor& d) noexcept {
    return static_cast<uint16_t>(d.statusWord & 0xFFFF);  // Already in host byte order
}

/// Initialize AR descriptor status for recycling (set resCount=reqCount, clear xferStatus)
/// @param d Descriptor to initialize
/// @param reqCount_host Buffer size in bytes (host byte order)
inline void AR_init_status(OHCIDescriptor& d, uint16_t reqCount_host) noexcept {
    // statusWord in native byte order: [xferStatus=0:16][resCount=reqCount:16]
    d.statusWord = (0x0000u << 16) | reqCount_host;
}

/// AT (Asynchronous Transmit) statusWord accessors — HOST byte order: [xferStatus:16][timeStamp:16]
/// Spec: OHCI §7.1.5.2, §7.1.5.3 — Hardware writes in PCI native byte order

/// Extract xferStatus from AT descriptor (ack code and event status)
[[nodiscard]] inline uint16_t AT_xferStatus(const OHCIDescriptor& d) noexcept {
    return d.xferStatus;  // Already in host byte order
}

/// Extract timeStamp from AT descriptor (cycle timer snapshot)
[[nodiscard]] inline uint16_t AT_timeStamp(const OHCIDescriptor& d) noexcept {
    return d.timeStamp;  // Already in host byte order
}

/// Check if a descriptor is an immediate descriptor (key=Immediate, i.e., key=0x2)
/// Spec: OHCI Table 7-2/7-4: Immediate descriptors have key=0x2
/// @param d Descriptor to check
/// @return true if descriptor is immediate (OUTPUT_MORE_Immediate or OUTPUT_LAST_Immediate), false otherwise
[[nodiscard]] inline bool IsImmediate(const OHCIDescriptor& d) noexcept {
    const uint32_t controlHi = d.control >> OHCIDescriptor::kControlHighShift;
    const uint8_t keyField = (controlHi >> OHCIDescriptor::kKeyShift) & 0x7;
    return keyField == OHCIDescriptor::kKeyImmediate;
}

/// 32-byte OHCI Immediate descriptor (OUTPUT_MORE_Immediate or OUTPUT_LAST_Immediate).
///
/// Spec: OHCI §7.1.2 "OUTPUT_MORE_Immediate descriptor" (Figure 7-2, Table 7-2)
///       OHCI §7.1.4 "OUTPUT_LAST_Immediate descriptor" (Figure 7-4, Table 7-4)
///
/// Memory Layout (32 bytes):
///   Bytes [0-15]:   Standard OHCIDescriptor structure
///   Bytes [16-31]:  immediateData[4] — inline packet header data (16 bytes = 4 quadlets)
///
/// USAGE: Per OHCI Table 7-5, ALL descriptor blocks MUST start with an *-Immediate descriptor.
///        The immediateData field contains the 1394 packet header in BIG-ENDIAN format.
///
/// ALIGNMENT: MUST be 16-byte aligned (OHCI §7.1)
/// SIZE: Counted as TWO 16-byte blocks when calculating Z values (Table 7-5)
struct alignas(16) OHCIDescriptorImmediate {
    OHCIDescriptor common;      ///< Standard descriptor fields (control, dataAddress, branchWord, etc.)
    uint32_t immediateData[4]{};  ///< 16 bytes of inline data (1394 packet header in BIG-ENDIAN format)
};
static_assert(sizeof(OHCIDescriptorImmediate) == 32, "OHCIDescriptorImmediate must be 32 bytes per OHCI §7.1.2/7.1.4");

/// Extracts tLabel from an OUTPUT_LAST_Immediate descriptor's packet header.
///
/// IEEE 1394 Packet Format (per IEEE 1394-1995 §6.2, OHCI Figures 7-9 to 7-14):
///   Quadlet 0 (big-endian): [destination_ID:16][tLabel:6][rt:2][tCode:4][pri:4]
///   Example: 0xFFC00140 → destID=0xFFC0, tLabel=0, rt=1, tCode=4, pri=0
///
/// CRITICAL: tLabel is at bits[15:10] of IEEE 1394 wire format, NOT bits[23:18]!
///
/// @param immDesc Pointer to OUTPUT_LAST_Immediate descriptor with immediateData[0] containing control quadlet
/// @return tLabel value (6 bits, range 0-63) or 0xFF if descriptor is invalid
[[nodiscard]] inline uint8_t ExtractTLabel(const OHCIDescriptorImmediate* immDesc) noexcept {
    if (!immDesc) {
        return 0xFF;  // Invalid descriptor
    }

    // CRITICAL: immediateData[0] contains OHCI INTERNAL format in HOST byte order
    // (NOT IEEE 1394 wire format - that conversion happens in hardware)
    // PacketBuilder writes this in native byte order per Linux firewire-ohci driver
    const uint32_t controlHost = immDesc->immediateData[0];

    // OHCI internal format: tLabel is bits[15:10] of quadlet 0
    // Per Linux firewire-ohci driver and OHCI spec §7.8
    const uint8_t tLabel = static_cast<uint8_t>((controlHost >> 10) & 0x3F);

    return tLabel;
}

/// Build IEEE 1394 wire format Quadlet 0 for async request
/// Uses constants from Core/OHCIConstants.hpp (single source of truth)
/// @param destID 16-bit destination node ID
/// @param tLabel 6-bit transaction label (0-63)
/// @param retry 2-bit retry code (typically kIEEE1394_RetryX = 0x1)
/// @param tCode 4-bit transaction code (e.g., kIEEE1394_TCodeReadQuadRequest = 0x4)
/// @param priority 4-bit priority (typically kIEEE1394_PriorityDefault = 0x0)
/// @return Quadlet 0 in HOST byte order (convert to big-endian before storing)
[[nodiscard]] inline constexpr uint32_t BuildIEEE1394Quadlet0(
    uint16_t destID,
    uint8_t tLabel,
    uint8_t retry,
    uint8_t tCode,
    uint8_t priority
) noexcept {
    return (static_cast<uint32_t>(destID & 0xFFFF) << Driver::kIEEE1394_DestinationIDShift) |
           (static_cast<uint32_t>(tLabel & 0x3F) << Driver::kIEEE1394_TLabelShift) |
           (static_cast<uint32_t>(retry & 0x03) << Driver::kIEEE1394_RetryShift) |
           (static_cast<uint32_t>(tCode & 0x0F) << Driver::kIEEE1394_TCodeShift) |
           (static_cast<uint32_t>(priority & 0x0F) << Driver::kIEEE1394_PriorityShift);
}

/// Build IEEE 1394 wire format Quadlet 1 for async request
/// Uses constants from Core/OHCIConstants.hpp (single source of truth)
/// @param sourceID 16-bit source node ID
/// @param offsetHigh High 16 bits of 48-bit destination offset
/// @return Quadlet 1 in HOST byte order (convert to big-endian before storing)
[[nodiscard]] inline constexpr uint32_t BuildIEEE1394Quadlet1(
    uint16_t sourceID,
    uint16_t offsetHigh
) noexcept {
    return (static_cast<uint32_t>(sourceID & 0xFFFF) << Driver::kIEEE1394_SourceIDShift) |
           (static_cast<uint32_t>(offsetHigh & 0xFFFF) << Driver::kIEEE1394_OffsetHighShift);
}

/// Build IEEE 1394 wire format Quadlet 3 for block request
/// Uses constants from Core/OHCIConstants.hpp (single source of truth)
/// @param dataLength Payload length in bytes
/// @param extendedTCode Extended transaction code (for lock requests)
/// @return Quadlet 3 in HOST byte order (convert to big-endian before storing)
[[nodiscard]] inline constexpr uint32_t BuildIEEE1394Quadlet3Block(
    uint16_t dataLength,
    uint16_t extendedTCode = 0
) noexcept {
    return (static_cast<uint32_t>(dataLength & 0xFFFF) << Driver::kIEEE1394_DataLengthShift) |
           (static_cast<uint32_t>(extendedTCode & 0xFFFF) << Driver::kIEEE1394_ExtendedTCodeShift);
}

/// IEEE 1394 Asynchronous Request Packet Header (software representation).
///
/// This structure is used to BUILD transmit packet headers. Fields are populated in HOST byte order,
/// then converted to BIG-ENDIAN using ToBigEndian16/32() before copying into immediateData[].
///
/// Spec: OHCI §7.8.1 "Async Packet Formats"
///       - Figure 7-9: Quadlet read request (12 bytes)
///       - Figure 7-10: Quadlet write request (16 bytes)
///       - Figure 7-11: Block read request (16 bytes)
///       - Figure 7-12: Block write request (16 bytes)
///       - Figure 7-13: Lock request (16 bytes)
///       - Figure 7-14: PHY packet (4 bytes header + 8 bytes payload)
///
/// IEEE 1394-1995 §6.2: All multi-byte fields transmitted MSB first (big-endian)
struct AsyncRequestHeader {
    uint32_t control{0};                  ///< Bits[31:0]: srcBusID, speed, tLabel, rt, tCode, pri (Figures 7-9 to 7-14)
    uint16_t destinationID{0};            ///< destination_ID (IEEE 1394-1995 §6.2.4.1)
    uint16_t destinationOffsetHigh{0};    ///< destination_offset[47:32]
    uint32_t destinationOffsetLow{0};     ///< destination_offset[31:0]

    union {
        uint32_t quadletData;             ///< For quadlet write (Figure 7-10)
        uint16_t dataLength;              ///< For block read/write/lock (Figures 7-11, 7-12, 7-13)
        uint16_t extendedTCode;           ///< For lock requests (Figure 7-13)
    } payload_info{};

    // Control word bitfield offsets (CORRECTED - see Issue #1)
    // Actual AT descriptor immediateData[0] format (host byte order):
    // bits[31:16] = destination_ID
    // bits[15:10] = tLabel
    // bits[9:8]   = retry
    // bits[7:4]   = tCode
    // bits[3:0]   = priority
    //
    // NOTE: Original OHCI spec figures show srcBusID/spd fields, but actual
    // implementation uses destination_ID at bits[31:16]. Hardware converts
    // this format to IEEE 1394 wire format before transmission.
    static constexpr uint32_t kLabelShift = 10;     ///< tl (tLabel) bits[15:10] (FIXED from 18)
    static constexpr uint32_t kRetryShift = 8;      ///< rt (retry code) bits[9:8]
    static constexpr uint32_t kTcodeShift = 4;      ///< tCode bits[7:4]

    // DEPRECATED: These fields don't exist in actual packet format
    // static constexpr uint32_t kSrcBusIDShift = 31;  // NOT USED
    // static constexpr uint32_t kSpdShift = 24;       // NOT USED

    // IEEE 1394-1995 tCode values (OHCI Figures 7-9 to 7-14)
    static constexpr uint8_t kTcodeWriteQuad = 0x0;    ///< Quadlet write (Figure 7-10)
    static constexpr uint8_t kTcodeWriteBlock = 0x1;   ///< Block write (Figure 7-12)
    static constexpr uint8_t kTcodeReadQuad = 0x4;     ///< Quadlet read (Figure 7-9)
    static constexpr uint8_t kTcodeReadBlock = 0x5;    ///< Block read (Figure 7-11)
    static constexpr uint8_t kTcodeLockRequest = 0x9;  ///< Lock request (Figure 7-13)
    static constexpr uint8_t kTcodeStreamData = 0xA;   ///< Async stream (Figure 7-19)
    static constexpr uint8_t kTcodePhyPacket = 0xE;    ///< PHY packet (Figure 7-14)
};

/// IEEE 1394 Asynchronous Receive Packet Header (as written by OHCI hardware).
///
/// This structure represents packet headers as they appear in Asynchronous Receive (AR) DMA buffers.
/// The OHCI controller writes these in BIG-ENDIAN format per IEEE 1394 wire format.
///
/// Spec: OHCI §8.7 "AR Packet Formats"
///       - Figure 8-7: Quadlet read request receive format
///       - Figure 8-8: Quadlet write request receive format
///       - And similar for responses
///
/// SIZE: 12 bytes minimum (quadlet packets), 16 bytes for block packets
/// ENDIANNESS: BIG-ENDIAN (use OSSwapBigToHostIntXX from DriverKit/IOLib.h to read)
struct __attribute__((packed)) AsyncReceiveHeader {
    uint16_t destinationID{0};       ///< destination_ID (big-endian)
    uint8_t  tl_tcode_rt{0};         ///< Packed byte: tLabel[7:2], tCode[1:0] high bits
    uint8_t  headerControl{0};       ///< Packed byte: tCode[3:2] low bits, rt[7:6], pri[3:0]

    uint16_t sourceID{0};            ///< source_ID (big-endian)
    uint16_t destinationOffsetHigh{0}; ///< destination_offset[47:32] (big-endian)

    uint32_t destinationOffsetLow{0};  ///< destination_offset[31:0] (big-endian)

    // Bit extraction masks for tl_tcode_rt byte (OHCI Figure 8-7)
    static constexpr uint8_t kTLabelMask = 0xFC;      ///< tLabel = bits[7:2] of tl_tcode_rt
    static constexpr uint8_t kTLabelShift = 2;        ///< Extract: (tl_tcode_rt & 0xFC) >> 2
    static constexpr uint8_t kTCodeMask = 0x0F;       ///< tCode = bits[3:0] (split across 2 bytes)
    static constexpr uint8_t kRetryShift = 6;         ///< rt = bits[7:6] of headerControl
};
static_assert(sizeof(AsyncReceiveHeader) == 12, "AsyncReceiveHeader must be 12 bytes per OHCI §8.7");

/// AR DMA Packet Trailer appended by OHCI hardware to every received packet.
///
/// Spec: OHCI §8.4.2.1 "AR DMA Packet Trailer" (Figure 8-5)
///
/// The OHCI controller appends this 4-byte trailer to the end of EVERY packet written to an
/// AR context buffer. The trailer provides completion status and a timestamp.
///
/// LOCATION: Last 4 bytes of each packet in AR buffer
/// ENDIANNESS: Mixed — xferStatus is little-endian (host order), timeStamp is big-endian (wire order)
struct __attribute__((packed)) ARPacketTrailer {
    uint16_t timeStamp{0};    ///< Cycle timer snapshot (big-endian): cycleSeconds[15:13] | cycleCount[12:0]
    uint16_t xferStatus{0};   ///< ContextControl[15:0] at completion (host order): contains evt code in bits[4:0]
};
static_assert(sizeof(ARPacketTrailer) == 4, "ARPacketTrailer must be 4 bytes per OHCI Figure 8-5");

} // namespace ASFW::Async::HW

