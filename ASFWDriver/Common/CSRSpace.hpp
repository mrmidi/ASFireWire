// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// CSRSpace.hpp — CSR address space: register constants, Config ROM structure, bus options
//
// Reference: IEEE 1394-1995 §8.3.2, IEEE 1212-2001, TA 1999027

#pragma once

#include "FWTypes.hpp"
#include <cstdint>
#include <cstdio>
#include <string>

// Forward declaration - FWAddress struct is defined in AsyncTypes.hpp
namespace ASFW::Async {
struct FWAddress;
}

namespace ASFW::FW {

// ============================================================================
// CSR Address Constants (SINGLE SOURCE)
// ============================================================================

// CSR Register Space Base Addresses (IEEE 1394-1995 §8.3.2)
inline constexpr uint16_t kCSRRegSpaceHi = 0x0000FFFFu;
inline constexpr uint32_t kCSRRegSpaceLo = 0xF0000000u;
inline constexpr uint32_t kCSRCoreBase = kCSRRegSpaceLo;

// Core CSR Registers (IEEE 1394-1995 §8.3.2.1)
inline constexpr uint32_t kCSR_NodeIDs         = kCSRCoreBase + 0x0008;
inline constexpr uint32_t kCSR_StateSet        = kCSRCoreBase + 0x0004;
inline constexpr uint32_t kCSR_StateClear      = kCSRCoreBase + 0x0000;
inline constexpr uint32_t kCSR_IndirectAddress = kCSRCoreBase + 0x0010;
inline constexpr uint32_t kCSR_IndirectData    = kCSRCoreBase + 0x0014;
inline constexpr uint32_t kCSR_SplitTimeoutHi  = kCSRCoreBase + 0x0018;
inline constexpr uint32_t kCSR_SplitTimeoutLo  = kCSRCoreBase + 0x001C;

// Config ROM Base Address (IEEE 1394-1995 §8.3.2.2)
// Low 32b offset within CSR register space (0xF0000400)
// Effective 64-bit CSR address is (nodeID<<48 | 0xFFFF<<32 | 0xF0000400)
inline constexpr uint32_t kCSR_ConfigROMBase     = kCSRRegSpaceLo + 0x0400;
inline constexpr uint32_t kCSR_ConfigROMBIBHeader = kCSR_ConfigROMBase + 0x00;
inline constexpr uint32_t kCSR_ConfigROMBIBBusName = kCSR_ConfigROMBase + 0x04;

// Legacy aliases for DiscoveryValues.hpp compatibility
namespace ConfigROMAddr {
inline constexpr uint16_t kAddressHi       = kCSRRegSpaceHi;
inline constexpr uint32_t kAddressLo       = kCSR_ConfigROMBase;
inline constexpr uint32_t kBIBHeaderOffset = 0x00;
inline constexpr uint32_t kBIBBusNameOffset = 0x04;
} // namespace ConfigROMAddr

/**
 * Build a 64-bit CSR address for (nodeID, offset).
 * Format: bits[63:48] = nodeID, bits[47:32] = kCSRRegSpaceHi, bits[31:0] = offset
 */
inline constexpr uint64_t CSRAddr(uint16_t nodeID, uint32_t csrOffset) {
    return (uint64_t(nodeID) << 48) | (uint64_t(kCSRRegSpaceHi) << 32) | uint64_t(csrOffset);
}

/**
 * Build a 64-bit Config ROM word address for (nodeID, byteOffset).
 * Convenience helper for Config ROM reads.
 */
inline constexpr uint64_t ConfigROMWord(uint16_t nodeID, uint32_t byteOffset) {
    return CSRAddr(nodeID, kCSR_ConfigROMBase + byteOffset);
}

/**
 * Format CSR address as string for logging (e.g., "0xffff:f0000400").
 */
inline std::string CSRAddrToString(uint64_t addr) {
    char buf[64];
    uint16_t nodeID = static_cast<uint16_t>((addr >> 48) & 0xFFFFu);
    uint16_t hi = static_cast<uint16_t>((addr >> 32) & 0xFFFFu);
    uint32_t lo = static_cast<uint32_t>(addr & 0xFFFFFFFFu);
    std::snprintf(buf, sizeof(buf), "0x%04x:%08x (node=0x%04x)", hi, lo, nodeID);
    return std::string(buf);
}

// ============================================================================
// Config ROM Keys (SINGLE SOURCE)
// ============================================================================

/**
 * Config ROM directory entry types (IEEE 1394-1995 §8.3.2.3).
 * These are the top 2 bits of the key byte in directory entries.
 */
namespace EntryType {
inline constexpr uint8_t kImmediate = 0; // Value is immediate data
inline constexpr uint8_t kCSROffset = 1; // Value is CSR address offset
inline constexpr uint8_t kLeaf     = 2;  // Value is offset to leaf structure
inline constexpr uint8_t kDirectory = 3; // Value is offset to subdirectory
} // namespace EntryType

/**
 * Config ROM directory keys (IEEE 1394-1995 §8.3.2.3).
 * These are the key values in directory entries.
 */
namespace ConfigKey {
inline constexpr uint8_t kTextualDescriptor  = 0x01;
inline constexpr uint8_t kBusDependentInfo   = 0x02;
inline constexpr uint8_t kModuleVendorId     = 0x03;
inline constexpr uint8_t kModuleHwVersion    = 0x04;
inline constexpr uint8_t kModuleSpecId       = 0x05;
inline constexpr uint8_t kModuleSwVersion    = 0x06;
inline constexpr uint8_t kModuleDependentInfo = 0x07;
inline constexpr uint8_t kNodeVendorId       = 0x08;
inline constexpr uint8_t kNodeHwVersion      = 0x09;
inline constexpr uint8_t kNodeSpecId         = 0x0A;
inline constexpr uint8_t kNodeSwVersion      = 0x0B;
inline constexpr uint8_t kNodeCapabilities   = 0x0C;
inline constexpr uint8_t kNodeUniqueId       = 0x0D;
inline constexpr uint8_t kNodeUnitsExtent    = 0x0E;
inline constexpr uint8_t kNodeMemoryExtent   = 0x0F;
inline constexpr uint8_t kNodeDependentInfo  = 0x10;
inline constexpr uint8_t kUnitDirectory      = 0x11;
inline constexpr uint8_t kUnitSpecId         = 0x12;
inline constexpr uint8_t kUnitSwVersion      = 0x13;
inline constexpr uint8_t kUnitDependentInfo  = 0x14;
inline constexpr uint8_t kUnitLocation       = 0x15;
inline constexpr uint8_t kUnitPollMask       = 0x16;
inline constexpr uint8_t kModelId            = 0x17;
inline constexpr uint8_t kGeneration         = 0x38; // Apple-specific
} // namespace ConfigKey

// ============================================================================
// Config ROM Header + Bus Info Block (IEEE 1212 + TA 1999027)
// ============================================================================

/**
 * Config ROM quadlet 0 (header) field masks.
 *
 * Layout (host numeric after BE->host swap):
 *   [31:24] bus_info_length  (quadlets following header in BIB)
 *   [23:16] crc_length       (quadlets covered by CRC, starting at quadlet 1)
 *   [15:0]  crc              (CRC-16 of quadlets 1..crc_length)
 */
namespace ConfigROMHeaderFields {
inline constexpr uint32_t kBusInfoLengthShift = 24;
inline constexpr uint32_t kBusInfoLengthMask  = 0xFF000000u;

inline constexpr uint32_t kCRCLengthShift = 16;
inline constexpr uint32_t kCRCLengthMask  = 0x00FF0000u;

inline constexpr uint32_t kCRCMask = 0x0000FFFFu;
} // namespace ConfigROMHeaderFields

/**
 * Bus options quadlet (BIB quadlet 2) field masks.
 *
 * This matches TA 1999027 Annex C sample bus options bytes: E0 64 61 02 (0xE0646102).
 *
 * Layout (host numeric after BE->host swap):
 *   [31]    irmc
 *   [30]    cmc
 *   [29]    isc
 *   [28]    bmc
 *   [27]    pmc
 *   [23:16] cyc_clk_acc
 *   [15:12] max_rec
 *   [11:10] reserved
 *   [9:8]   max_ROM
 *   [7:4]   generation
 *   [3]     reserved
 *   [2:0]   link_spd
 */
namespace BusOptionsFields {
// Capability bits (MSB side)
inline constexpr uint32_t kIRMCMask = 0x80000000u;
inline constexpr uint32_t kCMCMask  = 0x40000000u;
inline constexpr uint32_t kISCMask  = 0x20000000u;
inline constexpr uint32_t kBMCMask  = 0x10000000u;
inline constexpr uint32_t kPMCMask  = 0x08000000u;

// CycClkAcc (8-bit)
inline constexpr uint32_t kCycClkAccShift = 16;
inline constexpr uint32_t kCycClkAccMask  = 0x00FF0000u;

// MaxRec (4-bit)
inline constexpr uint32_t kMaxRecShift = 12;
inline constexpr uint32_t kMaxRecMask  = 0x0000F000u;

// Reserved [11:10]
inline constexpr uint32_t kReserved11_10Mask = 0x00000C00u;

// MaxROM (2-bit)
inline constexpr uint32_t kMaxROMShift = 8;
inline constexpr uint32_t kMaxROMMask  = 0x00000300u;

// Generation (4-bit)
inline constexpr uint32_t kGenerationShift = 4;
inline constexpr uint32_t kGenerationMask  = 0x000000F0u;

// Reserved [3]
inline constexpr uint32_t kReserved3Mask = 0x00000008u;

// Link speed code (3-bit)
inline constexpr uint32_t kLinkSpdShift = 0;
inline constexpr uint32_t kLinkSpdMask  = 0x00000007u;
} // namespace BusOptionsFields

struct BusOptionsDecoded {
    bool irmc{false};
    bool cmc{false};
    bool isc{false};
    bool bmc{false};
    bool pmc{false};

