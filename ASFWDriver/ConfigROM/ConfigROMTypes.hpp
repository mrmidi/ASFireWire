#pragma once

#include <cstdint>
#include <string_view>
#include "../Common/FWCommon.hpp"

namespace ASFW::Driver {

// Handle identifying a created text leaf (allow future introspection)
struct LeafHandle {
    uint16_t offsetQuadlets = 0; // Quadlet offset from start of image to leaf header
    bool valid() const { return offsetQuadlets != 0; }
};

} // namespace ASFW::Driver
