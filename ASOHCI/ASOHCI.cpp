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
static constexpr size_t kPageSize = 4096;                  // Standard page size
static constexpr size_t kMaxAllocation = 16 * 1024 * 1024; // 16MB limit

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
#include <DriverKit/OSMetaClass.h>

// Dispatch queue for deferred self-ID processing (DriverKit equivalent of Linux
// workqueue)
#include <DriverKit/IODispatchQueue.h>

// Generated Header
#include "BridgeLog.hpp"
#include <net.mrmidi.ASFireWire.ASOHCI/ASOHCI.h>

// Include Link API header
// #include "Core/ASOHCILinkAPI.h"
// #include "ASOHCILink.h"

// Concrete ivars definition (workaround for IIG forward declaration issue)
#include "ASOHCIIVars.h"

// Private helper methods (implementation details, not in public interface)
static kern_return_t CreateWorkQueue(ASOHCI_IVars *ivars);
static kern_return_t MapDeviceMemory(ASOHCI_IVars *ivars);
static kern_return_t InitializeManagers(ASOHCI *self, ASOHCI_IVars *ivars);
static kern_return_t InitializeOHCI(ASOHCI *self, ASOHCI_IVars *ivars);
static kern_return_t SetupInterrupts(ASOHCI *self, ASOHCI_IVars *ivars);
static kern_return_t InitializeOHCIHardware(ASOHCI_IVars *ivars);
static kern_return_t DispatchAsync(ASOHCI_IVars *ivars, void (^work)(void));
static kern_return_t DispatchAsyncWithCompletion(ASOHCI_IVars *ivars,
                                                 void (^work)(void),
                                                 void (^completion)(void));

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

  // OSSharedPtr objects automatically initialized to nullptr
  os_log(ASLog(), "ASOHCI: init() completed");
  return true;
}

void ASOHCI::free() {
  os_log(ASLog(), "ASOHCI: free()");
  if (ivars != nullptr) {
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

    // Legacy raw pointers (manual cleanup)
    if (ivars->phyAccess) {
      delete ivars->phyAccess;
      ivars->phyAccess = nullptr;
    }

    // Legacy context cleanup (kept for transition - managed by managers now)
    if (ivars->arRequestContext) {
      delete ivars->arRequestContext;
      ivars->arRequestContext = nullptr;
    }
    if (ivars->arResponseContext) {
      delete ivars->arResponseContext;
      ivars->arResponseContext = nullptr;
    }
    if (ivars->atRequestContext) {
      delete ivars->atRequestContext;
      ivars->atRequestContext = nullptr;
    }
    if (ivars->atResponseContext) {
      delete ivars->atResponseContext;
      ivars->atResponseContext = nullptr;
    }
  }

  // Step 3: Safe deallocation with null-setting
  IOSafeDeleteNULL(ivars, ASOHCI_IVars, 1);
  super::free();
}

// =====================================================================================
// Helper Methods for Dispatch Queue Integration
// =====================================================================================

// Thread-safe dispatch of work to the default queue
static kern_return_t DispatchAsync(ASOHCI_IVars *ivars, void (^work)(void)) {
  if (!ivars || !ivars->defaultQ) {
    os_log(ASLog(), "ASOHCI: Cannot dispatch work - no queue available");
    return kIOReturnNotReady;
  }

  // Use variables for thread-safe capture
  ASOHCI_IVars *blockIv = ivars;
  void (^blockWork)(void) = work;

  // Dispatch work asynchronously with proper block capture
  ivars->defaultQ->DispatchAsync(^{
    if (blockIv && !__atomic_load_n(&blockIv->stopping, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&blockIv->deviceGone, __ATOMIC_ACQUIRE)) {
      blockWork();
    }
  });

  return kIOReturnSuccess;
}

