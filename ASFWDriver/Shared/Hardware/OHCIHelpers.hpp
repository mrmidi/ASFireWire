#pragma once

#include <cstdint>

namespace ASFW::Shared {

/**
 * Generic OHCI Hardware Constants
 *
 * These constants are defined by the OHCI specification and are protocol-agnostic.
 * They apply to all OHCI-compliant controllers regardless of the higher-level protocol.
 */

/// OHCI DMA Address Bits (OHCI §7.1.5.1)
/// OHCI only supports 32-bit physical addresses for descriptor chains
constexpr uint32_t kOHCIDmaAddressBits = 32;

/// OHCI Branch Address Bits (OHCI §7.1.5.1, Table 7-3)
/// branchWord format stores address in bits [31:4], leaving lower 4 bits for Z field
constexpr uint32_t kOHCIBranchAddressBits = kOHCIDmaAddressBits - 4; // bits [31:4]

// Compile-time validation of OHCI spec constants
static_assert(kOHCIDmaAddressBits == 32,
              "OHCI DMA only supports 32-bit physical addresses (see OHCI §7.1.5.1)");
static_assert(kOHCIBranchAddressBits == 28,
              "BranchWord encoding hard-codes 28 address bits (bits [31:4]); "
              "update branch word helpers if the spec changes.");

/**
 * IEEE 1394 Endianness Conversion Helpers
 *
 * CRITICAL ENDIANNESS REQUIREMENTS:
 *
 * - **OHCI Descriptors**: Host byte order (little-endian on x86/ARM)
 *   Per OHCI §7: "Descriptors are fetched via PCI in the host's native byte order"
 *   Descriptor fields (control, dataAddress, branchWord, statusWord) must be in host order.
 *
 * - **IEEE 1394 Packet Headers**: Big-endian (network byte order)
 *   Per IEEE 1394-1995 §6.2: "All multi-byte fields transmitted MSB first"
 *   Packet header fields must be converted to/from big-endian for wire transmission.
 *
 * Use ToBigEndian* ONLY for packet headers, NOT for descriptor fields!
 */

/// Convert 16-bit host value to big-endian (IEEE 1394 wire format).
///
/// Use ONLY for 1394 packet header fields, NOT for OHCI descriptor fields.
/// Spec: IEEE 1394-1995 §6.2 — all packet fields transmitted MSB first
///
/// @param value Host-order 16-bit value
/// @return Big-endian 16-bit value for wire transmission
[[nodiscard]] constexpr uint16_t ToBigEndian16(uint16_t value) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap16(value);
#else
    return value;
#endif
}

/// Convert 32-bit host value to big-endian (IEEE 1394 wire format).
///
/// Use ONLY for 1394 packet header fields, NOT for OHCI descriptor fields.
/// Spec: IEEE 1394-1995 §6.2 — all packet fields transmitted MSB first
///
/// @param value Host-order 32-bit value
/// @return Big-endian 32-bit value for wire transmission
[[nodiscard]] constexpr uint32_t ToBigEndian32(uint32_t value) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap32(value);
#else
    return value;
#endif
}

/// Convert 64-bit host value to big-endian (IEEE 1394 wire format).
///
/// Use ONLY for 1394 packet header fields, NOT for OHCI descriptor fields.
/// Spec: IEEE 1394-1995 §6.2 — all packet fields transmitted MSB first
///
/// @param value Host-order 64-bit value
/// @return Big-endian 64-bit value for wire transmission
[[nodiscard]] constexpr uint64_t ToBigEndian64(uint64_t value) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(value);
#else
    return value;
#endif
}

/// Convert 16-bit big-endian value to host byte order.
///
/// Use for parsing received 1394 packet headers.
///
/// @param value Big-endian 16-bit value from wire
/// @return Host-order 16-bit value
[[nodiscard]] constexpr uint16_t FromBigEndian16(uint16_t value) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap16(value);
#else
    return value;
#endif
}

/// Convert 32-bit big-endian value to host byte order.
///
/// Use for parsing received 1394 packet headers.
///
/// @param value Big-endian 32-bit value from wire
/// @return Host-order 32-bit value
[[nodiscard]] constexpr uint32_t FromBigEndian32(uint32_t value) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap32(value);
#else
    return value;
#endif
}

/// Convert 64-bit big-endian value to host byte order.
///
/// Use for parsing received 1394 packet headers.
///
/// @param value Big-endian 64-bit value from wire
/// @return Host-order 64-bit value
[[nodiscard]] constexpr uint64_t FromBigEndian64(uint64_t value) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(value);
#else
    return value;
#endif
}

} // namespace ASFW::Shared
