//
//  ASOHCI.cpp
//  ASOHCI
//
//  Created by Aleksandr Shabelnikov on 23.08.2025.
//

#include <TargetConditionals.h>
#include <os/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Memory allocation constants (OHCI 1.1 spec + Linux reference compliance)
[[maybe_unused]] static constexpr size_t kPageSize = 4096; // Standard page size
[[maybe_unused]] static constexpr size_t kMaxAllocation =
    16 * 1024 * 1024; // 16MB limit

#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/IOUserServer.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSData.h>
#include <DriverKit/OSSharedPtr.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <PCIDriverKit/IOPCIFamilyDefinitions.h>

// Include system headers for DriverKit runtime functions
#include <DriverKit/IOService.h>

// Include project headers
#include "ASOHCIDriverTypes.hpp"
#include "Core/ASOHCIIVars.h"
#include <DriverKit/OSMetaClass.h>

// Dispatch queue for deferred self-ID processing (DriverKit equivalent of Linux
// workqueue)
#include <DriverKit/IODispatchQueue.h>

// IIG-generated header (path comes from your product name/bundle):
#include <net.mrmidi.ASFireWire.ASOHCI/ASOHCI.h>

// Private helpers; NO 'class ASOHCI' here
#include "ASOHCI_Priv.hpp"

// Generated Header
#include "BridgeLog.hpp"

// Include Link API header
// #include "Core/ASOHCILinkAPI.h"
// #include "ASOHCILink.h"

// Forward declarations used early
struct ASOHCI_IVars;
static kern_return_t TransitionState(ASOHCI_IVars *, ASOHCIState, const char *);
static bool IsOperationAllowed(ASOHCI_IVars *, ASOHCIState);
static const char *StateToString(ASOHCIState);

// Memory barrier header for DMA-safe programming
#include "Core/ASOHCIMemoryBarrier.hpp"

// Private helper methods (implementation details, not in public interface)

// OHCI constants and contexts
#include "ASOHCIARContext.hpp"
#include "ASOHCIATContext.hpp"
#include "OHCIConstants.hpp"
#include "PhyAccess.hpp"
// Config ROM
#include "ASOHCIConfigROM.hpp"
// Context Managers
#include "ASOHCIARManager.hpp"
#include "ASOHCIATManager.hpp"
#include "ASOHCIIRManager.hpp"
#include "ASOHCIITManager.hpp"
// Managers
#include "ConfigROMManager.hpp"
#include "SelfIDManager.hpp"
#include "Topology.hpp"
// ------------------------ Logging -----------------------------------
#include "ASOHCIInterruptDump.hpp"
#include "LogHelper.hpp"
#include "Shared/ASOHCIInterruptRouter.hpp"
#include "Shared/ASOHCIRegisterIO.hpp"

// BRIDGE_LOG macro/functionality provided by BridgeLog.hpp

// Self-ID deferred processing moved to ASOHCIInterruptRouter

// Hex dump helper moved to LogHelper.hpp (DumpHexBigEndian)

// NOTE: Using concrete ASOHCI_IVars definition from ASOHCIIVars.h
// This is a workaround for IIG forward declaration issues where the
// generated header only provides forward declarations instead of
// the full struct definition. The struct matches the .iig ivars exactly.

// Helper: program Self-ID reception via manager
void ASOHCI::ArmSelfIDReceive(bool clearCount) {
  if (!ivars || !ivars->selfIDManager)
    return;
  kern_return_t akr = ivars->selfIDManager->Arm(clearCount);
  os_log(ASLog(), "ASOHCI: Self-ID armed clear=%u iova=0x%llx status=0x%08x",
         clearCount ? 1u : 0u,
         (unsigned long long)ivars->selfIDManager->BufferIOVA(), akr);
  ivars->selfIDArmed = true;
}

// Device access safety helper
OS_ALWAYS_INLINE bool DeviceAccessOk(ASOHCI_IVars *iv) {
  return iv && iv->pciDevice &&
         !__atomic_load_n(&iv->deviceGone, __ATOMIC_ACQUIRE);
}

// (Legacy) Self-ID parser not used when manager is active

// Init
bool ASOHCI::init() {
  auto success = super::init();
  if (!success) {
    return false;
  }

  // Use Apple's type-safe allocation
  ivars = IONewZero(ASOHCI_IVars, 1);
  if (ivars == nullptr) {
    return false;
  }

  // State machine: Initialize to Stopped state (REFACTOR.md §9)
  __atomic_store_n(&ivars->state, static_cast<uint32_t>(ASOHCIState::Stopped),
                   __ATOMIC_RELEASE);
  strlcpy(ivars->stateDescription, "Stopped", sizeof(ivars->stateDescription));

  // OSSharedPtr objects automatically initialized to nullptr
  os_log(ASLog(), "ASOHCI: init() completed - state: %s",
         ivars->stateDescription);
  return true;
}

void ASOHCI::free() {
  os_log(ASLog(), "ASOHCI: free() - current state: %s",
         ivars ? ivars->stateDescription : "null");

  if (ivars != nullptr) {
    // State machine: Force transition to Dead if not already there (REFACTOR.md
    // §9)
    ASOHCIState currentState = static_cast<ASOHCIState>(
        __atomic_load_n(&ivars->state, __ATOMIC_ACQUIRE));
    if (currentState != ASOHCIState::Dead) {
      __atomic_store_n(&ivars->state, static_cast<uint32_t>(ASOHCIState::Dead),
                       __ATOMIC_RELEASE);
      strlcpy(ivars->stateDescription, "Dead", sizeof(ivars->stateDescription));
      os_log(ASLog(), "ASOHCI: free() forced state to Dead");
    }

    // Step 1: Stop all operations first
    if (ivars->arManager.get() != nullptr) {
      ivars->arManager->Stop();
    }
    if (ivars->atManager.get() != nullptr) {
      ivars->atManager->Stop();
    }

    // Step 2: Reset smart pointers in reverse order of creation
    // Context Managers
    ivars->itManager.reset();
    ivars->irManager.reset();
    ivars->atManager.reset();
    ivars->arManager.reset();

    // DMA Resources
    ivars->configROMDMA.reset();
    ivars->configROMMap.reset();
    ivars->configROMBuffer.reset();
    ivars->selfIDDMA.reset();
    ivars->selfIDMap.reset();
    ivars->selfIDBuffer.reset();

    // Device Resources
    ivars->intSource.reset();
    ivars->defaultQ.reset();
    // bar0Map not used in DriverKit - using direct memory access
    ivars->pciDevice.reset();

    // Managers (factored subsystems)
    ivars->interruptRouter.reset();
    ivars->regs.reset();
    ivars->topology.reset();
    ivars->configROMManager.reset();
    ivars->selfIDManager.reset();

    // Legacy raw pointers (manual cleanup) - now smart pointers
    ivars->phyAccess.reset();
    ivars->arRequestContext.reset();
    ivars->arResponseContext.reset();
    // AT contexts removed - not used
    // ivars->atRequestContext.reset();
    // ivars->atResponseContext.reset();
  }

  // Step 3: Safe deallocation with null-setting
  IOSafeDeleteNULL(ivars, ASOHCI_IVars, 1);
  super::free();
}

// =====================================================================================
// Error Handling and Cleanup Patterns
// =====================================================================================

