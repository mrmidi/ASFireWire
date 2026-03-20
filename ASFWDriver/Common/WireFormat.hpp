// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// WireFormat.hpp — Bit manipulation and byte-order utilities (no FireWire-specific concepts)

#pragma once

#include <cstdint>
#include <cstring>

namespace ASFW::FW {

// ============================================================================
// Bit Manipulation Utilities (Type-Safe, Constexpr)
// ============================================================================

// LSB-0 bit helpers (host convention)
template <class T> constexpr T bit(unsigned n) { return T(1u) << n; }

// MSB-0 bit helpers (CSR convention)
template <class T> constexpr T msb_bit32(unsigned n) { return T(1u) << (31u - n); }

// LSB-0 inclusive range helpers
template <class T> constexpr T bit_range(unsigned msb, unsigned lsb) {
#if !defined(NDEBUG)
    if (msb < lsb)
        __builtin_trap();
#endif
    return ((T(~T(0)) << lsb) & (T(~T(0)) >> (sizeof(T) * 8 - 1 - msb)));
}

// MSB-0 inclusive range helpers (CSR convention)
template <class T> constexpr T msb_range32(unsigned msb, unsigned lsb) {
    return bit_range<T>(31u - lsb, 31u - msb);
}

// ============================================================================
// Byte-array ↔ integer (big-endian wire format)
// ============================================================================

// Read big-endian uint32 from byte array (IEEE 1394 wire format)
[[nodiscard]] inline uint32_t ReadBE32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           (static_cast<uint32_t>(p[3]));
}

// Write uint32 as big-endian to byte array
inline void WriteBE32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8)  & 0xFF);
    p[3] = static_cast<uint8_t>(v          & 0xFF);
}

// Read big-endian uint64 from byte array
[[nodiscard]] inline uint64_t ReadBE64(const uint8_t* p) noexcept {
    return (static_cast<uint64_t>(p[0]) << 56) |
           (static_cast<uint64_t>(p[1]) << 48) |
           (static_cast<uint64_t>(p[2]) << 40) |
           (static_cast<uint64_t>(p[3]) << 32) |
           (static_cast<uint64_t>(p[4]) << 24) |
           (static_cast<uint64_t>(p[5]) << 16) |
           (static_cast<uint64_t>(p[6]) << 8)  |
           (static_cast<uint64_t>(p[7]));
}

// Write uint64 as big-endian to byte array
inline void WriteBE64(uint8_t* p, uint64_t v) noexcept {
    p[0] = static_cast<uint8_t>((v >> 56) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 48) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 40) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 32) & 0xFF);
    p[4] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[5] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[6] = static_cast<uint8_t>((v >> 8)  & 0xFF);
    p[7] = static_cast<uint8_t>(v          & 0xFF);
}

// Compile-time validation
static_assert(sizeof(uint32_t) == 4, "uint32_t must be 4 bytes");
static_assert(sizeof(uint64_t) == 8, "uint64_t must be 8 bytes");

} // namespace ASFW::FW