// Thread-safe dispatch of work to the default queue with completion handler
static kern_return_t DispatchAsyncWithCompletion(ASOHCI_IVars *ivars,
                                                 void (^work)(void),
                                                 void (^completion)(void)) {
  if (!ivars || !ivars->defaultQ) {
    os_log(ASLog(), "ASOHCI: Cannot dispatch work - no queue available");
    return kIOReturnNotReady;
  }

  // Use variables for thread-safe capture
  ASOHCI_IVars *blockIv = ivars;
  void (^blockWork)(void) = work;
  void (^blockCompletion)(void) = completion;

  // Dispatch work asynchronously with completion
  ivars->defaultQ->DispatchAsync(^{
    if (blockIv && !__atomic_load_n(&blockIv->stopping, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&blockIv->deviceGone, __ATOMIC_ACQUIRE)) {
      blockWork();
      if (blockCompletion) {
        blockCompletion();
      }
    }
  });

  return kIOReturnSuccess;
}

// =====================================================================================
// Error Handling and Cleanup Patterns
// =====================================================================================

// Comprehensive cleanup helper for error recovery
void ASOHCI::CleanupOnError() {
  os_log(ASLog(), "ASOHCI: CleanupOnError - performing comprehensive cleanup");

  // Step 1: Stop all managers in reverse order of initialization
  if (ivars->arManager) {
    ivars->arManager->Stop();
    ivars->arManager.reset();
  }
  if (ivars->atManager) {
    ivars->atManager->Stop();
    ivars->atManager.reset();
  }
  if (ivars->irManager) {
    ivars->irManager->StopAll();
    ivars->irManager.reset();
  }
  if (ivars->itManager) {
    ivars->itManager->StopAll();
    ivars->itManager.reset();
  }

  // Step 2: Clean up DMA resources
  ivars->configROMDMA.reset();
  ivars->configROMMap.reset();
  ivars->configROMBuffer.reset();
  ivars->selfIDDMA.reset();
  ivars->selfIDMap.reset();
  ivars->selfIDBuffer.reset();

  // Step 3: Clean up device resources
  if (ivars->intSource) {
    ivars->intSource->SetEnableWithCompletion(false, nullptr);
    ivars->intSource.reset();
  }
  ivars->defaultQ.reset();
  // bar0Map not used in DriverKit - using direct memory access

  // Step 4: Clean up managers and helpers
  if (ivars->selfIDManager) {
    ivars->selfIDManager->Teardown();
    ivars->selfIDManager.reset();
  }
  if (ivars->configROMManager) {
    ivars->configROMManager->Teardown();
    ivars->configROMManager.reset();
  }
  ivars->topology.reset();
  ivars->regs.reset();
  ivars->interruptRouter.reset();

  // Step 5: Clean up legacy resources
  if (ivars->phyAccess) {
    delete ivars->phyAccess;
    ivars->phyAccess = nullptr;
  }
  if (ivars->arRequestContext) {
    delete ivars->arRequestContext;
    ivars->arRequestContext = nullptr;
  }
  if (ivars->arResponseContext) {
    delete ivars->arResponseContext;
    ivars->arResponseContext = nullptr;
  }
  if (ivars->atRequestContext) {
    delete ivars->atRequestContext;
    ivars->atRequestContext = nullptr;
  }
  if (ivars->atResponseContext) {
    delete ivars->atResponseContext;
    ivars->atResponseContext = nullptr;
  }

  // Step 6: Close PCI device if open
  if (ivars->pciDevice) {
    ivars->pciDevice->Close(this, 0);
    ivars->pciDevice.reset();
  }

  os_log(ASLog(), "ASOHCI: CleanupOnError - cleanup completed");
}

// =====================================================================================
// Helper Methods for Start()
// =====================================================================================

