//
//  ASOHCIIVars.h
//  ASOHCI
//
//  Created by Aleksandr Shabelnikov on 23.08.2025.
//

#ifndef ASOHCIIVars_h
#define ASOHCIIVars_h

#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/OSSharedPtr.h>
#include <PCIDriverKit/IOPCIDevice.h>

// Forward declarations for classes
class ASOHCIPHYAccess;
class ASOHCIARContext;
class ASOHCIATContext;
class ASOHCIARManager;
class ASOHCIATManager;
class ASOHCIIRManager;
class ASOHCIITManager;
class SelfIDManager;
class ConfigROMManager;
class Topology;

// Concrete definition of ASOHCI_IVars matching the .iig ivars struct
struct ASOHCI_IVars {
  // Device / MMIO
  IOPCIDevice *pciDevice = nullptr;
  IOMemoryMap *bar0Map = nullptr;
  uint8_t barIndex = 0;
  IOInterruptDispatchSource *intSource = nullptr;
  IODispatchQueue *defaultQ = nullptr;

  // Interrupt/accounting
  uint64_t interruptCount = 0;
  bool stopping = false;   // Add this for teardown gate
  bool deviceGone = false; // Set when device removal is detected

  // Self-ID DMA resources
  IOBufferMemoryDescriptor *selfIDBuffer = nullptr;
  IODMACommand *selfIDDMA = nullptr;
  IOAddressSegment selfIDSeg = {};
  IOMemoryMap *selfIDMap = nullptr; // CPU mapping

  // Config ROM DMA resources
  IOBufferMemoryDescriptor *configROMBuffer = nullptr; // 1KB ROM image
  IOMemoryMap *configROMMap = nullptr;                 // CPU mapping
  IODMACommand *configROMDMA = nullptr;                // DMA mapping
  IOAddressSegment configROMSeg = {};                  // 32-bit IOVA
  uint32_t configROMHeaderQuad = 0;        // Computed BIB header quadlet
  uint32_t configROMBusOptions = 0;        // Mirror of ROM[2]
  bool configROMHeaderNeedsCommit = false; // Write hdr after next BusReset

  // Link/Bus state flags
  bool cycleTimerArmed = false;
  bool selfIDInProgress = false;
  bool selfIDArmed = false;
  uint32_t collapsedBusResets = 0;
  uint32_t lastLoggedNodeID = 0xFFFFFFFFu;
  bool lastLoggedValid = false;
  bool lastLoggedRoot = false;
  bool didInitialPhyScan = false;
  bool busResetMasked = false;
  uint64_t lastBusResetTime = 0;

  // Cycle inconsistent rate limiting
  uint32_t cycleInconsistentCount = 0;
  uint64_t lastCycleInconsistentTime = 0;

  // PHY access helper
  ASOHCIPHYAccess *phyAccess = nullptr;

  // DMA Contexts (legacy - will be managed by context managers)
  ASOHCIARContext *arRequestContext = nullptr;
  ASOHCIARContext *arResponseContext = nullptr;
  ASOHCIATContext *atRequestContext = nullptr;
  ASOHCIATContext *atResponseContext = nullptr;

  // Context Managers (OHCI 1.1 DMA orchestration) - using OSSharedPtr for
  // automatic lifecycle management
  OSSharedPtr<ASOHCIARManager> arManager;
  OSSharedPtr<ASOHCIATManager> atManager;
  OSSharedPtr<ASOHCIIRManager> irManager;
  OSSharedPtr<ASOHCIITManager> itManager;

  // Managers (factored subsystems) - using OSSharedPtr for automatic lifecycle
  // management
  OSSharedPtr<SelfIDManager> selfIDManager;
  OSSharedPtr<ConfigROMManager> configROMManager;
  OSSharedPtr<Topology> topology;
};

#endif /* ASOHCIIVars_h */