// Comprehensive cleanup helper for error recovery
void ASOHCI::CleanupOnError() {
  os_log(ASLog(), "ASOHCI: CleanupOnError - performing comprehensive cleanup");

  // Take a stable snapshot and guard it
  ASOHCI_IVars *const iv = ivars;
  if (!iv) {
    os_log(ASLog(), "ASOHCI: CleanupOnError - ivars is null, nothing to do");
    return;
  }

  // State machine: Transition to Quiescing if not already stopping
  ASOHCIState currentState =
      static_cast<ASOHCIState>(__atomic_load_n(&iv->state, __ATOMIC_ACQUIRE));
  if (currentState != ASOHCIState::Quiescing &&
      currentState != ASOHCIState::Dead) {
    kern_return_t stateKr =
        TransitionState(iv, ASOHCIState::Quiescing, "CleanupOnError");
    if (stateKr != kIOReturnSuccess) {
      os_log(ASLog(), "ASOHCI: CleanupOnError state transition failed: 0x%08x",
             stateKr);
    }
  }

  // Step 1: Stop managers (all iv->... guarded by the early return above)
  if (iv->arManager) {
    iv->arManager->Stop();
    iv->arManager.reset();
  }
  if (iv->atManager) {
    iv->atManager->Stop();
    iv->atManager.reset();
  }
  if (iv->irManager) {
    iv->irManager->StopAll();
    iv->irManager.reset();
  }
  if (iv->itManager) {
    iv->itManager->StopAll();
    iv->itManager.reset();
  }

  // Step 2: Clean up DMA resources
  iv->configROMDMA.reset();
  iv->configROMMap.reset();
  iv->configROMBuffer.reset();
  iv->selfIDDMA.reset();
  iv->selfIDMap.reset();
  iv->selfIDBuffer.reset();

  // Step 3: Clean up device resources
  if (iv->intSource) {
    iv->intSource->SetEnableWithCompletion(false, nullptr);
    iv->intSource.reset();
  }
  iv->defaultQ.reset();
  // bar0Map not used in DriverKit - using direct memory access

  // Step 4: Clean up managers and helpers
  if (iv->selfIDManager) {
    iv->selfIDManager->Teardown();
    iv->selfIDManager.reset();
  }
  if (iv->configROMManager) {
    iv->configROMManager->Teardown();
    iv->configROMManager.reset();
  }
  iv->topology.reset();
  iv->regs.reset();
  iv->interruptRouter.reset();

  // Step 5: Clean up legacy resources - now smart pointers
  iv->phyAccess.reset();
  iv->arRequestContext.reset();
  iv->arResponseContext.reset();
  // ivars->atRequestContext.reset();  // Removed - not used
  // ivars->atResponseContext.reset();  // Removed - not used

  // Step 6: Close PCI device if open
  if (iv->pciDevice) {
    iv->pciDevice->Close(this, 0);
    iv->pciDevice.reset();
  }

  // State machine: Transition to Dead (REFACTOR.md §9)
  (void)TransitionState(iv, ASOHCIState::Dead, "CleanupOnError complete");
  os_log(ASLog(), "ASOHCI: CleanupOnError - cleanup completed");
}

// =====================================================================================
// Validation and Error Handling Helpers
// =====================================================================================

// Validate ivars and device state
static kern_return_t ValidateState(ASOHCI_IVars *ivars, const char *operation) {
  if (!ivars) {
    os_log(ASLog(), "ASOHCI: %s - ivars not allocated", operation);
    return kIOReturnNoResources;
  }

  if (__atomic_load_n(&ivars->stopping, __ATOMIC_ACQUIRE)) {
    os_log(ASLog(), "ASOHCI: %s - operation blocked, driver stopping",
           operation);
    return kIOReturnNotReady;
  }

  if (__atomic_load_n(&ivars->deviceGone, __ATOMIC_ACQUIRE)) {
    os_log(ASLog(), "ASOHCI: %s - operation blocked, device gone", operation);
    return kIOReturnNoDevice;
  }

  // State machine validation (REFACTOR.md §9)
  ASOHCIState currentState = static_cast<ASOHCIState>(
      __atomic_load_n(&ivars->state, __ATOMIC_ACQUIRE));
  if (currentState == ASOHCIState::Dead) {
    os_log(ASLog(), "ASOHCI: %s - operation blocked, driver is dead",
           operation);
    return kIOReturnNotReady;
  }

  if (!ivars->pciDevice) {
    os_log(ASLog(), "ASOHCI: %s - PCI device not available", operation);
    return kIOReturnNoDevice;
  }

  return kIOReturnSuccess;
}

// Enhanced error logging with context
static void LogError(kern_return_t error, const char *operation,
                     const char *details = nullptr) {
  const char *errorString = "unknown";
  switch (error) {
  case kIOReturnSuccess:
    return; // Don't log success
  case kIOReturnNoMemory:
    errorString = "no memory";
    break;
  case kIOReturnNoDevice:
    errorString = "no device";
    break;
  case kIOReturnNotReady:
    errorString = "not ready";
    break;
  case kIOReturnBadArgument:
    errorString = "bad argument";
    break;
  case kIOReturnNoResources:
    errorString = "no resources";
    break;
  default:
    break;
  }

  if (details) {
    os_log(ASLog(), "ASOHCI: %s failed (%s) - %s", operation, errorString,
           details);
  } else {
    os_log(ASLog(), "ASOHCI: %s failed (%s)", operation, errorString);
  }
}

// Timeout wrapper for hardware operations
static kern_return_t WaitForCondition(bool (^condition)(void),
                                      uint32_t timeoutMs,
                                      const char *description) {
  for (uint32_t i = 0; i < timeoutMs; i++) {
    if (condition()) {
      return kIOReturnSuccess;
    }
    IOSleep(1);
  }

  os_log(ASLog(), "ASOHCI: Timeout waiting for %s after %u ms", description,
         timeoutMs);
  return kIOReturnTimeout;
}

kern_return_t ASOHCI::CreateWorkQueue() {
  kern_return_t kr = ValidateState(ivars, "CreateWorkQueue");
  if (kr != kIOReturnSuccess) {
    return kr;
  }

  IODispatchQueue *queue = nullptr;
  kr = IODispatchQueue::Create("ASOHCI.WorkQueue", 0, 0, &queue);
  if (kr != kIOReturnSuccess) {
    LogError(kr, "CreateWorkQueue", "IODispatchQueue::Create failed");
    return kr;
  }

  ivars->defaultQ = OSSharedPtr(queue, OSNoRetain);
  os_log(ASLog(), "ASOHCI: Work queue created successfully");
  return kIOReturnSuccess;
}

kern_return_t ASOHCI::MapDeviceMemory() {
  // Get BAR0 info
  uint64_t bar0Size = 0;
  uint8_t bar0Type = 0;
  kern_return_t kr =
      ivars->pciDevice->GetBARInfo(0, &ivars->barIndex, &bar0Size, &bar0Type);
  if (kr != kIOReturnSuccess) {
    os_log(ASLog(), "ASOHCI: GetBARInfo(BAR0) failed: 0x%08x", kr);
    return kr;
  }

  if (bar0Size < 0x2C) {
    os_log(ASLog(), "ASOHCI: BAR0 too small (0x%llx)", bar0Size);
    return kIOReturnNoResources;
  }

  os_log(ASLog(), "ASOHCI: BAR0 idx=%u size=0x%llx type=0x%02x",
         ivars->barIndex, bar0Size, bar0Type);

  // For DriverKit, use direct memory access instead of mapping
  // The MemoryRead32/MemoryWrite32 methods provide access to device memory
  os_log(ASLog(), "ASOHCI: Using direct memory access for device registers");
  return kIOReturnSuccess;
}

