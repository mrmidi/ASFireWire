#pragma once
#include "IFireWireBusOps.hpp"
#include "IFireWireBusInfo.hpp"

namespace ASFW::Async {

/**
 * @brief Combined FireWire bus interface (ops + info).
 *
 * Most consumers only need this interface. Use separate interfaces for:
 * - Mocking: Mock only IFireWireBusOps if you don't care about topology queries
 * - Testing: Fake only IFireWireBusInfo if you're testing speed/hop calculations
 */
class IFireWireBus : public IFireWireBusOps, public IFireWireBusInfo {
public:
    virtual ~IFireWireBus() = default;
};

} // namespace ASFW::Async
