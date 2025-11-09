#include "HardwareInterface.hpp"
#include "RegisterMap.hpp"

// Minimal stub implementation for unit tests
namespace ASFW::Driver {

void HardwareInterface::Write(Register32, uint32_t) noexcept {
    // Stub - no-op for tests
}

}
