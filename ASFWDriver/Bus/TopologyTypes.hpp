#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <optional>
#include <utility>

namespace ASFW::Driver {

// Self-ID quadlet bit masks and shifts
// Source: IEEE 1394-2008 (Beta PHY), §16.3.3 / §16.3.3.1 — Figure 16-11 and Table 16-13.
// These constants map the wire-format Self‑ID quadlet fields (phy_ID, L/link_active,
// gap_cnt, sp (speed), brdg (bridge), c (contender), pwr (power class), p0..p15 (port
// connection states), i (initiated_reset), m (more_packets)). OHCI provides the
// mechanism to capture Self‑ID quadlets (SelfIDBuffer / SelfIDCount) but does not
// re-document the wire-format bitfields; the IEEE 1394 standard is the canonical
// source for these definitions.

// Packet identifier (top two bits) — Self‑ID packets use the '10' pattern in the
// packet identifier bits; kSelfIDTagValue is the expected tagged value for a
// Self‑ID quadlet when masked with kSelfIDTagMask.
constexpr uint32_t kSelfIDTagMask = 0xC0000000u; // bits [31:30] (packet identifier)
constexpr uint32_t kSelfIDTagValue = 0x80000000u; // '10' in the top two bits => Self‑ID

// phy_ID field (6 bits) — physical node identifier (Table 16-13)
constexpr uint32_t kSelfIDPhyMask = 0x3F000000u;
constexpr uint32_t kSelfIDPhyShift = 24;

// Extended / link active flags
constexpr uint32_t kSelfIDIsExtendedMask = 0x00800000u; // 'n' / extended packet indicator
constexpr uint32_t kSelfIDLinkActiveMask = 0x00400000u; // 'L' / link_active (Table 16-13)

// gap_cnt (6 bits) and sequence number fields
constexpr uint32_t kSelfIDGapMask = 0x003F0000u; // gap_cnt
constexpr uint32_t kSelfIDGapShift = 16;
constexpr uint32_t kSelfIDSeqMask = 0x00700000u; // sequence number 'n' for extended packets
constexpr uint32_t kSelfIDSeqShift = 20;

// Speed (sp) 2-bit field (index into kSpeedToMbps)
constexpr uint32_t kSelfIDSpeedMask = 0x0000C000u;
constexpr uint32_t kSelfIDSpeedShift = 14;

// Contender (c) and power class (pwr)
constexpr uint32_t kSelfIDContenderMask = 0x00000800u; // 'c' bit
constexpr uint32_t kSelfIDPowerMask = 0x00000700u; // pwr (3 bits)
constexpr uint32_t kSelfIDPowerShift = 8;

// Port states (p0..p2 for first three shown; additional ports are packed similarly)
// Each port status is 2 bits: 00=NotPresent, 01=NotActive, 10=Parent, 11=Child
constexpr uint32_t kSelfIDP0Mask = 0x000000C0u;
constexpr uint32_t kSelfIDP1Mask = 0x00000030u;
constexpr uint32_t kSelfIDP2Mask = 0x0000000Cu;

// More packets flag (LSB) — 'm' indicating another self-ID packet follows for this PHY
constexpr uint32_t kSelfIDMoreMask = 0x00000001u;

enum class PortState : uint8_t {
    NotPresent = 0,
    NotActive = 1,
    Parent = 2,
    Child = 3,
};

// Power class (pwr) enumeration matching Table 16-13 descriptions
enum class PowerClass : uint8_t {
    NoPower = 0,        // 000b
    SelfPower_15W = 1,  // 001b
    SelfPower_30W = 2,  // 010b
    SelfPower_45W = 3,  // 011b
    BusPowered_UpTo3W = 4, // 100b
    Reserved101 = 5,       // 101b (reserved)
    BusPowered_3W_plus3 = 6, // 110b (bus powered + additional 3W)
    BusPowered_3W_plus7 = 7  // 111b (bus powered + additional 7W)
};

// Speed translation table (index -> Mbps). The IEEE table notes Beta PHY uses value '11'
// for Beta mode and other values for legacy/alpha modes; mapping here follows the
// commonly-used kernel translation (index -> nominal Mbps values).
constexpr std::array<uint32_t, 8> kSpeedToMbps = {100, 200, 400, 800, 1600, 3200, 6400, 12800};

inline PortState DecodePort(uint32_t code) {
    return static_cast<PortState>(code & 0x3u);
}

inline uint32_t DecodeSpeed(uint32_t code) {
    return kSpeedToMbps[code < kSpeedToMbps.size() ? code : (kSpeedToMbps.size() - 1)];
}

// Small utilities to extract and interpret common Self-ID fields from a raw quadlet
inline bool IsSelfIDTag(uint32_t quad) {
    return (quad & kSelfIDTagMask) == kSelfIDTagValue;
}

inline uint8_t ExtractPhyID(uint32_t quad) {
    return static_cast<uint8_t>((quad & kSelfIDPhyMask) >> kSelfIDPhyShift);
}

inline bool IsExtended(uint32_t quad) {
    return (quad & kSelfIDIsExtendedMask) != 0;
}

inline bool IsLinkActive(uint32_t quad) {
    return (quad & kSelfIDLinkActiveMask) != 0;
}

// Initiated reset flag (i): set when a node initiated a bus reset
inline bool IsInitiatedReset(uint32_t quad) {
    return (quad & 0x00000002u) != 0;
}

inline uint8_t ExtractGapCount(uint32_t quad) {
    return static_cast<uint8_t>((quad & kSelfIDGapMask) >> kSelfIDGapShift);
}

inline uint8_t ExtractSeq(uint32_t quad) {
    return static_cast<uint8_t>((quad & kSelfIDSeqMask) >> kSelfIDSeqShift);
}

inline bool IsContender(uint32_t quad) {
    return (quad & kSelfIDContenderMask) != 0;
}

inline PowerClass ExtractPowerClass(uint32_t quad) {
    return static_cast<PowerClass>((quad & kSelfIDPowerMask) >> kSelfIDPowerShift);
}

// Extract the raw 2-bit speed code (index) from the quadlet
inline uint8_t ExtractSpeedCode(uint32_t quad) {
    return static_cast<uint8_t>((quad & kSelfIDSpeedMask) >> kSelfIDSpeedShift);
}

// Returns true when the 'more packets' (m) flag is set indicating additional
// quadlets follow for the same Self-ID sequence.
inline bool HasMorePackets(uint32_t quad) {
    return (quad & kSelfIDMoreMask) != 0;
}

inline const char* PowerClassToString(PowerClass p) {
    switch (p) {
        case PowerClass::NoPower: return "NoPower";
        case PowerClass::SelfPower_15W: return "SelfPower_15W";
        case PowerClass::SelfPower_30W: return "SelfPower_30W";
        case PowerClass::SelfPower_45W: return "SelfPower_45W";
        case PowerClass::BusPowered_UpTo3W: return "BusPowered_UpTo3W";
        case PowerClass::Reserved101: return "Reserved101";
        case PowerClass::BusPowered_3W_plus3: return "BusPowered_3W_plus3";
        case PowerClass::BusPowered_3W_plus7: return "BusPowered_3W_plus7";
        default: return "Unknown";
    }
}

// Extract the 2-bit port status for port index (0..15). Returns PortState.
// Ports are packed as p0 (bits 7:6), p1 (5:4), p2 (3:2) in the primary quadlet, extended
// ports appear in subsequent quadlets for extended Self-ID packets.
inline PortState ExtractPortState(uint32_t quad, unsigned portIndex) {
    // Only supports first 3 ports in the base quadlet; callers should read extended
    // quadlets for p3..p15 as described in Figure 16-11 when IsExtended() is true.
    unsigned shift = 6 - (portIndex * 2);
    if (portIndex > 2) return PortState::NotPresent; // caller must handle extended packets
    uint32_t code = (quad >> shift) & 0x3u;
    return DecodePort(code);
}

// Maximum number of quadlets allowed in a single Self-ID sequence (base + extended)
constexpr unsigned int kSelfIDSequenceMaximumQuadletCount = 4u;

// Enumerator to iterate over Self-ID sequences stored as quadlets.
// Mirrors the behavior of the C helper `self_id_sequence_enumerator_next()`:
// - Validates 'more packets' chaining
// - Validates extended-quadlet sequence numbers
// - Caps by kSelfIDSequenceMaximumQuadletCount and provided quadlet_count
struct SelfIDSequenceEnumerator {
    const uint32_t* cursor{nullptr};
    unsigned int quadlet_count{0};

    // Returns {pointer_to_sequence_start, quadlet_count} on success or nullopt on error/underflow
    std::optional<std::pair<const uint32_t*, unsigned int>> next() {
        if (cursor == nullptr || quadlet_count == 0)
            return std::nullopt;

        const uint32_t* start = cursor;
        unsigned int count = 1;

        uint32_t quadlet = *start;
        unsigned int sequence = 0;
        // While the 'more packets' flag is set, advance and validate extended quadlets
        while ((quadlet & kSelfIDMoreMask) != 0) {
            if (count >= quadlet_count || count >= kSelfIDSequenceMaximumQuadletCount)
                return std::nullopt;
            ++start;
            ++count;
            quadlet = *start;

            if (!IsExtended(quadlet) || sequence != ExtractSeq(quadlet))
                return std::nullopt;
            ++sequence;
        }

        const uint32_t* result_ptr = cursor;
        // advance the enumerator state
        cursor += count;
        quadlet_count -= count;

        return std::make_pair(result_ptr, count);
    }
};

} // namespace ASFW::Driver
