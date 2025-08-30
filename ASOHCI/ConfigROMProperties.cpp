// ConfigROMProperties.cpp

#include "ConfigROMProperties.hpp"

bool ConfigROMProperties::HasUnitBySpec(uint32_t specId) const {
  for (const auto &u : units)
    if (u.specId == specId)
      return true;
  return false;
}

const UnitDirectory *
ConfigROMProperties::FindUnitBySpec(uint32_t specId) const {
  for (const auto &u : units)
    if (u.specId == specId)
      return &u;
  return nullptr;
}