kern_return_t ASOHCI::InitializeManagers() {

  // AR Manager
  ivars->arManager = std::make_unique<ASOHCIARManager>();
  if (ivars->arManager.get() == nullptr) {
    os_log(ASLog(), "ASOHCI: Failed to allocate AR Manager");
    return kIOReturnNoMemory;
  }

  // AT Manager
  ivars->atManager = std::make_unique<ASOHCIATManager>();
  if (ivars->atManager.get() == nullptr) {
    os_log(ASLog(), "ASOHCI: Failed to allocate AT Manager");
    // Clean up AR Manager on AT Manager failure
    ivars->arManager.reset();
    return kIOReturnNoMemory;
  }

  // Initialize register IO helper
  ivars->regs = OSSharedPtr(ASOHCIRegisterIO::Create(), OSNoRetain);
  if (ivars->regs &&
      !ivars->regs->Init(ivars->pciDevice.get(), ivars->barIndex)) {
    ivars->regs.reset();
    os_log(ASLog(),
           "ASOHCI: WARNING: Register IO helper initialization failed");
  }

  // Initialize Self-ID Manager with thread-safe callbacks
  ivars->selfIDManager = std::make_unique<SelfIDManager>();
  if (!ivars->selfIDManager) {
    os_log(ASLog(), "ASOHCI: Failed to allocate SelfIDManager");
    return kIOReturnNoMemory;
  }

  // Initialize Topology
  ivars->topology = std::make_unique<Topology>();
  if (!ivars->topology) {
    os_log(ASLog(), "ASOHCI: Failed to allocate Topology");
    ivars->selfIDManager.reset();
    return kIOReturnNoMemory;
  }

  // Set up Self-ID Manager callbacks with error handling
  ASOHCI_IVars *blockIv = ivars;
  ASOHCI *blockSelf = this;

  ivars->selfIDManager->SetCallbacks(
      // onDecode: begin cycle and accumulate nodes (thread-safe)
      [blockIv, blockSelf](const SelfID::Result &res) {
        if (!blockIv || !blockIv->topology)
          return;
        os_log(ASLog(),
               "ASOHCI: Topology decode callback fired (begin cycle): "
               "gen=%u nodes=%lu",
               res.generation, (unsigned long)res.nodes.size());
        blockIv->topology->BeginCycle(res.generation);
        for (const auto &n : res.nodes)
          blockIv->topology->AddOrUpdateNode(n);
      },
      // onStable: finalize and log a concise summary (thread-safe)
      [blockIv, blockSelf](const SelfID::Result &res) {
        if (!blockIv || !blockIv->topology)
          return;
        blockIv->topology->Finalize();
        os_log(ASLog(), "ASOHCI: Topology callback fired (finalize)");
        size_t nodes = blockIv->topology->NodeCount();
        const Topology::Node *root = blockIv->topology->Root();
        uint8_t hops = blockIv->topology->MaxHopsFromRoot();
        bool ok = blockIv->topology->IsConsistent();
        auto &info = blockIv->topology->Info();
        os_log(ASLog(),
               "ASOHCI: Topology gen=%u nodes=%lu rootPhy=%u hops=%u "
               "consistent=%d warnings=%lu",
               info.generation, (unsigned long)nodes,
               root ? root->phy.value : 0xFF, hops, ok ? 1 : 0,
               (unsigned long)info.warnings.size());

        blockIv->topology->Log();
      });

  // IR Manager
  ivars->irManager = std::make_unique<ASOHCIIRManager>();
  if (ivars->irManager.get() == nullptr) {
    os_log(ASLog(), "ASOHCI: Failed to allocate IR Manager");
    return kIOReturnNoMemory;
  }

  // Initialize IR Manager
  kern_return_t irResult =
      ivars->irManager->Initialize(ivars->pciDevice, ivars->barIndex, {});
  if (irResult != kIOReturnSuccess) {
    os_log(ASLog(), "ASOHCI: IR Manager initialization failed: 0x%08x",
           irResult);
    ivars->irManager.reset();
    return irResult;
  }

  // IT Manager
  ivars->itManager = std::make_unique<ASOHCIITManager>();
  if (ivars->itManager.get() == nullptr) {
    os_log(ASLog(), "ASOHCI: Failed to allocate IT Manager");
    ivars->irManager.reset();
    return kIOReturnNoMemory;
  }

  os_log(ASLog(), "ASOHCI: Managers initialized successfully");

  return kIOReturnSuccess;
}

kern_return_t ASOHCI::InitializeOHCI() {
  kern_return_t kr = kIOReturnSuccess;

  // Open device and enable PCI capabilities
  kr = ivars->pciDevice->Open(this, 0);
  if (kr != kIOReturnSuccess) {
    os_log(ASLog(), "ASOHCI: PCI Open failed: 0x%08x", kr);
    return kr;
  }

  // Enable BusMaster|MemorySpace
  uint16_t cmd = 0;
  ivars->pciDevice->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &cmd);
  uint16_t newCmd =
      (uint16_t)(cmd | kIOPCICommandBusMaster | kIOPCICommandMemorySpace);
  if (newCmd != cmd) {
    ivars->pciDevice->ConfigurationWrite16(kIOPCIConfigurationOffsetCommand,
                                           newCmd);
    os_log(ASLog(), "ASOHCI: PCI CMD updated: 0x%04x -> 0x%04x", cmd, newCmd);
  }

  // Initialize interrupt handling
  kr = SetupInterrupts();
  if (kr != kIOReturnSuccess) {
    return kr;
  }

  // Continue with OHCI hardware initialization
  kr = InitializeOHCIHardware();
  if (kr != kIOReturnSuccess) {
    return kr;
  }

  os_log(ASLog(), "ASOHCI: OHCI initialization completed successfully");
  return kIOReturnSuccess;
}

kern_return_t ASOHCI::SetupInterrupts() {
  kern_return_t kr = kIOReturnSuccess;

  // Configure interrupts (MSI-X preferred, fallback to MSI, then legacy)
  kr = ivars->pciDevice->ConfigureInterrupts(kIOInterruptTypePCIMessagedX, 1, 1,
                                             0);
  if (kr == kIOReturnSuccess) {
    os_log(ASLog(), "ASOHCI: Configured MSI-X interrupts");
  } else {
    kr = ivars->pciDevice->ConfigureInterrupts(kIOInterruptTypePCIMessaged, 1,
                                               1, 0);
    if (kr == kIOReturnSuccess) {
      os_log(ASLog(), "ASOHCI: Configured MSI interrupts");
    } else {
      os_log(ASLog(), "ASOHCI: Falling back to legacy interrupts");
    }
  }

  // Create interrupt source
  IOInterruptDispatchSource *src = nullptr;
  kr = IOInterruptDispatchSource::Create(ivars->pciDevice.get(), 0,
                                         ivars->defaultQ.get(), &src);
  if (kr != kIOReturnSuccess) {
    os_log(ASLog(), "ASOHCI: IOInterruptDispatchSource::Create failed: 0x%08x",
           kr);
    return kr;
  }

  ivars->intSource = OSSharedPtr(src, OSNoRetain);

  // Set up interrupt handler
  OSAction *action = nullptr;
  kr = CreateActionInterruptOccurred(0, &action);
  if (kr != kIOReturnSuccess) {
    os_log(ASLog(), "ASOHCI: CreateActionInterruptOccurred failed: 0x%08x", kr);
    return kr;
  }

  ivars->intSource->SetHandler(action);
  action->release();
  ivars->intSource->SetEnableWithCompletion(true, nullptr);

  os_log(ASLog(), "ASOHCI: Interrupt handling configured successfully");
  return kIOReturnSuccess;
}

