//
// ConfigROMProperties.hpp
// Container types for parsed *remote* node IEEE-1212 (Config ROM) properties
//
// Purpose:
//   • Keep parsed directory/leaf data per node, decoupled from transport
//   • Feed higher-level discovery (AV/C, AMDTP) without coupling to parsing
//
// Status:
//   • Data-only. No parser here. You can fill these from a future AR reader.

#pragma once

#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

// Minimal “Unit Directory” view commonly used by A/V devices.
// Populate from the node’s root directory → unit directories.
struct UnitDirectory {
  // IEEE 1212 keys (subset)
  uint32_t specId = 0;    // Unit_Spec_ID
  uint32_t swVersion = 0; // Unit_Sw_Version
  uint32_t modelId = 0;   // Model_ID (if present)
  uint32_t vendorId = 0;  // Vendor_ID (fallback if not in root)
  // Extra raw entries for vendor quirks or future keys: (key, 24-bit value)
  std::vector<std::pair<uint8_t, uint32_t>> extras;
};

// High-level per-node ROM snapshot.
struct ConfigROMProperties {
  uint64_t eui64 = 0;
  uint32_t vendorId = 0; // from root Vendor_ID or EUI-64 OUI
  uint32_t nodeCaps = 0; // Node_Capabilities (mirrors BusOptions)
  // Optional: textual vendor/model names if you parse textual leaves later.
  std::string vendorName;
  std::string modelName;

  std::vector<UnitDirectory> units; // zero or more

  // Helpers to query common capabilities.
  bool HasUnitBySpec(uint32_t specId) const;
  const UnitDirectory *FindUnitBySpec(uint32_t specId) const;
};
