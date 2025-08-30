#pragma once
//
// ASFWShared.hpp
// Shared DriverKit-friendly types and constants for ASFireWire
//
// This header centralizes common structs, enums, and small utilities that
// multiple components (controller, link, command objects) rely on.
// Keep this header lightweight; no heavy dependencies.
//

#include <stdint.h>

//
// ASFWAddress
// Rewritten analogue of the legacy FWAddress. Represents a 48-bit node
// address (16-bit high, 32-bit low) paired with a nodeID for routing.
//
struct ASFWAddress {
  uint16_t nodeID{0};    // bus/node ID (updated per generation)
  uint16_t addressHi{0}; // top 16 bits of node address
  uint32_t addressLo{0}; // low 32 bits of node address

  ASFWAddress() = default;
  ASFWAddress(uint16_t hi, uint32_t lo)
      : nodeID(0), addressHi(hi), addressLo(lo) {}
  ASFWAddress(uint16_t hi, uint32_t lo, uint16_t nid)
      : nodeID(nid), addressHi(hi), addressLo(lo) {}
};

//
// ASFWSpeed
// Basic link speeds used throughout higher layers. Mirrors the intent of
// IOFWSpeed but kept project-local. Values are chosen to be compatible with
// simple casts when interoperating with legacy paths in docs/tests.
//
enum class ASFWSpeed : int32_t {
  s100 = 0,
  s200 = 1,
  s400 = 2,
  s800 = 3,
  UnknownMask = 0x80,
  Maximum = 0x7FFFFFFF,
  Invalid = static_cast<int32_t>(0x80000000u) // Cast unsigned to signed
};

//
// ASFWWriteFlags / ASFWReadFlags
// Bitfield enums for async transaction options. Named and valued to make
// translation from legacy flags trivial without including legacy headers.
//
enum ASFWWriteFlags : uint32_t {
  kWriteNone = 0x00000000,
  kWriteDeferredNotify = 0x00000001,
  kWriteFastRetryOnBusy = 0x00000002,
  kWriteBlockRequest = 0x00000004 // force a block request
};

enum ASFWReadFlags : uint32_t {
  kReadNone = 0x00000000,
  kReadBlockRequest = 0x00000004, // force a block request
  kReadPingTime = 0x00000008      // request ping time
};

//
// ASFWSecurityMode
// Basic physical access/security modes exposed by controller policies.
//
enum class ASFWSecurityMode : uint32_t {
  Normal = 0,
  Secure = 1,
  SecurePermanent = 2
};

// Future additions (fill as we port):
// - Common rcode/ack code mappings
// - Node flags definitions used by controller policy
// - Small helpers for CSR space addressing (e.g., constructors for common CSRs)
