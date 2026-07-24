#pragma once

#include <cstdint>

#include "DiscoveryTypes.hpp"

namespace ASFW::Discovery {

// A route is valid only for one discovered device incarnation and one binding of
// that device to a FireWire generation/node.  It is intentionally a value type:
// callers retain it with their own operation state and ask DeviceRegistry to
// validate it before acting on an asynchronous completion.
struct DeviceRouteToken {
    Guid64 guid{0};
    uint64_t deviceIncarnation{0}; // Changes only after removal/replacement.
    uint64_t routeEpoch{0};        // Changes for every reset, invalidation, and rebind.
    Generation generation{0};
    uint16_t nodeId{kInvalidNodeId};

    constexpr bool operator==(const DeviceRouteToken&) const = default;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return guid != 0 && deviceIncarnation != 0 && routeEpoch != 0 &&
               TryOperationalNodeId(nodeId).has_value();
    }
};

} // namespace ASFW::Discovery
