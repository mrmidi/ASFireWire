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
// class ASOHCIATContext;  // Removed - not used
class ASOHCIARManager;
class ASOHCIATManager;
class ASOHCIIRManager;
class ASOHCIITManager;
class SelfIDManager;
class ConfigROMManager;
class Topology;
class ASOHCIInterruptRouter;
class ASOHCIRegisterIO;

// State machine for ASOHCI driver lifecycle (REFACTOR.md §9)
enum class ASOHCIState {
  Stopped,   // Initial state, no resources allocated
  Starting,  // In the process of starting up
  Running,   // Fully operational, accepting requests
  Quiescing, // In the process of shutting down
  Dead       // Terminal state, cleanup complete
};

// Concrete definition of ASOHCI_IVars matching the .iig ivars struct
struct ASOHCI_IVars {
  // Device / MMIO
  OSSharedPtr<IOPCIDevice> pciDevice;
  IOMemoryMap *bar0Map = nullptr;
  uint8_t barIndex = 0;
  OSSharedPtr<IOInterruptDispatchSource> intSource;
  OSSharedPtr<IODispatchQueue> defaultQ;

  // State machine (REFACTOR.md §9)
  alignas(4) uint32_t state = static_cast<uint32_t>(ASOHCIState::Stopped);
  char stateDescription[32] = "Stopped"; // Human-readable state for logging

  // Interrupt/accounting
  uint64_t interruptCount = 0;
  bool stopping = false;   // Add this for teardown gate
  bool deviceGone = false; // Set when device removal is detected

  // Self-ID DMA resources
  OSSharedPtr<IOBufferMemoryDescriptor> selfIDBuffer;
  OSSharedPtr<IODMACommand> selfIDDMA;
  IOAddressSegment selfIDSeg = {};
  OSSharedPtr<IOMemoryMap> selfIDMap; // CPU mapping

  // Config ROM DMA resources
  OSSharedPtr<IOBufferMemoryDescriptor> configROMBuffer; // 1KB ROM image
  OSSharedPtr<IOMemoryMap> configROMMap;                 // CPU mapping
  OSSharedPtr<IODMACommand> configROMDMA;                // DMA mapping
  IOAddressSegment configROMSeg = {};                    // 32-bit IOVA
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
  OSSharedPtr<ASOHCIPHYAccess> phyAccess;

  // DMA Contexts (legacy - will be managed by context managers)
  OSSharedPtr<ASOHCIARContext> arRequestContext;
  OSSharedPtr<ASOHCIARContext> arResponseContext;
  // OSSharedPtr<ASOHCIATContext> atRequestContext;  // Removed - not used
  // OSSharedPtr<ASOHCIATContext> atResponseContext;  // Removed - not used

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

  // Interrupt fan-out
  OSSharedPtr<ASOHCIInterruptRouter> interruptRouter;

  // Register IO helper
  OSSharedPtr<ASOHCIRegisterIO> regs;
};

#endif /* ASOHCIIVars_h */