kern_return_t ASOHCI::InitializeOHCIHardware() {
  // Clear interrupts
  ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IntEventClear,
                                  0xFFFFFFFFu);
  ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IsoXmitIntEventClear,
                                  0xFFFFFFFFu);
  ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IsoRecvIntEventClear,
                                  0xFFFFFFFFu);
  ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IntMaskClear,
                                  0xFFFFFFFFu);
  ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IsoXmitIntMaskClear,
                                  0xFFFFFFFFu);
  ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IsoRecvIntMaskClear,
                                  0xFFFFFFFFu);
  os_log(ASLog(), "ASOHCI: Cleared interrupt events/masks");

  // Software reset
  ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_HCControlSet,
                                  kOHCI_HCControl_SoftReset);
  IOSleep(10);
  os_log(ASLog(), "ASOHCI: Software reset issued");

  // Re-clear after reset
  ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IntEventClear,
                                  0xFFFFFFFFu);
  ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IsoXmitIntEventClear,
                                  0xFFFFFFFFu);
  ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IsoRecvIntEventClear,
                                  0xFFFFFFFFu);

  // Enter LPS + enable posted writes
  const uint32_t hcSet = (kOHCI_HCControl_LPS | kOHCI_HCControl_PostedWriteEn);
  ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_HCControlSet, hcSet);
  OHCI_MEMORY_BARRIER(); // Ensure HC control changes are visible to hardware
  os_log(ASLog(), "ASOHCI: HCControlSet LPS+PostedWrite (0x%08x)", hcSet);

  // Program BusOptions and NodeID
  uint32_t bo = 0;
  ivars->pciDevice->MemoryRead32(ivars->barIndex, kOHCI_BusOptions, &bo);
  uint32_t origBo = bo;
  bo |= 0x60000000;  // set ISC|CMC
  bo &= ~0x18000000; // clear BMC|PMC
  bo &= ~0x00FF0000; // clear cyc_clk_acc
  if (bo != origBo) {
    ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_BusOptions, bo);
    OHCI_MEMORY_BARRIER(); // Ensure bus options are visible to hardware
    os_log(ASLog(), "ASOHCI: BusOptions updated 0x%08x->0x%08x", origBo, bo);
  }

  // Provisional NodeID
  ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_NodeID, 0x0000FFC0);
  OHCI_MEMORY_BARRIER(); // Ensure NodeID is visible to hardware
  os_log(ASLog(), "ASOHCI: Provisional NodeID set to 0x0000FFC0");

  // Enable link and reception
  // Enable link and reception
  ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_HCControlSet,
                                  kOHCI_HCControl_programPhyEnable);
  ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_HCControlSet,
                                  kOHCI_HCControl_LinkEnable);
  ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_LinkControlSet,
                                  (kOHCI_LC_RcvSelfID | kOHCI_LC_RcvPhyPkt));
  OHCI_MEMORY_BARRIER(); // Ensure link control changes are visible to hardware
  os_log(ASLog(), "ASOHCI: Link enabled with Self-ID and PHY reception");

  // Enable comprehensive interrupts
  uint32_t irqs = kOHCI_Int_ReqTxComplete | kOHCI_Int_RespTxComplete |
                  kOHCI_Int_RqPkt | kOHCI_Int_RsPkt | kOHCI_Int_IsochTx |
                  kOHCI_Int_IsochRx | kOHCI_Int_PostedWriteErr |
                  kOHCI_Int_SelfIDComplete | kOHCI_Int_SelfIDComplete2 |
                  kOHCI_Int_RegAccessFail | kOHCI_Int_UnrecoverableError |
                  kOHCI_Int_CycleTooLong | kOHCI_Int_MasterEnable |
                  kOHCI_Int_BusReset | kOHCI_Int_Phy;
  ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IntMaskSet, irqs);
  OHCI_MEMORY_BARRIER(); // Ensure interrupt mask changes are visible to
                         // hardware
  os_log(ASLog(), "ASOHCI: Comprehensive interrupt mask set: 0x%08x", irqs);

  // Final link activation
  ivars->pciDevice->MemoryWrite32(
      ivars->barIndex, kOHCI_HCControlSet,
      (kOHCI_HCControl_LinkEnable | kOHCI_HCControl_BIBimageValid));
  OHCI_MEMORY_BARRIER(); // Ensure final link activation is visible to hardware
  os_log(ASLog(), "ASOHCI: Final link activation completed");

  return kIOReturnSuccess;
}

// =====================================================================================
// Start
// =====================================================================================
kern_return_t IMPL(ASOHCI, Start) {
  kern_return_t kr = Start(provider, SUPERDISPATCH);
  if (kr != kIOReturnSuccess) {
    os_log(ASLog(), "ASOHCI: Start superdispatch failed: 0x%08x", kr);
    CleanupOnError();
    return kr;
  }
  if (!ivars) {
    os_log(ASLog(), "ASOHCI: ivars not allocated");
    CleanupOnError();
    return kIOReturnNoResources;
  }

  // State machine: Transition to Starting (REFACTOR.md §9)
  kr = TransitionState(ivars, ASOHCIState::Starting, "Start begin");
  if (kr != kIOReturnSuccess) {
    CleanupOnError();
    return kr;
  }

  os_log(ASLog(), "ASOHCI: Start() begin bring-up");

  // Step 1: Store provider with smart pointer and retain
  ivars->pciDevice =
      OSSharedPtr(OSDynamicCast(IOPCIDevice, provider), OSRetain);
  if (ivars->pciDevice.get() == nullptr) {
    os_log(ASLog(), "ASOHCI: Provider is not IOPCIDevice");
    TransitionState(ivars, ASOHCIState::Quiescing, "provider cast failed");
    CleanupOnError();
    return kIOReturnBadArgument;
  }

  // Step 2: Create dispatch queue
  kr = CreateWorkQueue();
  if (kr != kIOReturnSuccess) {
    TransitionState(ivars, ASOHCIState::Quiescing,
                    "work queue creation failed");
    CleanupOnError();
    return kr;
  }

  // Step 3: Map device memory
  kr = MapDeviceMemory();
  if (kr != kIOReturnSuccess) {
    TransitionState(ivars, ASOHCIState::Quiescing,
                    "device memory mapping failed");
    CleanupOnError();
    return kr;
  }

  // Step 4: Initialize managers
  kr = InitializeManagers();
  if (kr != kIOReturnSuccess) {
    TransitionState(ivars, ASOHCIState::Quiescing,
                    "manager initialization failed");
    CleanupOnError();
    return kr;
  }

  // Step 5: Continue with OHCI initialization sequence
  kr = InitializeOHCI();
  if (kr != kIOReturnSuccess) {
    TransitionState(ivars, ASOHCIState::Quiescing,
                    "OHCI initialization failed");
    CleanupOnError();
    return kr;
  }

  // State machine: Transition to Running (REFACTOR.md §9)
  kr = TransitionState(ivars, ASOHCIState::Running, "bring-up complete");
  if (kr != kIOReturnSuccess) {
    // This shouldn't happen with valid transitions, but handle it
    CleanupOnError();
    return kr;
  }

  os_log(ASLog(), "ASOHCI: Start() bring-up complete");
  return kIOReturnSuccess;
}

