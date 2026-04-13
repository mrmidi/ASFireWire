#pragma once

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/IOReturn.h>
#include <DriverKit/OSSharedPtr.h>
#endif

#include "ConfigROMBuilder.hpp"

namespace ASFW::Driver {

class HardwareInterface;

/**
 * @class ConfigROMStager
 * @brief DriverKit-facing helper that stages a Config ROM image for OHCI.
 *
 * Stages the ROM into a 1 KiB-aligned DMA buffer and programs the OHCI "shadow"
 * Config ROM registers so the new image becomes active on the next bus reset
 * (OHCI 1.1 §5.5.6).
 *
 * ### OHCI Shadow ROM Mechanism
 * OHCI uses a shadow mechanism for `ConfigROMmap`: software stages the "next" value
 * before enabling the link, and the controller latches it when `linkEnable` is 0
 * or immediately after a bus reset.
 *
 * ### Atomic Update Procedure
 * To safely update the ROM, this class follows the OHCI 1.1 §5.5.6 procedure:
 * - Prepare the new image in a 1 KB-aligned DMA buffer.
 * - Program `BusOptions`, `GUIDHi`, `GUIDLo`, and `ConfigROMmap`.
 * - Write `ConfigROMheader` last to complete staging.
 *
 * After a bus reset, BusResetCoordinator restores `BusOptions` and `ConfigROMheader`.
 */
class ConfigROMStager {
  public:
    ConfigROMStager();
    ~ConfigROMStager();

    /**
     * @brief Prepares the required DMA buffer for the Config ROM.
     * @param hw Hardware interface for creating the DMA command.
     * @param romBytes Size of the buffer to allocate (default 1024 bytes per IEEE 1394-1995).
     * @return kIOReturnSuccess on success.
     */
    kern_return_t Prepare(HardwareInterface& hw,
                          size_t romBytes = ConfigROMBuilder::kConfigROMSize);

    /**
     * @brief Stages the prepared ROM image into OHCI memory.
     *
     * Copies the native byte-order ROM image to the DMA buffer, and writes the
     * shadow registers (BusOptions, GUIDHi, GUIDLo, ConfigROMmap). Finally, it
     * writes to ConfigROMheader to activate the new ROM on the next bus reset.
     *
     * @param image The configured and finalized ConfigROMBuilder instance.
     * @param hw Hardware interface to execute register writes.
     * @return kIOReturnSuccess on success.
     */
    kern_return_t StageImage(const ConfigROMBuilder& image, HardwareInterface& hw);

    /**
     * @brief Clears ConfigROMMap and unmaps DMA resources.
     */
    void Teardown(HardwareInterface& hw);

    /**
     * @brief Restores the first quadlet of the DMA buffer.
     *
     * Called after a bus reset to restore the header quadlet in the DMA buffer.
     * The staging path temporarily clears this quadlet as part of the standard
     * OHCI/Linux staging sequence.
     */
    void RestoreHeaderAfterBusReset();

    /** @brief Returns true if the DMA buffer has been successfully prepared. */
    [[nodiscard]] bool Ready() const noexcept { return prepared_; }

    /** @brief Returns the expected header quadlet from the last staged image. */
    [[nodiscard]] uint32_t ExpectedHeader() const noexcept { return savedHeader_; }
    /** @brief Returns the expected BusOptions quadlet from the last staged image. */
    [[nodiscard]] uint32_t ExpectedBusOptions() const noexcept { return savedBusOptions_; }

  private:
    kern_return_t EnsurePrepared(HardwareInterface& hw);
    void ZeroBuffer();

    OSSharedPtr<IOBufferMemoryDescriptor> buffer_;
    OSSharedPtr<IOMemoryMap> map_;
    OSSharedPtr<IODMACommand> dma_;
    IOAddressSegment segment_{};
    uint64_t dmaFlags_{0};
    bool prepared_{false};
    bool guidWritten_{false};
    uint32_t savedHeader_{0};     // Saved header quadlet (zeroed in DMA buffer during staging)
    uint32_t savedBusOptions_{0}; // Saved BusOptions quadlet for restoration after bus reset
};

} // namespace ASFW::Driver
