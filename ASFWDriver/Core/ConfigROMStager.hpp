#pragma once

#include <DriverKit/IOReturn.h>
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOMemoryMap.h>

#include "ConfigROMBuilder.hpp"

namespace ASFW::Driver {

class HardwareInterface;

// DriverKit-facing helper that maps the generated Config ROM image into device
// visible memory, programs ConfigROMMap and asserts BIBimageValid. Split from
// ConfigROMBuilder so the pure assembly logic stays host-testable.
class ConfigROMStager {
public:
    ConfigROMStager();
    ~ConfigROMStager();

    kern_return_t Prepare(HardwareInterface& hw, size_t romBytes = ConfigROMBuilder::kConfigROMSize);
    kern_return_t StageImage(const ConfigROMBuilder& image, HardwareInterface& hw);
    void Teardown(HardwareInterface& hw);
    void RestoreHeaderAfterBusReset();  // Called after bus reset to restore header in DMA buffer

    bool Ready() const { return prepared_; }

    // Expose expected register values (from last staged image)
    uint32_t ExpectedHeader() const { return savedHeader_; }
    uint32_t ExpectedBusOptions() const { return savedBusOptions_; }

private:
    kern_return_t EnsurePrepared(HardwareInterface& hw);
    void ZeroBuffer();

    OSSharedPtr<IOBufferMemoryDescriptor> buffer_;
    OSSharedPtr<IOMemoryMap> map_;
    OSSharedPtr<IODMACommand> dma_;
    IOAddressSegment segment_{};
    uint64_t dmaFlags_{0};
    bool prepared_{false};
    uint32_t savedHeader_{0};      // Saved header quadlet (zeroed in DMA buffer during staging)
    uint32_t savedBusOptions_{0};  // Saved BusOptions quadlet for restoration after bus reset
};

} // namespace ASFW::Driver