    uint8_t cycClkAcc{0};
    uint8_t maxRec{0};
    uint8_t maxRom{0};
    uint8_t generation{0};
    uint8_t linkSpd{0};
};

[[nodiscard]] constexpr BusOptionsDecoded DecodeBusOptions(uint32_t busOptionsHost) noexcept {
    BusOptionsDecoded out{};
    out.irmc = (busOptionsHost & BusOptionsFields::kIRMCMask) != 0;
    out.cmc  = (busOptionsHost & BusOptionsFields::kCMCMask)  != 0;
    out.isc  = (busOptionsHost & BusOptionsFields::kISCMask)  != 0;
    out.bmc  = (busOptionsHost & BusOptionsFields::kBMCMask)  != 0;
    out.pmc  = (busOptionsHost & BusOptionsFields::kPMCMask)  != 0;

    out.cycClkAcc = static_cast<uint8_t>((busOptionsHost & BusOptionsFields::kCycClkAccMask) >>
                                         BusOptionsFields::kCycClkAccShift);
    out.maxRec = static_cast<uint8_t>((busOptionsHost & BusOptionsFields::kMaxRecMask) >>
                                      BusOptionsFields::kMaxRecShift);
    out.maxRom = static_cast<uint8_t>((busOptionsHost & BusOptionsFields::kMaxROMMask) >>
                                      BusOptionsFields::kMaxROMShift);
    out.generation = static_cast<uint8_t>((busOptionsHost & BusOptionsFields::kGenerationMask) >>
                                          BusOptionsFields::kGenerationShift);
    out.linkSpd = static_cast<uint8_t>((busOptionsHost & BusOptionsFields::kLinkSpdMask) >>
                                       BusOptionsFields::kLinkSpdShift);
    return out;
}

[[nodiscard]] constexpr uint32_t EncodeBusOptions(const BusOptionsDecoded& in) noexcept {
    uint32_t out = 0;
    if (in.irmc) out |= BusOptionsFields::kIRMCMask;
    if (in.cmc)  out |= BusOptionsFields::kCMCMask;
    if (in.isc)  out |= BusOptionsFields::kISCMask;
    if (in.bmc)  out |= BusOptionsFields::kBMCMask;
    if (in.pmc)  out |= BusOptionsFields::kPMCMask;

    out |= (static_cast<uint32_t>(in.cycClkAcc) << BusOptionsFields::kCycClkAccShift) &
           BusOptionsFields::kCycClkAccMask;
    out |= (static_cast<uint32_t>(in.maxRec) << BusOptionsFields::kMaxRecShift) &
           BusOptionsFields::kMaxRecMask;
    out |= (static_cast<uint32_t>(in.maxRom) << BusOptionsFields::kMaxROMShift) &
           BusOptionsFields::kMaxROMMask;
    out |= (static_cast<uint32_t>(in.generation) << BusOptionsFields::kGenerationShift) &
           BusOptionsFields::kGenerationMask;
    out |= (static_cast<uint32_t>(in.linkSpd) << BusOptionsFields::kLinkSpdShift) &
           BusOptionsFields::kLinkSpdMask;
    return out;
}

// Convenience: update only the generation bits and preserve all other bits (including reserved bits).
struct GenerationUpdate {
    uint32_t busOptionsHost{0};
    uint8_t gen4{0};
};

[[nodiscard]] constexpr uint32_t SetGeneration(GenerationUpdate update) noexcept {
    const uint32_t cleared = (update.busOptionsHost & ~BusOptionsFields::kGenerationMask);
    const uint32_t genBits =
        (static_cast<uint32_t>(update.gen4 & 0x0Fu) << BusOptionsFields::kGenerationShift);
    return cleared | genBits;
}

namespace Detail {
// Local constexpr popcount (avoid <bit> include in this shared header).
[[nodiscard]] constexpr unsigned Popcount32(uint32_t v) noexcept {
    unsigned c = 0;
    while (v != 0) {
        c += (v & 1u);
        v >>= 1u;
    }
    return c;
}
} // namespace Detail

static_assert((BusOptionsFields::kReserved11_10Mask &
               (BusOptionsFields::kCycClkAccMask | BusOptionsFields::kMaxRecMask |
                BusOptionsFields::kMaxROMMask | BusOptionsFields::kGenerationMask |
                BusOptionsFields::kLinkSpdMask | BusOptionsFields::kIRMCMask |
                BusOptionsFields::kCMCMask | BusOptionsFields::kISCMask |
                BusOptionsFields::kBMCMask | BusOptionsFields::kPMCMask)) == 0,
              "BusOptionsFields reserved bits [11:10] must be disjoint from active fields");

static_assert((BusOptionsFields::kReserved3Mask &
               (BusOptionsFields::kCycClkAccMask | BusOptionsFields::kMaxRecMask |
                BusOptionsFields::kMaxROMMask | BusOptionsFields::kGenerationMask |
                BusOptionsFields::kLinkSpdMask | BusOptionsFields::kIRMCMask |
                BusOptionsFields::kCMCMask | BusOptionsFields::kISCMask |
                BusOptionsFields::kBMCMask | BusOptionsFields::kPMCMask)) == 0,
              "BusOptionsFields reserved bit [3] must be disjoint from active fields");

static_assert(Detail::Popcount32(BusOptionsFields::kIRMCMask | BusOptionsFields::kCMCMask |
                                 BusOptionsFields::kISCMask | BusOptionsFields::kBMCMask |
                                 BusOptionsFields::kPMCMask | BusOptionsFields::kCycClkAccMask |
                                 BusOptionsFields::kMaxRecMask | BusOptionsFields::kMaxROMMask |
                                 BusOptionsFields::kGenerationMask |
                                 BusOptionsFields::kLinkSpdMask) ==
                  Detail::Popcount32(BusOptionsFields::kIRMCMask) +
                      Detail::Popcount32(BusOptionsFields::kCMCMask) +
                      Detail::Popcount32(BusOptionsFields::kISCMask) +
                      Detail::Popcount32(BusOptionsFields::kBMCMask) +
                      Detail::Popcount32(BusOptionsFields::kPMCMask) +
                      Detail::Popcount32(BusOptionsFields::kCycClkAccMask) +
                      Detail::Popcount32(BusOptionsFields::kMaxRecMask) +
                      Detail::Popcount32(BusOptionsFields::kMaxROMMask) +
                      Detail::Popcount32(BusOptionsFields::kGenerationMask) +
                      Detail::Popcount32(BusOptionsFields::kLinkSpdMask),
              "BusOptionsFields masks must not overlap");

// ============================================================================
// Max Payload by Speed (Conservative Values)
// ============================================================================

// Max Payload by Speed (DISPLAY-ONLY - use MaxAsyncPayloadBytesFromMaxRec() for actual limits)
namespace MaxPayload {
inline constexpr uint16_t kS100 = 512;  // 100 Mbit/s max payload (display only)
inline constexpr uint16_t kS200 = 1024; // 200 Mbit/s max payload (display only)
inline constexpr uint16_t kS400 = 2048; // 400 Mbit/s max payload (display only)
inline constexpr uint16_t kS800 = 4096; // 800 Mbit/s max payload (1394b, display only)
} // namespace MaxPayload

// ============================================================================
// Compile-Time Validation
// ============================================================================

// Validate CSR address construction
static_assert(kCSRRegSpaceHi == 0xFFFFu,         "CSR register space HI must be 0xFFFF");
static_assert(kCSRRegSpaceLo == 0xF0000000u,     "CSR register space LO must be 0xF0000000");
static_assert(kCSR_ConfigROMBase == 0xF0000400u, "Config ROM base must be 0xF0000400");

// Validate CSR address helper
// CSRAddr(0x3FF, 0xF0000400) = (0x3FF << 48) | (0xFFFF << 32) | 0xF0000400 = 0x03fffffff0000400
static_assert(CSRAddr(0x3FF, 0xF0000400) == 0x03fffffff0000400ULL,
              "CSRAddr helper must produce correct 64-bit address");
static_assert(ConfigROMWord(0x3FF, 0x00) == 0x03fffffff0000400ULL,
              "ConfigROMWord helper must produce correct 64-bit address");

// ============================================================================
// Config ROM helpers and constants
// ============================================================================

// Bus name constant '1394' (ASCII) per OHCI 1.1 §7.2
inline constexpr uint32_t kBusNameQuadlet = 0x31333934u; // '1394'

// CRC polynomial for IEEE 1212 (same as ITU-T CRC-16)
inline constexpr uint16_t kConfigROMCRCPolynomial = 0x1021;

// Helper to build a directory entry (host-endian)
constexpr inline uint32_t MakeDirectoryEntry(uint8_t key, uint8_t type, uint32_t value24) {
    return (static_cast<uint32_t>(type) & 0x3u) << 30 |
           (static_cast<uint32_t>(key) & 0x3Fu) << 24 |
           (value24 & 0x00FFFFFFu);
}

} // namespace ASFW::FW
