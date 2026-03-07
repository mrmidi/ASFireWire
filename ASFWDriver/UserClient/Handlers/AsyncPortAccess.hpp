#pragma once

#include "../../Async/Interfaces/IAsyncSubsystemPort.hpp"

namespace ASFW::UserClient {

/**
 * @brief Recover the async subsystem port from the DriverKit-local driver bridge.
 *
 * The IIG boundary exposes the dependency as `void*`; this helper centralizes
 * the cast so handler code stays typed.
 */
template <typename DriverLike>
[[nodiscard]] inline ASFW::Async::IAsyncSubsystemPort*
GetAsyncSubsystemPort(DriverLike* driver) noexcept {
    if (driver == nullptr) {
        return nullptr;
    }
    return static_cast<ASFW::Async::IAsyncSubsystemPort*>(driver->GetAsyncSubsystem());
}

} // namespace ASFW::UserClient