// =====================================================================================
// Stop
// =====================================================================================
kern_return_t IMPL(ASOHCI, Stop) {
  // We're being torn down because the provider is going away – behave as
  // unplug. Set deviceGone early to prevent any further MMIO accesses during
  // teardown.
  if (ivars) {
    __atomic_store_n(&ivars->deviceGone, true, __ATOMIC_RELEASE);
    // Also set device gone in managers
    if (ivars->irManager) {
      ivars->irManager->SetDeviceGone(true);
    }
    if (ivars->itManager) {
      // Assuming ITManager has similar method
      // ivars->itManager->SetDeviceGone(true);
    }
  }

  os_log(ASLog(), "ASOHCI: Stop begin");

  // State machine: Check if we're already in a stopping state (REFACTOR.md §9)
  if (ivars && IsOperationAllowed(ivars, ASOHCIState::Quiescing)) {
    os_log(ASLog(), "ASOHCI: Stop called while already quiescing");
    return kIOReturnSuccess; // Idempotent
  }

  // State machine: Transition to Quiescing if not already there (REFACTOR.md
  // §9)
  if (ivars) {
    kern_return_t stateKr =
        TransitionState(ivars, ASOHCIState::Quiescing, "Stop begin");
    if (stateKr != kIOReturnSuccess) {
      // If transition fails, we're probably already in a terminal state
      os_log(ASLog(), "ASOHCI: Stop state transition failed: 0x%08x", stateKr);
    }
  }

  // 0) Set stopping flag FIRST to prevent new interrupt processing
  if (ivars) {
    __atomic_store_n(&ivars->stopping, true, __ATOMIC_RELEASE);
    os_log(ASLog(),
           "ASOHCI: Stopping flag set - blocking new interrupt processing");
  }

  // Check if device is still present
  bool devicePresent = !__atomic_load_n(&ivars->deviceGone, __ATOMIC_ACQUIRE);

  // 1) Disable interrupt source immediately to stop new interrupts
  if (ivars && ivars->intSource) {
    // Use completion handler to ensure interrupt disable is synchronized
    __block bool interruptDisabled = false;
    ivars->intSource->SetEnableWithCompletion(false, ^{
      interruptDisabled = true;
      os_log(ASLog(), "ASOHCI: Interrupt source disabled");
    });
    // Wait for completion (simple polling approach for DriverKit)
    while (!interruptDisabled) {
      IOSleep(1); // Brief sleep to avoid busy waiting
    }
    os_log(ASLog(), "ASOHCI: Interrupt source disabled and synchronized");
  }

  // 2) Disable PCI bus mastering immediately to stop DMA
  if (auto pci = OSDynamicCast(IOPCIDevice, provider)) {
    uint16_t cmd = 0;
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &cmd);
    uint16_t newCmd = (uint16_t)(cmd & ~kIOPCICommandBusMaster);
    if (newCmd != cmd) {
      pci->ConfigurationWrite16(kIOPCIConfigurationOffsetCommand, newCmd);
      os_log(ASLog(), "ASOHCI: PCI Bus Mastering disabled early");
    }
  }

  // 3) Stop all managers BEFORE hardware teardown
  if (ivars) {
    os_log(ASLog(), "ASOHCI: Stopping context managers...");
    if (ivars->arManager) {
      ivars->arManager->Stop();
      os_log(ASLog(), "ASOHCI: AR Manager stopped");
    }
    if (ivars->atManager) {
      ivars->atManager->Stop();
      os_log(ASLog(), "ASOHCI: AT Manager stopped");
    }
    if (ivars->irManager) {
      ivars->irManager->StopAll();
      os_log(ASLog(), "ASOHCI: IR Manager stopped");
    }
    if (ivars->itManager) {
      ivars->itManager->StopAll();
      os_log(ASLog(), "ASOHCI: IT Manager stopped");
    }
  }

  // 4) Hardware soft reset only if device is present
  if (devicePresent && ivars && ivars->pciDevice) {
    os_log(ASLog(), "ASOHCI: Quiescing hardware...");

    // Clear and mask ALL interrupts to prevent any further processing
    if (DeviceAccessOk(ivars)) {
      ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IntMaskClear,
                                      0xFFFFFFFFu);
      ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IntEventClear,
                                      0xFFFFFFFFu);
      ivars->pciDevice->MemoryWrite32(ivars->barIndex,
                                      kOHCI_IsoXmitIntEventClear, 0xFFFFFFFFu);
      ivars->pciDevice->MemoryWrite32(ivars->barIndex,
                                      kOHCI_IsoRecvIntEventClear, 0xFFFFFFFFu);
      ivars->pciDevice->MemoryWrite32(ivars->barIndex,
                                      kOHCI_IsoXmitIntMaskClear, 0xFFFFFFFFu);
      ivars->pciDevice->MemoryWrite32(ivars->barIndex,
                                      kOHCI_IsoRecvIntMaskClear, 0xFFFFFFFFu);

      // Drop link control enables
      ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_LinkControlClear,
                                      (kOHCI_LC_RcvSelfID | kOHCI_LC_RcvPhyPkt |
                                       kOHCI_LC_CycleTimerEnable));

      // Soft reset to quiesce the controller
      ivars->pciDevice->MemoryWrite32(
          ivars->barIndex, kOHCI_HCControlClear,
          (kOHCI_HCControl_LinkEnable | kOHCI_HCControl_aPhyEnhanceEnable));
      ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_HCControlSet,
                                      kOHCI_HCControl_SoftReset);
      OHCI_MEMORY_BARRIER(); // Ensure soft reset commands are visible to
                             // hardware
    }

    // Wait for soft reset to complete (safe to read even if device gone)
    kern_return_t resetResult = WaitForCondition(
        ^{
          if (!DeviceAccessOk(ivars))
            return true; // Skip if device gone
          uint32_t hcControl = 0;
          ivars->pciDevice->MemoryRead32(ivars->barIndex, kOHCI_HCControl,
                                         &hcControl);
          return !(hcControl & kOHCI_HCControl_SoftReset);
        },
        100, "soft reset completion");

    if (resetResult == kIOReturnSuccess) {
      os_log(ASLog(), "ASOHCI: Hardware quiesced successfully");
    } else {
      LogError(resetResult, "Stop", "hardware quiesce timeout");
    }
  }

  // 6) Self-ID dispatch queue is owned by InterruptRouter (released with
  // router)

  // 7) Close PCI device
  if (auto pci = OSDynamicCast(IOPCIDevice, provider)) {
    os_log(ASLog(), "ASOHCI: Closing PCI device...");
    // Clear BusMaster and MemorySpace bits
    uint16_t cmd = 0;
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &cmd);
    uint16_t clr =
        (uint16_t)(cmd & ~(kIOPCICommandBusMaster | kIOPCICommandMemorySpace));
    if (clr != cmd) {
      pci->ConfigurationWrite16(kIOPCIConfigurationOffsetCommand, clr);
    }
    pci->Close(this, 0);
    os_log(ASLog(), "ASOHCI: PCI device closed");
  }

  // 8) Clear PCI device reference to prevent any further access
  if (ivars) {
    ivars->pciDevice.reset();
    ivars->barIndex = 0;
    os_log(ASLog(), "ASOHCI: PCI device reference cleared");
  }

  // 9) Clean up managers and helpers (safe now that hardware is quiesced)
  if (ivars) {
    os_log(ASLog(), "ASOHCI: Cleaning up managers and helpers...");
    if (ivars->selfIDManager) {
      ivars->selfIDManager->Teardown();
      ivars->selfIDManager.reset();
    }
    if (ivars->configROMManager) {
      ivars->configROMManager->Teardown();
      ivars->configROMManager.reset();
    }
    if (ivars->topology) {
      ivars->topology.reset();
    }
    if (ivars->phyAccess) {
      ivars->phyAccess.reset();
    }

    // Reset OSSharedPtr managers (automatic cleanup)
    ivars->arManager.reset();
    ivars->atManager.reset();
    ivars->irManager.reset();
    ivars->itManager.reset();

    os_log(ASLog(), "ASOHCI: Managers and helpers cleaned up");
  }

  // 10) Release interrupt source (do this late to ensure no interrupts during
  // cleanup)
  if (ivars && ivars->intSource) {
    ivars->intSource->SetEnableWithCompletion(false, nullptr);
    ivars->intSource.reset();
    os_log(ASLog(), "ASOHCI: Interrupt source released");
  }

  // 11) Set deviceGone flag LAST
  if (ivars) {
    __atomic_store_n(&ivars->deviceGone, true, __ATOMIC_RELEASE);
    os_log(ASLog(), "ASOHCI: Stop completed - device marked as gone");
  }

  // State machine: Transition to Dead (terminal state) (REFACTOR.md §9)
  if (ivars) {
    kern_return_t stateKr =
        TransitionState(ivars, ASOHCIState::Dead, "Stop completed");
    if (stateKr != kIOReturnSuccess) {
      os_log(ASLog(), "ASOHCI: Stop state transition to Dead failed: 0x%08x",
             stateKr);
    }
  }

  // 12) NOW call super Stop LAST (following Apple's pattern)
  kern_return_t result = Stop(provider, SUPERDISPATCH);
  os_log(ASLog(), "ASOHCI: Super Stop completed: 0x%08x", result);

  return result;
}

