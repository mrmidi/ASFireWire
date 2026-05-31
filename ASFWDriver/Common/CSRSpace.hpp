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
#include <span>

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

// Remote CSR core state registers. FW-10 writes STATE_SET.cmstr on the root
// node; these are the same CSR core offsets as the local state aliases, but
// addressed through the target node's CSR space.
inline constexpr uint32_t kCSRRemoteStateClear = kCSR_StateClear;
inline constexpr uint32_t kCSRRemoteStateSet   = kCSR_StateSet;
inline constexpr uint32_t kCSRStateBitCMSTR    = 1u << 8;
// STATE_CLEAR/STATE_SET abdicate bit (IEEE 1394a-2000). Matches Linux
// CSR_STATE_BIT_ABDICATE (core.h: 1<<10). Set via STATE_SET, cleared via
// STATE_CLEAR; consumed once per bus reset by the BM election path.
inline constexpr uint32_t kCSRStateBitABDICATE = 1u << 10;

// RESET_START (IEEE 1394-1995 §8.3.2.1). A write here is defined to behave as a
// STATE_CLEAR write with the ABDICATE bit set (Linux core-transaction.c).
inline constexpr uint32_t kCSR_ResetStart      = kCSRCoreBase + 0x000C;

// IRM/BM resource CSRs (IEEE 1394a-2000 §8.3.2.3.x). On OHCI these four are
// served autonomously by the controller's CSR compare-swap engine for remote
// read/lock — ASFW does NOT software-serve them (matches Linux: handle_registers
// hits BUG() for these offsets). Offsets kept here as the single source of truth.
inline constexpr uint32_t kCSR_BusManagerID       = kCSRCoreBase + 0x021C;
inline constexpr uint32_t kCSR_BandwidthAvailable = kCSRCoreBase + 0x0220;
inline constexpr uint32_t kCSR_ChannelsAvailableHi = kCSRCoreBase + 0x0224;
inline constexpr uint32_t kCSR_ChannelsAvailableLo = kCSRCoreBase + 0x0228;

// BROADCAST_CHANNEL (IEEE 1394a-2000 §8.3.2.3.10). Software-owned by ASFW.
inline constexpr uint32_t kCSR_BroadcastChannel = kCSRCoreBase + 0x0234;
// Initial value: valid-for-transmit bit (1<<31) | channel number 31. Matches
// Linux BROADCAST_CHANNEL_INITIAL (core.h: (1<<31 | 31)).
inline constexpr uint32_t kBroadcastChannelInitial = (1u << 31) | 31u;
// Writable "valid" bit (1<<30). Matches Linux BROADCAST_CHANNEL_VALID — note
// this is bit 30, distinct from the transmit-valid bit 31 in INITIAL.
inline constexpr uint32_t kBroadcastChannelValid   = 1u << 30;

// TOPOLOGY_MAP CSR region (IEEE 1394a-2000 §8.3.2.4.1). Software-served by ASFW;
// the handler covers 0x400 bytes (256 quadlets) like Linux (handle_topology_map).
inline constexpr uint32_t kCSR_TopologyMapBase = kCSRCoreBase + 0x1000;
inline constexpr uint32_t kCSR_TopologyMapEnd  = kCSRCoreBase + 0x13FF;

// SPEED_MAP CSR region. The SPEED_MAP is explicitly marked as Obsoleted in the 1394-2008 standard.
// Software-served as AddressError.
inline constexpr uint32_t kCSR_SpeedMapBase = kCSRCoreBase + 0x2000;
inline constexpr uint32_t kCSR_SpeedMapEnd  = kCSRCoreBase + 0x2FFF;

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
inline constexpr uint8_t kGeneration         = 0x38; // Apple-specific (root dir, immediate)
inline constexpr uint8_t kManagementAgentOffset = 0x38; // SBP-2 (unit dir, CSR offset type=1)
inline constexpr uint8_t kUnitCharacteristics   = 0x39; // SBP-2 (unit dir, immediate)
inline constexpr uint8_t kFastStart             = 0x3A; // SBP-2 (unit dir, leaf)
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

