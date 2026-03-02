#pragma once

#include "../Common/FWCommon.hpp"
#include <cstdint>
#include <string_view>

namespace ASFW::Driver {

// Handle identifying a created text leaf (allow future introspection)
struct LeafHandle {
    uint16_t offsetQuadlets = 0; // Quadlet offset from start of image to leaf header
    [[nodiscard]] bool valid() const noexcept { return offsetQuadlets != 0; }
};

} // namespace ASFW::Driver