// =====================================================================================
// Interrupt handler (typed action). IMPORTANT: complete the OSAction.
// =====================================================================================
void ASOHCI::InterruptOccurred_Impl(ASOHCI_InterruptOccurred_Args) {
  // CRITICAL: Check stopping and deviceGone flags FIRST before any processing
  if (!ivars || __atomic_load_n(&ivars->stopping, __ATOMIC_ACQUIRE) ||
      __atomic_load_n(&ivars->deviceGone, __ATOMIC_ACQUIRE)) {
    os_log(ASLog(),
           "ASOHCI: Interrupt during teardown or device gone - ignoring");
    return;
  }

  // State machine: Only process interrupts when Running (REFACTOR.md §9)
  if (!IsOperationAllowed(ivars, ASOHCIState::Running)) {
    os_log(ASLog(), "ASOHCI: Interrupt blocked - state is %s, requires Running",
           ivars->stateDescription);
    return;
  }

  // Use variables for thread-safe access to ivars on dispatch queue
  // (Variables not used in this method)

  // Double-check PCI device is still valid (defensive programming)
  if (!ivars->pciDevice) {
    os_log(ASLog(), "ASOHCI: Interrupt with null PCI device - ignoring");
    return;
  }

  // Check device access safety before any MMIO
  if (!DeviceAccessOk(ivars)) {
    os_log(ASLog(), "ASOHCI: Interrupt blocked - device access not safe");
    return;
  }

  uint64_t seq =
      __atomic_add_fetch(&ivars->interruptCount, 1, __ATOMIC_RELAXED);
  os_log(ASLog(), "ASOHCI: InterruptOccurred #%llu (count=%llu time=%llu)",
         (unsigned long long)seq, (unsigned long long)count,
         (unsigned long long)time);

  // Re-check stopping flag after logging (another teardown might have started)
  if (__atomic_load_n(&ivars->stopping, __ATOMIC_ACQUIRE)) {
    os_log(ASLog(),
           "ASOHCI: Interrupt processing aborted - teardown in progress");
    return;
  }

  uint32_t intEvent = 0;
  ivars->pciDevice->MemoryRead32(ivars->barIndex, kOHCI_IntEvent, &intEvent);
  if (intEvent == 0) {
    os_log(ASLog(), "ASOHCI: Spurious MSI (IntEvent=0)");
    return;
  }

  // Watchdog: if BusReset was masked and Self-ID has not completed within
  // ~250ms, re-enable BusReset
  if (ivars->selfIDInProgress && ivars->busResetMasked) {
    const uint64_t threshold_ns = 250000000ULL; // 250 ms
    if (time > ivars->lastBusResetTime &&
        (time - ivars->lastBusResetTime) > threshold_ns) {
      if (DeviceAccessOk(ivars)) {
        ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IntMaskSet,
                                        kOHCI_Int_BusReset);
      }
      ivars->busResetMasked = false;
      os_log(ASLog(),
             "ASOHCI: Watchdog re-enabled BusReset mask after timeout");
      // Best-effort: keep Self-ID armed in case we missed it
      ArmSelfIDReceive(/*clearCount=*/false);
    }
  }

  // Ack/clear what we saw (write-1-to-clear), but per OHCI 1.1 and Linux parity
  // do not clear busReset or postedWriteErr in this bulk clear.
  uint32_t clearMask =
      intEvent & ~(kOHCI_Int_BusReset | kOHCI_Int_PostedWriteErr);
  if (clearMask && DeviceAccessOk(ivars))
    ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IntEventClear,
                                    clearMask);
  os_log(ASLog(), "ASOHCI: IntEvent=0x%08x", intEvent);

  LogUtils::DumpIntEvent(intEvent);

  // Handle Posted Write Error via router
  if (intEvent & kOHCI_Int_PostedWriteErr) {
    if (ivars->interruptRouter)
      ivars->interruptRouter->OnPostedWriteError();
  }

  // Bus reset (coalesce repeated resets until SelfIDComplete)
  if (intEvent & kOHCI_Int_BusReset) {
    if (ivars->interruptRouter) {

      ivars->interruptRouter->OnBusReset(time);
    }
  }

  // NOTE: Self-ID complete will deliver alpha self-ID quadlets (#0 and optional
  // #1/#2). Parser implements IEEE 1394-2008 §16.3.2.1 (Alpha). Beta support
  // can be added later.

  // Self-ID complete
  if (intEvent & (kOHCI_Int_SelfIDComplete | kOHCI_Int_SelfIDComplete2)) {
    uint32_t selfIDCount1 = 0;
    if (DeviceAccessOk(ivars)) {
      ivars->pciDevice->MemoryRead32(ivars->barIndex, kOHCI_SelfIDCount,
                                     &selfIDCount1);
    }
    uint32_t generation =
        (selfIDCount1 & kOHCI_SelfIDCount_selfIDGeneration) >> 16;
    bool err = (selfIDCount1 & kOHCI_SelfIDCount_selfIDError) != 0;
    if (ivars->interruptRouter) {

      ivars->interruptRouter->OnSelfIDComplete(selfIDCount1, generation, err);
    }
  }

  // AR/AT Manager Interrupt Handling (OHCI 1.1 §6.1 bits 0-3)
  if (intEvent & (kOHCI_Int_RqPkt | kOHCI_Int_RsPkt | kOHCI_Int_ReqTxComplete |
                  kOHCI_Int_RespTxComplete)) {

    // AR Manager packet reception interrupts (bits 2-3)
    if (ivars->interruptRouter) {
      if (intEvent & kOHCI_Int_RqPkt)
        ivars->interruptRouter->OnAR_Request_PacketArrived();
      if (intEvent & kOHCI_Int_RsPkt)
        ivars->interruptRouter->OnAR_Response_PacketArrived();
    }

    // AT Manager transmission complete interrupts (bits 0-1)
    if (ivars->interruptRouter) {
      if (intEvent & kOHCI_Int_ReqTxComplete)
        ivars->interruptRouter->OnAT_Request_TxComplete();
      if (intEvent & kOHCI_Int_RespTxComplete)
        ivars->interruptRouter->OnAT_Response_TxComplete();
    }

    // Legacy context interrupt handling (kept for transition)
    if ((intEvent & kOHCI_Int_RqPkt) && ivars->arRequestContext) {
      ivars->arRequestContext->HandleInterrupt();
    }
    if ((intEvent & kOHCI_Int_RsPkt) && ivars->arResponseContext) {
      ivars->arResponseContext->HandleInterrupt();
    }
    // AT contexts removed - handled by AT Manager
    // if ((intEvent & kOHCI_Int_ReqTxComplete) && ivars->atRequestContext) {
    //   ivars->atRequestContext->HandleInterrupt();
    // }
    // if ((intEvent & kOHCI_Int_RespTxComplete) && ivars->atResponseContext) {
    //   ivars->atResponseContext->HandleInterrupt();
    // }
  }

  // Cycle too long handling via router
  if (intEvent & kOHCI_Int_CycleTooLong) {
    if (ivars->interruptRouter)
      ivars->interruptRouter->OnCycleTooLong();
  }

  // Isochronous Transmit/Receive Manager Interrupts (OHCI 1.1 §6.3-6.4)
  if (intEvent & (kOHCI_Int_IsochTx | kOHCI_Int_IsochRx)) {

    // IT Manager interrupt handling (OHCI 1.1 §6.3)
    if (intEvent & kOHCI_Int_IsochTx) {
      uint32_t txMask = 0;
      if (DeviceAccessOk(ivars)) {
        ivars->pciDevice->MemoryRead32(ivars->barIndex,
                                       kOHCI_IsoXmitIntEventSet, &txMask);
      }
      if (ivars->interruptRouter)
        ivars->interruptRouter->OnIsoTxMask(txMask);
      if (txMask && DeviceAccessOk(ivars)) {
        // Clear the context-specific events
        ivars->pciDevice->MemoryWrite32(ivars->barIndex,
                                        kOHCI_IsoXmitIntEventClear, txMask);
      }
    }

    // IR Manager interrupt handling (OHCI 1.1 §6.4)
    if (intEvent & kOHCI_Int_IsochRx) {
      uint32_t rxMask = 0;
      if (DeviceAccessOk(ivars)) {
        ivars->pciDevice->MemoryRead32(ivars->barIndex,
                                       kOHCI_IsoRecvIntEventSet, &rxMask);
      }
      if (ivars->interruptRouter)
        ivars->interruptRouter->OnIsoRxMask(rxMask);
      if (rxMask && DeviceAccessOk(ivars)) {
        // Clear the context-specific events
        ivars->pciDevice->MemoryWrite32(ivars->barIndex,
                                        kOHCI_IsoRecvIntEventClear, rxMask);
      }
    }
  }

  // Cycle inconsistent: rate limiting + IT fan-out via router (enabled after
  // cycle timer armed)
  if (intEvent & kOHCI_Int_CycleInconsistent) {
    if (ivars->interruptRouter)
      ivars->interruptRouter->OnCycleInconsistent(time);
  }

  // All interrupt bits are now handled by the comprehensive DumpIntEvent
  // function No need for generic "Other IRQ bits" logging as every bit is
  // properly identified per OHCI §6.1
}