// Normalize the controller's hardware BusOptions for advertisement in the local
// Config ROM Bus_Info_Block. The ONLY capability this asserts a policy on is bmc.
//
// OHCI controllers commonly default their BusOptions register to bmc=1 (Bus
// Manager Capable). ASFW does NOT implement IEEE 1394a BUS_MANAGER_ID
// compare-swap election, so it must not advertise bmc=1 — other nodes would
// otherwise expect ASFW to manage the bus.
//
// Policy (FW-11):
//   - bmc : forced to 0. >>> Flip this when BUS_MANAGER_ID election lands. <<<
//   - irmc / cmc / isc / pmc and all numeric fields (cyc_clk_acc, max_rec,
//     max_ROM, generation, link_spd): PRESERVED from hardware, UNCHANGED. The
//     OHCI register is authoritative for physical capability — do not fabricate
// FW-22: role mode driving local BIB capability advertisement. ASFW must never
// advertise a role it cannot serve, so the advertised bits are mode-gated.
enum class RoleMode : uint8_t {
    // Keeps the legacy behavior (FW-11): force bmc=0, preserve hardware
    // irmc/cmc/isc/pmc. Included for backwards-compatibility verification.
    LegacyBmcCleared = 0,
    // Pure client: force bmc=0, irmc=0 so the node advertises no management
    // capability and will not be elected as IRM or BM. This is MORE conservative
    // than the reference stacks (Apple/Linux advertise bmc=1 from the OHCI bits
    // and actively manage); it is ASFW's safe posture until full BM/IRM machinery
    // is hardware-proven. Not "Apple behavior" — Apple does the opposite.
    ClientOnly = 1,
    // ASFW advertises IRM capability and can host the local IRM resource set
    // when the local node wins IRM election (FW-13/FW-19). OHCI hardware
    // autonomously serves the four core IRM CSRs. ASFW software owns
    // BROADCAST_CHANNEL and policy/diagnostics. This mode does not perform
    // full Bus Manager election or topology mutation.
    IRMResourceHost = 2,
    // Full Bus Manager: bmc=1, irmc=1 (legal only once FW-18/19/20/21 land).
    FullBusManager = 3,
};
// Capability ladder for FullBusManager mode, ordered least- to most-invasive so
// the `>=` threshold gates compose correctly. RemoteCmstrAllowed is LAST:
// sending remote STATE_SET.cmstr writes to the root node if it is CMC-capable
// but not currently cycling. This is a spec/Linux-compatible path; not the
// Apple-compatible default; explicit opt-in only.
//
// NOTE: the numeric values cross the diagnostics ABI to the Swift GUI
// (ASFWDiagnosticsABI.h + DiagnosticsTextFormatter.swift) — keep both in sync.
enum class FullBMActivityLevel : uint8_t {
    ObserveOnly = 0,
    ElectionOnly = 1,
    CyclePolicyAllowed = 2,
    GapPolicyAllowed = 3,
    ForceRootAllowed = 4,
    RemoteCmstrAllowed = 5,
};

// A BIB advertising bmc=1 MUST also advertise irmc=1 (IEEE 1394a-2000: a
// BM-capable node is required to be IRM-capable). Other combinations are legal.
[[nodiscard]] constexpr bool IsLegalCapabilityCombo(uint32_t busOptionsHost) noexcept {
    const bool bmc  = (busOptionsHost & BusOptionsFields::kBMCMask) != 0;
    const bool irmc = (busOptionsHost & BusOptionsFields::kIRMCMask) != 0;
    return !(bmc && !irmc);
}

// Mode-driven normalizer. Manipulates capability bits directly (rather than
// Decode->Encode) so reserved bits [11:10]/[3] and all numeric fields are
// preserved byte-for-byte from the hardware register in every mode.
// Safety default for activityLevel: ObserveOnly. A FullBusManager caller must
// explicitly opt in to ElectionOnly-or-higher before this advertises bmc=1; no
// caller should advertise Bus-Manager-Capable by accidentally omitting the level.
[[nodiscard]] constexpr uint32_t NormalizeLocalBusOptions(uint32_t hwBusOptions,
                                                          RoleMode mode,
                                                          FullBMActivityLevel activityLevel = FullBMActivityLevel::ObserveOnly) noexcept {
    using namespace BusOptionsFields;
    uint32_t out = hwBusOptions;
    switch (mode) {
    case RoleMode::LegacyBmcCleared:
        out &= ~kBMCMask; // bmc=0; preserve everything else
        break;
    case RoleMode::ClientOnly:
        out &= ~(kBMCMask | kIRMCMask); // bmc=0, irmc=0; pure client, no management
        break;
    case RoleMode::IRMResourceHost:
        out &= ~kBMCMask; // bmc=0
        out |= kIRMCMask; // irmc=1
        break;
    case RoleMode::FullBusManager:
        if (activityLevel >= FullBMActivityLevel::ElectionOnly) {
            out |= (kBMCMask | kIRMCMask); // bmc=1 implies irmc=1
        } else {
            out &= ~(kBMCMask | kIRMCMask); // bmc=0, irmc=0 (ObserveOnly is fully passive)
        }
        break;
    }
    return out;
}

// Legacy 1-arg overload: LegacyBmcCleared semantics (preserves legacy callers).
[[nodiscard]] constexpr uint32_t NormalizeLocalBusOptions(uint32_t hwBusOptions) noexcept {
    return NormalizeLocalBusOptions(hwBusOptions, RoleMode::LegacyBmcCleared);
}

// Mode invariants: every mode must yield a legal capability combo, and the
// default must still force bmc=0 while leaving irmc untouched (regression guard).
static_assert((NormalizeLocalBusOptions(0xFFFFFFFFu, RoleMode::LegacyBmcCleared) &
               BusOptionsFields::kBMCMask) == 0,
              "LegacyBmcCleared must force bmc=0");
static_assert((NormalizeLocalBusOptions(0xFFFFFFFFu, RoleMode::LegacyBmcCleared) &
               BusOptionsFields::kIRMCMask) != 0,
              "LegacyBmcCleared must preserve hardware irmc");
