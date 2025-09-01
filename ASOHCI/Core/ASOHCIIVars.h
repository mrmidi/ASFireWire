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
// #include <DriverKit/IOMemoryMap.h> // Not used in DriverKit - using direct
// memory access
#include <DriverKit/OSSharedPtr.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <memory>

// Include shared types
#include "../ASOHCIDriverTypes.hpp"

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
class ASOHCIInterruptRouter;
class ASOHCIRegisterIO;

// Forward declarations for new RAII architecture
namespace fw {
class LinkHandle;
class ASFireWireController;
} // namespace fw

// Concrete definition of ASOHCI_IVars matching the .iig ivars struct
struct ASOHCI_IVars {
  // Device / MMIO
  OSSharedPtr<IOPCIDevice> pciDevice;
  // bar0Map not used in DriverKit - using direct memory access via
  // MemoryRead32/MemoryWrite32
  uint8_t barIndex = 0;
  OSSharedPtr<IOInterruptDispatchSource> intSource;
  OSSharedPtr<IODispatchQueue> defaultQ;

  // Interrupt/accounting
  uint64_t interruptCount = 0;
  bool stopping = false;   // Add this for teardown gate
  bool deviceGone = false; // Set when device removal is detected

  // State machine (REFACTOR.md §9) - stored as uint32_t for atomic operations
  uint32_t state = static_cast<uint32_t>(ASOHCIState::Stopped);
  char stateDescription[32] = "Stopped";

  // Self-ID DMA resources - smart pointers for automatic cleanup
  OSSharedPtr<IOBufferMemoryDescriptor> selfIDBuffer;
  OSSharedPtr<IODMACommand> selfIDDMA;
  IOAddressSegment selfIDSeg = {};
  OSSharedPtr<IOMemoryMap> selfIDMap; // CPU mapping

  // Config ROM DMA resources - smart pointers for automatic cleanup
  OSSharedPtr<IOBufferMemoryDescriptor> configROMBuffer; // 1KB ROM image
  OSSharedPtr<IOMemoryMap> configROMMap;                 // CPU mapping
  OSSharedPtr<IODMACommand> configROMDMA;                // DMA mapping
  IOAddressSegment configROMSeg = {};                    // 32-bit IOVA
  uint32_t configROMHeaderQuad = 0;        // Computed BIB header quadlet
  uint32_t configROMBusOptions = 0;        // Mirror of ROM[2]
  bool configROMHeaderNeedsCommit = false; // Write hdr after next BusReset

  // Link/Bus state flags
  uint32_t generation = 0; // Current bus generation
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

  // PHY access helper - smart pointer for automatic cleanup
  std::unique_ptr<ASOHCIPHYAccess> phyAccess;

  // DMA Contexts (legacy - will be managed by context managers) - using
  // std::unique_ptr for exclusive ownership of non-OSObject types
  std::unique_ptr<ASOHCIARContext> arRequestContext;
  std::unique_ptr<ASOHCIARContext> arResponseContext;
  std::unique_ptr<ASOHCIATContext> atRequestContext;
  std::unique_ptr<ASOHCIATContext> atResponseContext;

  // Context Managers (OHCI 1.1 DMA orchestration) - using std::unique_ptr
  // for exclusive ownership of non-OSObject types
  std::unique_ptr<ASOHCIARManager> arManager;
  std::unique_ptr<ASOHCIATManager> atManager;
  std::unique_ptr<ASOHCIIRManager> irManager;
  std::unique_ptr<ASOHCIITManager> itManager;

  // Managers (factored subsystems) - using std::unique_ptr for exclusive
  // ownership of non-OSObject types
  std::unique_ptr<SelfIDManager> selfIDManager;
  std::unique_ptr<ConfigROMManager> configROMManager;
  std::unique_ptr<Topology> topology;

  // Interrupt fan-out
  OSSharedPtr<ASOHCIInterruptRouter> interruptRouter;

  // Register IO helper
  OSSharedPtr<ASOHCIRegisterIO> regs;

  // RAII Architecture Components (start_that_worked.txt design)
  std::shared_ptr<fw::LinkHandle>
      linkHandle; // Link adapter for ILink interface
  std::shared_ptr<fw::ASFireWireController> controller; // Plain C++ controller

  // Link API callbacks
  // void (*selfIDCallback)(void* context) = nullptr;
  // void* selfIDCallbackContext = nullptr;
  // void (*busResetCallback)(void* context) = nullptr;
  // void* busResetCallbackContext = nullptr;
};

#endif /* ASOHCIIVars_h */
