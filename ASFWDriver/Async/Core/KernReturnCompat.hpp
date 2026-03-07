#pragma once

#include <cstdint>

#ifdef __DRIVERKIT__
#include <DriverKit/IOReturn.h>
using kr_t = kern_return_t;
#else
#include <DriverKit/IOReturn.h>
using kr_t = IOReturn;
#endif

namespace ASFW::Async {

/**
 * @brief Minimal kern-return compatibility helpers.
 *
 * This header exists only to smooth over DriverKit vs. host-test type names.
 * It is intentionally not the project's primary error model.
 */
[[nodiscard]] constexpr bool KRSucceeded(kr_t status) noexcept {
    return status == kIOReturnSuccess;
}

[[nodiscard]] constexpr bool KRFailed(kr_t status) noexcept { return !KRSucceeded(status); }

} // namespace ASFW::Async
