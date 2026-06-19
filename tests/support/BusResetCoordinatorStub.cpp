#include "Bus/BusResetCoordinator.hpp"

namespace ASFW::Driver {

uint64_t BusResetCoordinator::MonotonicNow() noexcept {
    return ASFW::Testing::HostMonotonicNow();
}

} // namespace ASFW::Driver