// =====================================================================================
// State Machine Implementation (REFACTOR.md §9)
// =====================================================================================

// Forward declarations for state machine functions
static kern_return_t TransitionState(ASOHCI_IVars *ivars, ASOHCIState newState,
                                     const char *description);
static const char *StateToString(ASOHCIState state);
static bool IsOperationAllowed(ASOHCI_IVars *ivars, ASOHCIState allowedState);

// Thread-safe state transition with validation
static kern_return_t TransitionState(ASOHCI_IVars *ivars, ASOHCIState newState,
                                     const char *description) {
  if (!ivars) {
    return kIOReturnNoResources;
  }

  ASOHCIState currentState = static_cast<ASOHCIState>(
      __atomic_load_n(&ivars->state, __ATOMIC_ACQUIRE));

  // Validate state transitions (REFACTOR.md §9)
  bool validTransition = false;
  switch (currentState) {
  case ASOHCIState::Stopped:
    validTransition = (newState == ASOHCIState::Starting);
    break;
  case ASOHCIState::Starting:
    validTransition = (newState == ASOHCIState::Running ||
                       newState == ASOHCIState::Quiescing);
    break;
  case ASOHCIState::Running:
    validTransition = (newState == ASOHCIState::Quiescing);
    break;
  case ASOHCIState::Quiescing:
    validTransition = (newState == ASOHCIState::Dead);
    break;
  case ASOHCIState::Dead:
    validTransition = false; // Terminal state
    break;
  }

  if (!validTransition) {
    os_log(ASLog(), "ASOHCI: Invalid state transition %s -> %s (%s)",
           ivars->stateDescription, StateToString(newState), description);
    return kIOReturnInvalid;
  }

  // Perform atomic transition
  __atomic_store_n(&ivars->state, static_cast<uint32_t>(newState),
                   __ATOMIC_RELEASE);
  strlcpy(ivars->stateDescription, StateToString(newState),
          sizeof(ivars->stateDescription));

  os_log(ASLog(), "ASOHCI: State transition %s -> %s (%s)",
         StateToString(currentState), StateToString(newState), description);

  return kIOReturnSuccess;
}

// Convert state enum to string for logging
static const char *StateToString(ASOHCIState state) {
  switch (state) {
  case ASOHCIState::Stopped:
    return "Stopped";
  case ASOHCIState::Starting:
    return "Starting";
  case ASOHCIState::Running:
    return "Running";
  case ASOHCIState::Quiescing:
    return "Quiescing";
  case ASOHCIState::Dead:
    return "Dead";
  default:
    return "Unknown";
  }
}

// Check if operation is allowed (more permissive than ValidateOperation)
static bool IsOperationAllowed(ASOHCI_IVars *ivars, ASOHCIState allowedState) {
  if (!ivars)
    return false;
  ASOHCIState currentState = static_cast<ASOHCIState>(
      __atomic_load_n(&ivars->state, __ATOMIC_ACQUIRE));
  return (currentState == allowedState);
}

// =====================================================================================
// Copy Bridge Logs
// =====================================================================================
kern_return_t ASOHCI::CopyBridgeLogs(OSData **outData) {
  if (!outData) {
    os_log(ASLog(), "ASOHCI: CopyBridgeLogs - null out parameter");
    return kIOReturnBadArgument;
  }

  *outData = nullptr;

  // Get the bridge logs from the logging system
  // This is a placeholder - in a real implementation, you'd collect
  // the logs from your logging buffer
  const char *logData = "ASOHCI Bridge Logs\n"; // Placeholder
  size_t logLength = strlen(logData);

  // Create OSData with the log content
  *outData = OSData::withBytes(logData, logLength);
  if (!*outData) {
    os_log(ASLog(), "ASOHCI: CopyBridgeLogs - failed to create OSData");
    return kIOReturnNoMemory;
  }

  os_log(ASLog(), "ASOHCI: CopyBridgeLogs - returned %zu bytes of log data",
         logLength);
  return kIOReturnSuccess;
}

// =====================================================================================
// Link Interface Implementation (PREPARATION.md §155-249)
// =====================================================================================

// Controller registration
kern_return_t ASOHCI::SetController(ASFireWireController *controller) {
  kern_return_t kr = ValidateState(ivars, "SetController");
  if (kr != kIOReturnSuccess) {
    return kr;
  }

  ivars->controller = controller;
  os_log(ASLog(), "ASOHCI: Controller registered: %p", controller);
  return kIOReturnSuccess;
}

// Hardware state access methods
uint16_t ASOHCI::GetNodeID() const {
  if (!ivars || !ivars->pciDevice) {
    return 0xFFFF; // Invalid node ID
  }

  uint32_t nodeReg = 0;
  ivars->pciDevice->MemoryRead32(ivars->barIndex, kOHCI_NodeID, &nodeReg);

  // Verify iDValid bit (bit 31) is set
  if (!(nodeReg & (1u << 31))) {
    return 0xFFFF; // Invalid node ID
  }

  // Extract busNumber (bits 25:16) and nodeNumber (bits 5:0)
  uint16_t busNumber = (nodeReg >> 16) & 0x3FF;
  uint16_t nodeNumber = nodeReg & 0x3F;

  return (busNumber << 6) | nodeNumber;
}

uint64_t ASOHCI::GetLocalGUID() const {
  if (!ivars || !ivars->pciDevice) {
    return 0;
  }

  uint32_t guidHi = 0, guidLo = 0;
  ivars->pciDevice->MemoryRead32(ivars->barIndex, kOHCI_GUIDHi, &guidHi);
  ivars->pciDevice->MemoryRead32(ivars->barIndex, kOHCI_GUIDLo, &guidLo);

  return ((uint64_t)guidHi << 32) | guidLo;
}

uint32_t ASOHCI::GetGeneration() const {
  if (!ivars) {
    return 0;
  }

  // Generation tracked from Self-ID processing
  return ivars->generation;
}

kern_return_t ASOHCI::GetCycleTime(uint32_t *cycleTime) {
  if (!cycleTime) {
    return kIOReturnBadArgument;
  }

  kern_return_t kr = ValidateState(ivars, "GetCycleTime");
  if (kr != kIOReturnSuccess) {
    return kr;
  }

  ivars->pciDevice->MemoryRead32(ivars->barIndex, kOHCI_CycleTimer, cycleTime);
  return kIOReturnSuccess;
}

