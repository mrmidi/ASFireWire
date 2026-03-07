#include "ASFWDriver/ConfigROM/ConfigROMStager.hpp"
#include "ASFWDriver/ConfigROM/ROMScanner.hpp"

namespace ASFW::Driver {

ConfigROMStager::ConfigROMStager() = default;
ConfigROMStager::~ConfigROMStager() = default;

kern_return_t ConfigROMStager::Prepare(HardwareInterface&, size_t) { return kIOReturnSuccess; }

kern_return_t ConfigROMStager::StageImage(const ConfigROMBuilder&, HardwareInterface&) {
    return kIOReturnSuccess;
}

void ConfigROMStager::Teardown(HardwareInterface&) {}

void ConfigROMStager::RestoreHeaderAfterBusReset() {}

} // namespace ASFW::Driver

namespace ASFW::Discovery {

void ROMScanner::Abort(Generation) {}

} // namespace ASFW::Discovery