kern_return_t ASOHCI::CreateWorkQueue() {
  IODispatchQueue *queue = nullptr;
  kern_return_t kr = IODispatchQueue::Create("ASOHCI.WorkQueue", 0, 0, &queue);
  if (kr != kIOReturnSuccess) {
    os_log(ASLog(), "ASOHCI: Failed to create work queue: 0x%08x", kr);
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
  kern_return_t result = kIOReturnSuccess;

  // AR Manager
  ivars->arManager = OSSharedPtr(new ASOHCIARManager(), OSNoRetain);
  if (ivars->arManager.get() == nullptr) {
    os_log(ASLog(), "ASOHCI: Failed to allocate AR Manager");
    return kIOReturnNoMemory;
  }

  // AT Manager
  ivars->atManager = OSSharedPtr(new ASOHCIATManager(), OSNoRetain);
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
  ivars->selfIDManager = OSSharedPtr(new SelfIDManager(), OSNoRetain);
  if (!ivars->selfIDManager) {
    os_log(ASLog(), "ASOHCI: Failed to allocate SelfIDManager");
    return kIOReturnNoMemory;
  }

  // Initialize Topology
  ivars->topology = OSSharedPtr(new Topology(), OSNoRetain);
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
    os_log(ASLog(), "ASOHCI: BusOptions updated 0x%08x->0x%08x", origBo, bo);
  }

  // Provisional NodeID
  ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_NodeID, 0x0000FFC0);
  os_log(ASLog(), "ASOHCI: Provisional NodeID set to 0x0000FFC0");

  // Enable link and reception
  ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_HCControlSet,
                                  kOHCI_HCControl_programPhyEnable);
  ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_HCControlSet,
                                  kOHCI_HCControl_LinkEnable);
  ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_LinkControlSet,
                                  (kOHCI_LC_RcvSelfID | kOHCI_LC_RcvPhyPkt));
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
  os_log(ASLog(), "ASOHCI: Comprehensive interrupt mask set: 0x%08x", irqs);

  // Final link activation
  ivars->pciDevice->MemoryWrite32(
      ivars->barIndex, kOHCI_HCControlSet,
      (kOHCI_HCControl_LinkEnable | kOHCI_HCControl_BIBimageValid));
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
  os_log(ASLog(), "ASOHCI: Start() begin bring-up");

  // Step 1: Store provider with smart pointer and retain
  ivars->pciDevice =
      OSSharedPtr(OSDynamicCast(IOPCIDevice, provider), OSRetain);
  if (ivars->pciDevice.get() == nullptr) {
    os_log(ASLog(), "ASOHCI: Provider is not IOPCIDevice");
    CleanupOnError();
    return kIOReturnBadArgument;
  }

  // Step 2: Create dispatch queue
  kr = CreateWorkQueue();
  if (kr != kIOReturnSuccess) {
    CleanupOnError();
    return kr;
  }

  // Step 3: Map device memory
  kr = MapDeviceMemory();
  if (kr != kIOReturnSuccess) {
    CleanupOnError();
    return kr;
  }

  // Step 4: Initialize managers
  kr = InitializeManagers();
  if (kr != kIOReturnSuccess) {
    CleanupOnError();
    return kr;
  }

  // Step 5: Continue with OHCI initialization sequence
  kr = InitializeOHCI();
  if (kr != kIOReturnSuccess) {
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
  os_log(ASLog(), "ASOHCI: Stop begin");

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
    ivars->intSource->SetEnableWithCompletion(false, nullptr);
    os_log(ASLog(), "ASOHCI: Interrupt source disabled");
  }

  // 2) Wait for any pending interrupt processing to complete
  // This is critical to prevent race conditions during teardown
  if (ivars) {
    // Give interrupt handlers a moment to complete any in-flight processing
    IOSleep(10); // 10ms should be sufficient for any pending interrupts
    os_log(ASLog(), "ASOHCI: Waited for pending interrupts to complete");
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

  // 4) Disable PCI bus mastering BEFORE soft reset
  if (auto pci = OSDynamicCast(IOPCIDevice, provider)) {
    uint16_t cmd = 0;
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &cmd);
    uint16_t newCmd = (uint16_t)(cmd & ~kIOPCICommandBusMaster);
    if (newCmd != cmd) {
      pci->ConfigurationWrite16(kIOPCIConfigurationOffsetCommand, newCmd);
      os_log(ASLog(), "ASOHCI: PCI Bus Mastering disabled");
    }
  }

  // 5) Hardware soft reset only if device is present
  if (devicePresent && ivars && ivars->pciDevice) {
    os_log(ASLog(), "ASOHCI: Quiescing hardware...");

    // Clear and mask ALL interrupts to prevent any further processing
    ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IntMaskClear,
                                    0xFFFFFFFFu);
    ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IntEventClear,
                                    0xFFFFFFFFu);
    ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IsoXmitIntEventClear,
                                    0xFFFFFFFFu);
    ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IsoRecvIntEventClear,
                                    0xFFFFFFFFu);
    ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IsoXmitIntMaskClear,
                                    0xFFFFFFFFu);
    ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IsoRecvIntMaskClear,
                                    0xFFFFFFFFu);

    // Drop link control enables
    ivars->pciDevice->MemoryWrite32(
        ivars->barIndex, kOHCI_LinkControlClear,
        (kOHCI_LC_RcvSelfID | kOHCI_LC_RcvPhyPkt | kOHCI_LC_CycleTimerEnable));

    // Soft reset to quiesce the controller
    ivars->pciDevice->MemoryWrite32(
        ivars->barIndex, kOHCI_HCControlClear,
        (kOHCI_HCControl_LinkEnable | kOHCI_HCControl_aPhyEnhanceEnable));
    ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_HCControlSet,
                                    kOHCI_HCControl_SoftReset);

    // Wait for soft reset to complete
    bool resetComplete = false;
    for (int i = 0; i < 100; i++) { // 100ms timeout
      uint32_t hcControl = 0;
      // FIX: Read from base register, not Set/Clear
      ivars->pciDevice->MemoryRead32(ivars->barIndex, kOHCI_HCControl,
                                     &hcControl);
      if (!(hcControl & kOHCI_HCControl_SoftReset)) {
        resetComplete = true;
        break;
      }
      IOSleep(1);
    }

    if (resetComplete) {
      os_log(ASLog(), "ASOHCI: Hardware quiesced successfully");
    } else {
      os_log(ASLog(), "ASOHCI: WARNING - Hardware quiesce timeout");
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
    ivars->pciDevice = nullptr;
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
      delete ivars->phyAccess;
      ivars->phyAccess = nullptr;
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
    ivars->intSource->release();
    ivars->intSource = nullptr;
    os_log(ASLog(), "ASOHCI: Interrupt source released");
  }

  // 11) Set deviceGone flag LAST
  if (ivars) {
    __atomic_store_n(&ivars->deviceGone, true, __ATOMIC_RELEASE);
    os_log(ASLog(), "ASOHCI: Stop completed - device marked as gone");
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

  // Use variables for thread-safe access to ivars on dispatch queue
  // (Variables not used in this method)

  // Double-check PCI device is still valid (defensive programming)
  if (!ivars->pciDevice) {
    os_log(ASLog(), "ASOHCI: Interrupt with null PCI device - ignoring");
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
      ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IntMaskSet,
                                      kOHCI_Int_BusReset);
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
  if (clearMask)
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
    ivars->pciDevice->MemoryRead32(ivars->barIndex, kOHCI_SelfIDCount,
                                   &selfIDCount1);
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
    if ((intEvent & kOHCI_Int_ReqTxComplete) && ivars->atRequestContext) {
      ivars->atRequestContext->HandleInterrupt();
    }
    if ((intEvent & kOHCI_Int_RespTxComplete) && ivars->atResponseContext) {
      ivars->atResponseContext->HandleInterrupt();
    }
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
      ivars->pciDevice->MemoryRead32(ivars->barIndex, kOHCI_IsoXmitIntEventSet,
                                     &txMask);
      if (ivars->interruptRouter)
        ivars->interruptRouter->OnIsoTxMask(txMask);
      if (txMask) {
        // Clear the context-specific events
        ivars->pciDevice->MemoryWrite32(ivars->barIndex,
                                        kOHCI_IsoXmitIntEventClear, txMask);
      }
    }

    // IR Manager interrupt handling (OHCI 1.1 §6.4)
    if (intEvent & kOHCI_Int_IsochRx) {
      uint32_t rxMask = 0;
      ivars->pciDevice->MemoryRead32(ivars->barIndex, kOHCI_IsoRecvIntEventSet,
                                     &rxMask);
      if (ivars->interruptRouter)
        ivars->interruptRouter->OnIsoRxMask(rxMask);
      if (rxMask) {
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
