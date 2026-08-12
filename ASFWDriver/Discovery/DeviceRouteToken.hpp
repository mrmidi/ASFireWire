// SPDX-License-Identifier: Apache-2.0
//
// DeviceRouteToken.hpp
// ASFWDriver - Discovery Layer
//
// Immutable value-type token encapsulating device identity, route epoch, and node ID.
// See documentation:
//   - documentation/TOKEN_BASED_LIFECYCLE.md
//   - ASFWDriver/Service/Lifecycle/RUNTIME_LIFECYCLE_CONTRACT.md
//

#pragma once

#include <cstdint>

#include "DiscoveryTypes.hpp"

namespace ASFW::Discovery {

// A route is valid only for one runtime device instance and one binding of that
// device to a FireWire generation/node.  It is intentionally a value type:
// callers retain it with their own operation state and ask DeviceRegistry to
// validate it before acting on an asynchronous completion.
struct DeviceRouteToken {
    DeviceInstanceId deviceInstanceId{};
    uint64_t routeEpoch{0}; // Changes for every reset, invalidation, and rebind.
    Generation generation{0};
    uint16_t nodeId{kInvalidNodeId};

    constexpr bool operator==(const DeviceRouteToken&) const = default;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return static_cast<bool>(deviceInstanceId) && routeEpoch != 0 &&
               TryOperationalNodeId(nodeId).has_value();
    }
};

} // namespace ASFW::Discovery
