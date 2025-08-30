//
// SelfIDDecode.hpp
// Pure self-ID buffer decoder (no I/O)
// - IEEE 1394-2008 Alpha self-ID (§16.3.2.1)
// - OHCI 1.1 self-ID receive buffer format (§11.3)
//
// Responsibilities:
//   • Validate/tag-scan the OHCI self-ID buffer (header + tagged quads)
//   • Extract per-PHY Alpha records (+ optional extension packets)
//   • Produce a clean, testable Result for higher layers (Topology)
//
// Notes:
//   • No DriverKit deps. Suitable for unit tests/fuzz.
//   • Beta self-ID can be added later without breaking API.
//

#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace SelfID {

// Link speed codes for Alpha self-ID (2 bits).
enum class LinkSpeed : uint8_t { S100 = 0, S200 = 1, S400 = 2, Reserved = 3 };

// Per-port code (Table 16-4).
enum class PortCode : uint8_t {
  NotPresent = 0, // no connector
  NotActive = 1,  // present/idle
  Parent = 2,     // active → parent
  Child = 3       // active → child
};

// Decoded Alpha self-ID record for one PHY/node.
struct AlphaRecord {
  uint8_t phyId = 0; // PHY ID (0..63)
  bool linkActive = false;
  uint8_t gapCount = 0; // recommended gap count
  LinkSpeed speed = LinkSpeed::S100;
  bool delay = false;     // “del” field
  bool contender = false; // cycle master contender
  uint8_t powerClass = 0; // IEEE power class (0..7), 4=self-powered
  bool initiated = false; // ‘i’ bit
  bool more = false;      // ‘m’ bit (extended packets present)

  // Up to 16 ports total (3 in alpha + 2×10 via ext packets; practical cap=16)
  // Ports unused are left as NotPresent.
  PortCode ports[16] = {PortCode::NotPresent};
};

// Decoder diagnostics (optional, human-readable).
struct Warning {
  std::string message;
};

// Final decode result for a self-ID buffer.
struct Result {
  uint32_t generation = 0;        // copied from OHCI header (for correlation)
  std::vector<AlphaRecord> nodes; // one per PHY that reported self-ID
  bool integrityOk = true;        // inverted-quadlet checks etc.
  std::vector<Warning> warnings;  // non-fatal anomalies
};

// Decode an OHCI self-ID receive buffer (host-endian quadlets as read from CPU
// map). buffer[0] is the OHCI header quadlet; data starts at buffer[1]. Returns
// a structured Result; never throws.
Result Decode(const uint32_t *buffer, uint32_t quadletCount);

} // namespace SelfID
