//
//  ASOHCI.cpp
//  ASOHCI
//
//  Created by Aleksandr Shabelnikov on 23.08.2025.
//

#include <os/log.h>

#include <DriverKit/IOUserServer.h>
#include <DriverKit/IOLib.h>
#include <PCIDriverKit/IOPCIDevice.h>

#include "ASOHCI.h"

// PCI Configuration offsets
#define kIOPCIConfigurationOffsetVendorID         0x00
#define kIOPCIConfigurationOffsetDeviceID         0x02
#define kIOPCIConfigurationOffsetCommand          0x04

// PCI Command register bits
#define kIOPCICommandMemorySpace                  0x0002
#define kIOPCICommandBusMaster                    0x0004

kern_return_t
IMPL(ASOHCI, Start)
{
    kern_return_t kr = Start(provider, SUPERDISPATCH);
    if (kr != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "ASOHCI: Start superdispatch failed: 0x%08x", kr);
        return kr;
    }
    
    os_log(OS_LOG_DEFAULT, "ASOHCI: Starting driver initialization");
    
    // Cast provider to IOPCIDevice
    auto pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) {
        os_log(OS_LOG_DEFAULT, "ASOHCI: Provider is not IOPCIDevice");
        return kIOReturnBadArgument;
    }
    
    // Take exclusive ownership of the PCI device
    kr = pci->Open(this, 0);
    if (kr != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "ASOHCI: Failed to open PCI device: 0x%08x", kr);
        return kr;
    }
    
    os_log(OS_LOG_DEFAULT, "ASOHCI: Successfully opened PCI device");
    
    // Read PCI device/vendor IDs for verification
    uint16_t vendorID = 0, deviceID = 0;
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetVendorID, &vendorID);
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetDeviceID, &deviceID);
    
    os_log(OS_LOG_DEFAULT, "ASOHCI: PCI Device - Vendor: 0x%04x, Device: 0x%04x", vendorID, deviceID);
    
    // Enable Bus Master and Memory Space
    uint16_t command = 0;
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &command);
    os_log(OS_LOG_DEFAULT, "ASOHCI: Current PCI command register: 0x%04x", command);
    
    command |= (kIOPCICommandBusMaster | kIOPCICommandMemorySpace);
    pci->ConfigurationWrite16(kIOPCIConfigurationOffsetCommand, command);
    
    // Verify the command was written
    uint16_t newCommand = 0;
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &newCommand);
    os_log(OS_LOG_DEFAULT, "ASOHCI: Updated PCI command register: 0x%04x", newCommand);
    
    // Log BAR information for debugging
    for (uint8_t i = 0; i < 6; i++) {
        uint8_t memoryIndex = 0;
        uint64_t barSize = 0;
        uint8_t barType = 0;
        kr = pci->GetBARInfo(i, &memoryIndex, &barSize, &barType);
        if (kr == kIOReturnSuccess && barSize > 0) {
            os_log(OS_LOG_DEFAULT, "ASOHCI: BAR%d - MemoryIndex: %d, Size: 0x%llx, Type: 0x%02x", i, memoryIndex, barSize, barType);
        }
    }
    
    // Read OHCI Version register (offset 0x000) via BAR0 (memoryIndex 0)
    uint32_t ohci_version = 0;
    pci->MemoryRead32(0, 0x000, &ohci_version);
    os_log(OS_LOG_DEFAULT, "ASOHCI: OHCI Version register: 0x%08x", ohci_version);
    os_log(OS_LOG_DEFAULT, "ASOHCI: OHCI Version: %d.%d", 
           (ohci_version >> 16) & 0xFF, (ohci_version >> 4) & 0x0F);
    
    // Read additional OHCI registers for verification
    uint32_t bus_options = 0;
    pci->MemoryRead32(0, 0x020, &bus_options);  // OHCI1394_BusOptions
    os_log(OS_LOG_DEFAULT, "ASOHCI: Bus Options register: 0x%08x", bus_options);
    
    uint32_t guid_hi = 0, guid_lo = 0;
    pci->MemoryRead32(0, 0x024, &guid_hi);  // OHCI1394_GUIDHi  
    pci->MemoryRead32(0, 0x028, &guid_lo);  // OHCI1394_GUIDLo
    os_log(OS_LOG_DEFAULT, "ASOHCI: GUID: %08x:%08x", guid_hi, guid_lo);
    
    os_log(OS_LOG_DEFAULT, "ASOHCI: Driver initialization completed successfully");
    
    return kIOReturnSuccess;
}

kern_return_t
IMPL(ASOHCI, Stop)
{
    os_log(OS_LOG_DEFAULT, "ASOHCI: Stopping driver");
    
    // Cast provider to IOPCIDevice
    auto pci = OSDynamicCast(IOPCIDevice, provider);
    if (pci) {
        // Disable Bus Master and Memory Space
        uint16_t command = 0;
        pci->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &command);
        command &= ~(kIOPCICommandBusMaster | kIOPCICommandMemorySpace);
        pci->ConfigurationWrite16(kIOPCIConfigurationOffsetCommand, command);
        
        os_log(OS_LOG_DEFAULT, "ASOHCI: Disabled PCI command flags");
        
        // Close PCI device
        pci->Close(this, 0);
        
        os_log(OS_LOG_DEFAULT, "ASOHCI: Closed PCI device");
    }
    
    kern_return_t kr = Stop(provider, SUPERDISPATCH);
    os_log(OS_LOG_DEFAULT, "ASOHCI: Driver stopped, result: 0x%08x", kr);
    
    return kr;
}
