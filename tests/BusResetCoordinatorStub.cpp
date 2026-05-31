#include "Bus/BusResetCoordinator.hpp"

namespace ASFW::Driver {

uint64_t BusResetCoordinator::MonotonicNow() {
    return ASFW::Testing::HostMonotonicNow();
}

} // namespace ASFW::Driver
