// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// FWTypes.hpp — FireWire protocol enums, strong types, and transport constants

#pragma once

#include <cstdint>
#include <type_traits>

namespace ASFW::FW {

// ============================================================================
// Wire-Level Ack/Response Enums (SINGLE SOURCE)
// ============================================================================
// These are IEEE 1394 wire-level codes, distinct from OHCI hardware events.

/**
 * Wire-level ACK codes (IEEE 1394-1995 §6.2.4.3).
 * These are the ACK codes returned by the destination node in response to a request.
 */
enum class Ack : int8_t {
    Timeout = -1,   // Local pseudo-ack (timeout - not sent on wire)
    Unknown = 0,    // Not wire-encoded; guard for decode
    Complete = 1,   // ACK_COMPLETE (0x01) - Transaction completed successfully
    Pending = 2,    // ACK_PENDING (0x02) - Transaction pending, response will follow
    BusyX = 4,      // ACK_BUSY_X (0x04) - Resource busy, retry with exponential backoff
    BusyA = 5,      // ACK_BUSY_A (0x05) - Resource busy, retry with type A
    BusyB = 6,      // ACK_BUSY_B (0x06) - Resource busy, retry with type B
    DataError = 13, // ACK_DATA_ERROR (0x0D) - Data error
    TypeError = 14, // ACK_TYPE_ERROR (0x0E) - Type error
};

/**
 * Wire-level Response codes (IEEE 1394-1995 Table 3-3).
 * These are the response codes in response packets (tCode 0x2, 0x6, 0x7, 0xB).
 */
enum class Response : uint8_t {
    Complete = 0,      // RESP_COMPLETE - Transaction completed successfully
    ConflictError = 4, // RESP_CONFLICT_ERROR - Resource conflict, may retry
    DataError = 5,     // RESP_DATA_ERROR - Data not available
    TypeError = 6,     // RESP_TYPE_ERROR - Operation not supported
    AddressError = 7,  // RESP_ADDRESS_ERROR - Address not valid in target device
    BusReset = 16,     // RESP_BUS_RESET - Pseudo response generated locally (bus reset)
    Pending = 17,      // RESP_PENDING - Pseudo response, real response sent later
    Unknown = 0xFF,    // Not wire-encoded; guard for decode
};

/**
 * Human-readable name for ACK code.
 */
inline const char* AckName(Ack a) {
    switch (a) {
    case Ack::Timeout:   return "Timeout";
    case Ack::Unknown:   return "Unknown";
    case Ack::Complete:  return "Complete";
    case Ack::Pending:   return "Pending";
    case Ack::BusyX:     return "BusyX";
    case Ack::BusyA:     return "BusyA";
    case Ack::BusyB:     return "BusyB";
    case Ack::DataError: return "DataError";
    case Ack::TypeError: return "TypeError";
    }
    return "Unknown";
}

/**
 * Human-readable name for Response code.
 */
inline const char* RespName(Response r) {
    switch (r) {
    case Response::Complete:      return "Complete";
    case Response::ConflictError: return "Conflict";
    case Response::DataError:     return "DataError";
    case Response::TypeError:     return "TypeError";
    case Response::AddressError:  return "AddressError";
    case Response::BusReset:      return "BusReset";
    case Response::Pending:       return "Pending";
    case Response::Unknown:       return "Unknown";
    }
    return "Unknown";
}

/**
 * Convert raw ACK code byte to Ack enum.
 */
[[nodiscard]] inline Ack AckFromByte(uint8_t byte) {
    switch (byte) {
    case 0x01: return Ack::Complete;
    case 0x02: return Ack::Pending;
    case 0x04: return Ack::BusyX;
    case 0x05: return Ack::BusyA;
    case 0x06: return Ack::BusyB;
    case 0x0D: return Ack::DataError;
    case 0x0E: return Ack::TypeError;
    default:   return Ack::Unknown;
    }
}

/**
 * Convert raw Response code byte to Response enum.
 */
[[nodiscard]] inline Response ResponseFromByte(uint8_t byte) {
    switch (byte) {
    case 0x00: return Response::Complete;
    case 0x04: return Response::ConflictError;
    case 0x05: return Response::DataError;
    case 0x06: return Response::TypeError;
    case 0x07: return Response::AddressError;
    case 0x10: return Response::BusReset;
    case 0x11: return Response::Pending;
    default:   return Response::Unknown;
    }
}

// ============================================================================
// Bus Speed (SINGLE SOURCE)
// ============================================================================

/**
 * IEEE 1394-1995 speed codes.
 * These match the on-wire Self-ID speed field encoding (IEEE 1394-1995 §8.4.2.4).
 */
enum class Speed : uint8_t {
    S100 = 0, // 100 Mbit/s
    S200 = 1, // 200 Mbit/s
    S400 = 2, // 400 Mbit/s (most common)
    S800 = 3, // 800 Mbit/s (1394b) / Reserved
};

// Alias for DiscoveryValues.hpp compatibility
using FwSpeed = Speed;

/**
 * Human-readable name for speed code.
 */
inline const char* SpeedName(Speed s) {
    switch (s) {
    case Speed::S100: return "S100";
    case Speed::S200: return "S200";
    case Speed::S400: return "S400";
    case Speed::S800: return "S800";
    }
    return "Reserved";
}

// ============================================================================
// Strong types for interface facades.
// ============================================================================

/**
 * Bus generation number (increments on each bus reset).
 *
 * Valid range: 0-65535 (16-bit extended generation).
 * Used for validating async operations to prevent stale reads/writes.
 */
struct Generation {
    uint32_t value;

