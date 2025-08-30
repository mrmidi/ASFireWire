//
// TopologyTypes.hpp
// Small shared enums/IDs for the topology model
//

#pragma once

#include <stdint.h>

// Node index on this bus cycle (0..62). Not stable across resets.
struct NodeId {
  uint8_t value = 0xFF;
  constexpr bool valid() const { return value != 0xFF; }
  constexpr bool operator==(NodeId o) const { return value == o.value; }
  constexpr bool operator!=(NodeId o) const { return value != o.value; }
};

// Physical ID reported in self-ID (0..63).
struct PhyId {
  uint8_t value = 0xFF;
  constexpr bool valid() const { return value != 0xFF; }
  constexpr bool operator==(PhyId o) const { return value == o.value; }
  constexpr bool operator!=(PhyId o) const { return value != o.value; }
};

// Alias the Port code used by topology (kept identical to SelfID::PortCode).
enum class PortState : uint8_t {
  NotPresent = 0,
  NotActive = 1,
  Parent = 2,
  Child = 3
};