static_assert(NormalizeLocalBusOptions(0x00000000u, RoleMode::LegacyBmcCleared) == 0x00000000u,
              "LegacyBmcCleared must be a pure passthrough when bmc already 0");
static_assert((NormalizeLocalBusOptions(0xFFFFFFFFu, RoleMode::ClientOnly) &
               (BusOptionsFields::kBMCMask | BusOptionsFields::kIRMCMask)) == 0,
              "ClientOnly must force bmc=0 and irmc=0");
static_assert((NormalizeLocalBusOptions(0x00000000u, RoleMode::IRMResourceHost) &
               (BusOptionsFields::kIRMCMask | BusOptionsFields::kBMCMask)) ==
                  BusOptionsFields::kIRMCMask,
              "IRMResourceHost must set irmc=1 and bmc=0");
static_assert((NormalizeLocalBusOptions(0x00000000u, RoleMode::FullBusManager,
                                        FullBMActivityLevel::ElectionOnly) &
               (BusOptionsFields::kIRMCMask | BusOptionsFields::kBMCMask)) ==
                  (BusOptionsFields::kIRMCMask | BusOptionsFields::kBMCMask),
              "FullBusManager at ElectionOnly+ must set bmc=1 and irmc=1");
static_assert(IsLegalCapabilityCombo(NormalizeLocalBusOptions(0xFFFFFFFFu, RoleMode::LegacyBmcCleared)) &&
              IsLegalCapabilityCombo(NormalizeLocalBusOptions(0xFFFFFFFFu, RoleMode::ClientOnly)) &&
              IsLegalCapabilityCombo(NormalizeLocalBusOptions(0xFFFFFFFFu, RoleMode::IRMResourceHost)) &&
              IsLegalCapabilityCombo(NormalizeLocalBusOptions(0xFFFFFFFFu, RoleMode::FullBusManager)) &&
              IsLegalCapabilityCombo(NormalizeLocalBusOptions(0x00000000u, RoleMode::FullBusManager)),
              "every RoleMode must produce a legal capability combo");

static_assert((NormalizeLocalBusOptions(0xFFFFFFFFu, RoleMode::FullBusManager, FullBMActivityLevel::ObserveOnly) &
               (BusOptionsFields::kBMCMask | BusOptionsFields::kIRMCMask)) == 0,
              "FullBusManager + ObserveOnly must force bmc=0 and irmc=0");
static_assert((NormalizeLocalBusOptions(0x00000000u, RoleMode::FullBusManager, FullBMActivityLevel::ObserveOnly) &
               (BusOptionsFields::kIRMCMask | BusOptionsFields::kBMCMask)) == 0,
              "FullBusManager + ObserveOnly must remain fully passive");

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

// Validate BM/IRM CSR offsets against IEEE 1394a-2000 fixed addresses.
static_assert(kCSR_ResetStart == 0xF000000Cu, "RESET_START must be 0xF000000C");
static_assert(kCSR_BusManagerID == 0xF000021Cu, "BUS_MANAGER_ID must be 0xF000021C");
static_assert(kCSR_BandwidthAvailable == 0xF0000220u, "BANDWIDTH_AVAILABLE must be 0xF0000220");
static_assert(kCSR_ChannelsAvailableHi == 0xF0000224u, "CHANNELS_AVAILABLE_HI must be 0xF0000224");
static_assert(kCSR_ChannelsAvailableLo == 0xF0000228u, "CHANNELS_AVAILABLE_LO must be 0xF0000228");
static_assert(kCSR_BroadcastChannel == 0xF0000234u, "BROADCAST_CHANNEL must be 0xF0000234");
static_assert(kCSR_TopologyMapBase == 0xF0001000u, "TOPOLOGY_MAP base must be 0xF0001000");
static_assert(kCSR_TopologyMapEnd == 0xF00013FFu, "TOPOLOGY_MAP end must be 0xF00013FF");
static_assert(kBroadcastChannelInitial == 0x8000001Fu, "BROADCAST_CHANNEL initial must be 0x8000001F");
static_assert(kBroadcastChannelValid == 0x40000000u, "BROADCAST_CHANNEL valid must be bit 30");
static_assert(kCSRStateBitABDICATE == 0x00000400u, "STATE abdicate must be bit 10");

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

[[nodiscard]] constexpr uint16_t CRCStep(uint16_t crc, uint16_t data) noexcept {
    crc = static_cast<uint16_t>(crc ^ data);
    for (int bit = 0; bit < 16; ++bit) {
        if ((crc & 0x8000U) != 0U) {
            crc = static_cast<uint16_t>((crc << 1) ^ kConfigROMCRCPolynomial);
        } else {
            crc = static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

[[nodiscard]] constexpr uint16_t ComputeBlockCRC16(std::span<const uint32_t> block) noexcept {
    uint16_t crc = 0;
    for (uint32_t quadletHost : block) {
        crc = CRCStep(crc, static_cast<uint16_t>((quadletHost >> 16) & 0xFFFFU));
        crc = CRCStep(crc, static_cast<uint16_t>(quadletHost & 0xFFFFU));
    }
    return crc;
}

} // namespace ASFW::FW