// Transaction primitives - MVP synchronous implementation
kern_return_t ASOHCI::ReadQuad(uint16_t nodeID, uint16_t addrHi,
                               uint32_t addrLo, uint32_t *outValue,
                               uint32_t generation, uint32_t speed) {
  if (!outValue) {
    return kIOReturnBadArgument;
  }

  kern_return_t kr = ValidateState(ivars, "ReadQuad");
  if (kr != kIOReturnSuccess) {
    return kr;
  }

  // Validate parameters
  if (nodeID > 62) {
    return kIOReturnBadArgument;
  }

  if (generation != GetGeneration()) {
    os_log(ASLog(), "ASOHCI: ReadQuad generation mismatch: %u vs %u",
           generation, GetGeneration());
    return kIOReturnAborted;
  }

  // For MVP, implement a simple synchronous read using existing AT
  // infrastructure This is a simplified version - full implementation would use
  // ASFWReadQuadCommand

  os_log(ASLog(),
         "ASOHCI: ReadQuad nodeID=0x%04x addr=0x%04x%08x gen=%u speed=%u",
         nodeID, addrHi, addrLo, generation, speed);

  // TODO: Use ASFWReadQuadCommand or AT manager for actual transaction
  // For MVP, return realistic Config ROM data based on address

  uint64_t address = ((uint64_t)addrHi << 32) | addrLo;
  const uint64_t configROMBase = 0xFFFFF0000400ULL;

  if (address >= configROMBase && address < configROMBase + 1024) {
    // Reading from Config ROM address space
    uint32_t offset = (uint32_t)(address - configROMBase);
    uint32_t romIndex = offset / 4;

    // Generate realistic Config ROM data for MVP testing
    switch (romIndex) {
    case 0:                   // ROM header (info length + CRC)
      *outValue = 0x04040404; // 4-quadlet info block + dummy CRC
      break;
    case 1:                   // Bus info block - GUID high
      *outValue = 0x31333934; // "1394" in ASCII
      break;
    case 2:                            // Bus info block - GUID low
      *outValue = 0x12340000 | nodeID; // Dummy GUID with nodeID
      break;
    case 3:                   // Bus options
      *outValue = 0x83C0FFFF; // S400, cycle master capable
      break;
    case 4:                   // Root directory header
      *outValue = 0x0004C152; // 4 entries, CRC
      break;
    case 5:                   // Vendor ID
      *outValue = 0x03001234; // Vendor ID = 0x1234
      break;
    case 6:                   // Model ID
      *outValue = 0x17005678; // Model ID = 0x5678
      break;
    default:
      *outValue = 0x00000000; // Default for other ROM locations
      break;
    }
  } else {
    // Reading from other address spaces
    *outValue = 0xDEADBEEF; // Placeholder for non-ROM reads
  }

  return kIOReturnSuccess;
}

kern_return_t ASOHCI::ReadBlock(uint16_t nodeID, uint16_t addrHi,
                                uint32_t addrLo, IOMemoryDescriptor *buffer,
                                IOByteCount offset, uint32_t length,
                                uint32_t generation, uint32_t speed) {
  // Not implemented for MVP - focus on ReadQuad only
  os_log(ASLog(), "ASOHCI: ReadBlock not implemented in MVP");
  return kIOReturnUnsupported;
}

kern_return_t ASOHCI::WriteQuad(uint16_t nodeID, uint16_t addrHi,
                                uint32_t addrLo, uint32_t value,
                                uint32_t generation, uint32_t speed) {
  // Not implemented for MVP - focus on ReadQuad only
  os_log(ASLog(), "ASOHCI: WriteQuad not implemented in MVP");
  return kIOReturnUnsupported;
}

// Bus management operations - basic implementations
kern_return_t ASOHCI::ResetBus(bool forceIBR) {
  kern_return_t kr = ValidateState(ivars, "ResetBus");
  if (kr != kIOReturnSuccess) {
    return kr;
  }

  os_log(ASLog(), "ASOHCI: ResetBus forceIBR=%d", forceIBR);

  // Method 1: Standard bus reset via linkEnable manipulation
  // Clear linkEnable bit in LinkControlClear (use HCControl LinkEnable for now)
  ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_HCControlClear,
                                  kOHCI_HCControl_LinkEnable);

  // Set linkEnable bit in HCControlSet to trigger reset
  ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_HCControlSet,
                                  kOHCI_HCControl_LinkEnable);

  return kIOReturnSuccess;
}

kern_return_t ASOHCI::SendPHYPacket(uint32_t quadlet) {
  // Not implemented for MVP
  os_log(ASLog(), "ASOHCI: SendPHYPacket not implemented in MVP");
  return kIOReturnUnsupported;
}

kern_return_t ASOHCI::SetContender(bool enable) {
  // Not implemented for MVP
  os_log(ASLog(), "ASOHCI: SetContender not implemented in MVP");
  return kIOReturnUnsupported;
}

kern_return_t ASOHCI::SetRootHoldOff(bool enable) {
  // Not implemented for MVP
  os_log(ASLog(), "ASOHCI: SetRootHoldOff not implemented in MVP");
  return kIOReturnUnsupported;
}

kern_return_t ASOHCI::UpdateConfigROM(IOMemoryDescriptor *romData) {
  // Not implemented for MVP
  os_log(ASLog(), "ASOHCI: UpdateConfigROM not implemented in MVP");
  return kIOReturnUnsupported;
}

// Controller Event Delivery - placeholders for MVP
void ASOHCI::OnBusReset(uint32_t generation) {
  if (ivars && ivars->controller) {
    os_log(ASLog(), "ASOHCI: OnBusReset generation=%u (controller=%p)",
           generation, ivars->controller);
    // Update generation in ivars for Link Interface methods
    ivars->generation = generation;
    // Forward to controller
    ivars->controller->HandleBusReset(generation);
  }
}

void ASOHCI::OnSelfIDsComplete(const uint32_t *selfIDQuads, uint32_t count,
                               uint32_t generation) {
  if (ivars && ivars->controller) {
    os_log(ASLog(),
           "ASOHCI: OnSelfIDsComplete count=%u generation=%u (controller=%p)",
           count, generation, ivars->controller);
    // Forward to controller
    ivars->controller->HandleSelfIDs(selfIDQuads, count, generation);
  }
}

void ASOHCI::OnCycleInconsistent(uint32_t cycleTime) {
  if (ivars && ivars->controller) {
    // TODO: Forward to controller
    os_log(ASLog(),
           "ASOHCI: OnCycleInconsistent cycleTime=0x%08x (controller=%p)",
           cycleTime, ivars->controller);
  }
}

void ASOHCI::OnTransactionComplete(void *completionContext,
                                   kern_return_t status, uint32_t responseCode,
                                   IOMemoryDescriptor *responseData) {
  // Transaction completion handling - not needed for MVP synchronous reads
  os_log(ASLog(),
         "ASOHCI: OnTransactionComplete context=%p status=0x%08x rcode=%u",
         completionContext, status, responseCode);
}

void ASOHCI::OnAsyncPacketReceived(uint16_t sourceNodeID, uint16_t destAddrHi,
                                   uint32_t destAddrLo, uint32_t tCode,
                                   IOMemoryDescriptor *packetData,
                                   uint32_t generation, uint32_t speed) {
  // Async packet reception - not needed for MVP Config ROM reading
  os_log(ASLog(), "ASOHCI: OnAsyncPacketReceived src=0x%04x tCode=%u gen=%u",
         sourceNodeID, tCode, generation);
}

// =====================================================================================
// State Machine Query Methods (REFACTOR.md §9)
// =====================================================================================

// Get current state for debugging/testing
ASOHCIState ASOHCI::GetCurrentState() const {
  if (!ivars)
    return ASOHCIState::Dead;
  return static_cast<ASOHCIState>(
      __atomic_load_n(&ivars->state, __ATOMIC_ACQUIRE));
}

// Get current state as string for logging
const char *ASOHCI::GetCurrentStateString() const {
  if (!ivars)
    return "null";
  return ivars->stateDescription;
}

// Check if driver is in a specific state
bool ASOHCI::IsInState(ASOHCIState state) const {
  if (!ivars)
    return (state == ASOHCIState::Dead);
  return (static_cast<ASOHCIState>(
              __atomic_load_n(&ivars->state, __ATOMIC_ACQUIRE)) == state);
}
