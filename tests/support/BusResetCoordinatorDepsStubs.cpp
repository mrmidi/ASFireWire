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

namespace ASFW::Testing {

// Counts ROMScanner::Abort calls made by the coordinator under test.
int gRomScannerAbortCalls = 0;

} // namespace ASFW::Testing

namespace ASFW::Discovery {

ROMScanner::ROMScanner(Async::IFireWireBus& bus, SpeedPolicy& speedPolicy,
                       const ROMScannerParams& params, OSSharedPtr<IODispatchQueue> dispatchQueue)
    : bus_(bus), speedPolicy_(speedPolicy), params_(params),
      dispatchQueue_(std::move(dispatchQueue)) {}

ROMScanner::~ROMScanner() = default;

bool ROMScanner::Abort() {
    ++ASFW::Testing::gRomScannerAbortCalls;
    return true;
}

} // namespace ASFW::Discovery