    explicit constexpr Generation(uint32_t v) : value(v) {}
    constexpr bool operator==(const Generation& other) const { return value == other.value; }
    constexpr bool operator!=(const Generation& other) const { return value != other.value; }
};

static_assert(std::is_trivially_copyable_v<Generation>);
static_assert(sizeof(Generation) <= sizeof(uint32_t));

/**
 * FireWire node ID (0-63 per bus).
 *
 * Format: bus[15:10] | node[5:0]
 * Valid node IDs are 0-62, with 63 (0x3F) reserved for broadcast.
 */
struct NodeId {
    uint8_t value;

    explicit constexpr NodeId(uint8_t v) : value(v) {}
    constexpr bool IsValid() const { return value < 64; }
    constexpr bool operator==(const NodeId& other) const { return value == other.value; }
    constexpr bool operator!=(const NodeId& other) const { return value != other.value; }
};

inline constexpr NodeId kInvalidNodeId{0xFF};
inline constexpr NodeId kBroadcastNodeId{0x3F};

/**
 * Atomic lock operation types (IEEE 1394-1995 Table 3-3).
 *
 * Lock operations provide atomic read-modify-write semantics on remote memory.
 * The extended tCode field selects the operation type.
 *
 * CRITICAL: These values MUST match IEEE 1394 extended tCode wire format!
 * They are cast directly to extTcode in FireWireBusImpl::Lock().
 */
enum class LockOp : uint8_t {
    kMaskSwap   = 1, ///< extTcode 0x1: Masked swap: old = *addr; *addr = (old & ~arg) | (data & arg)
    kCompareSwap = 2, ///< extTcode 0x2: Compare-and-swap: if (*addr == arg) *addr = data
    kFetchAdd   = 3, ///< extTcode 0x3: Atomic add: old = *addr; *addr += arg
    kLittleAdd  = 4, ///< extTcode 0x4: Little-endian fetch-add
    kBoundedAdd = 5, ///< extTcode 0x5: Fetch-add with upper bound
    kWrapAdd    = 6, ///< extTcode 0x6: Fetch-add with wrapping
};

/**
 * Maximum async payload bytes from MaxRec field.
 * Formula: bytes = 4 * (2^(maxRec + 1))
 * Reference: IEEE 1394-1995 §6.2.3.1
 */
inline constexpr uint32_t MaxAsyncPayloadBytesFromMaxRec(uint8_t maxRec) {
    return 4u << (maxRec + 1);
}

// ============================================================================
// Compile-Time Validation
// ============================================================================

// Validate ACK/Response enum values
static_assert(static_cast<int8_t>(Ack::Timeout) == -1,   "Ack::Timeout must be -1");
static_assert(static_cast<uint8_t>(Ack::Complete) == 1,  "Ack::Complete must be 1");
static_assert(static_cast<uint8_t>(Response::Complete) == 0,  "Response::Complete must be 0");
static_assert(static_cast<uint8_t>(Response::BusReset) == 16, "Response::BusReset must be 16");

} // namespace ASFW::FW
