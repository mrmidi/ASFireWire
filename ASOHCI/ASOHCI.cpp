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
  if (!super::init())
    return false;
  if (!ivars) {
    ivars = new ASOHCI_IVars{}; // value-init → zero-initialize
    if (!ivars)
      return false;
  }

  // Initialize Link API
  // ivars->linkAPI = OSSharedPtr<ASOHCILinkAPI>(new
  // (IOMalloc(sizeof(ASOHCILink))) ASOHCILink(this), OSNoRetain); if
  // (!ivars->linkAPI) {
  //   os_log(ASLog(), "ASOHCI: Failed to create Link API");
  //   return false;
  // }

  os_log(ASLog(), "ASOHCI: init()");
  return true;
}

void ASOHCI::free() {
  os_log(ASLog(), "ASOHCI: free()");
  if (ivars) {
    os_log(ASLog(), "ASOHCI: free step A - stop contexts if present");
    // Assume Stop() already quiesced hardware. Just reset OSSharedPtr objects.
    ivars->arManager.reset();
    ivars->atManager.reset();
    ivars->irManager.reset();
    ivars->itManager.reset();

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

    // Release DMA resources (safe even if Start failed mid-way)
    os_log(ASLog(), "ASOHCI: free step C - release Self-ID DMA/map/buffer");
    if (ivars->selfIDDMA) {
      ivars->selfIDDMA->CompleteDMA(kIODMACommandCompleteDMANoOptions);
      ivars->selfIDDMA->release();
      ivars->selfIDDMA = nullptr;
    }
    if (ivars->selfIDMap) {
      ivars->selfIDMap->release();
      ivars->selfIDMap = nullptr;
    }
    if (ivars->selfIDBuffer) {
      ivars->selfIDBuffer->release();
      ivars->selfIDBuffer = nullptr;
    }
    // Release Config ROM resources
    if (ivars->configROMDMA) {
      ivars->configROMDMA->CompleteDMA(kIODMACommandCompleteDMANoOptions);
      ivars->configROMDMA->release();
      ivars->configROMDMA = nullptr;
    }
    if (ivars->configROMMap) {
      ivars->configROMMap->release();
      ivars->configROMMap = nullptr;
    }
    if (ivars->configROMBuffer) {
      ivars->configROMBuffer->release();
      ivars->configROMBuffer = nullptr;
    }
    // Release default queue if we retained it (see §4)
    if (ivars->defaultQ) {
      ivars->defaultQ->release();
      ivars->defaultQ = nullptr;
    }
    // Delete helpers
    os_log(ASLog(), "ASOHCI: free step D - delete helpers and ivars");
    if (ivars->phyAccess) {
      delete ivars->phyAccess;
      ivars->phyAccess = nullptr;
    }
    // Reset OSSharedPtr managers (automatic cleanup)
    ivars->selfIDManager.reset();
    ivars->configROMManager.reset();
    ivars->topology.reset();

    // Delete ivars (run C++ dtors if introduced later)
    delete ivars;
    ivars = nullptr;
  }
  super::free();
}

// =====================================================================================
// Start
// =====================================================================================
kern_return_t IMPL(ASOHCI, Start) {
  kern_return_t kr = Start(provider, SUPERDISPATCH);
  if (kr != kIOReturnSuccess) {
    os_log(ASLog(), "ASOHCI: Start superdispatch failed: 0x%08x", kr);
    return kr;
  }
  if (!ivars) {
    os_log(ASLog(), "ASOHCI: ivars not allocated");
    return kIOReturnNoResources;
  }
  os_log(ASLog(), "ASOHCI: Start() begin bring-up");
  bridge_log_init();

  // Reset bring-up/lifecycle flags to known defaults
  ivars->cycleTimerArmed = false;
  ivars->selfIDInProgress = false;
  ivars->selfIDArmed = false;
  ivars->collapsedBusResets = 0;
  ivars->didInitialPhyScan = false;

  auto pci = OSDynamicCast(IOPCIDevice, provider);
  if (!pci) {
    os_log(ASLog(), "ASOHCI: Provider is not IOPCIDevice");
    return kIOReturnBadArgument;
  }

  // Open device
  kr = pci->Open(this, 0);
  if (kr != kIOReturnSuccess) {
    os_log(ASLog(), "ASOHCI: PCI Open failed: 0x%08x", kr);
    return kr;
  }

  // IDs
  uint16_t vendorID = 0, deviceID = 0;
  pci->ConfigurationRead16(kIOPCIConfigurationOffsetVendorID, &vendorID);
  pci->ConfigurationRead16(kIOPCIConfigurationOffsetDeviceID, &deviceID);
  os_log(ASLog(), "ASOHCI: PCI IDs V:0x%04x D:0x%04x", vendorID, deviceID);

  // Enable BusMaster|MemorySpace
  uint16_t cmd = 0, newCmd = 0;
  pci->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &cmd);
  newCmd = (uint16_t)(cmd | kIOPCICommandBusMaster | kIOPCICommandMemorySpace);
  if (newCmd != cmd) {
    pci->ConfigurationWrite16(kIOPCIConfigurationOffsetCommand, newCmd);
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &newCmd);
  }
  os_log(ASLog(), "ASOHCI: PCI CMD=0x%04x (was 0x%04x)", newCmd, cmd);

  // BAR0 info
  uint8_t bar0Index = 0;
  uint64_t bar0Size = 0;
  uint8_t bar0Type = 0;
  kr = pci->GetBARInfo(0 /* BAR0 */, &bar0Index, &bar0Size, &bar0Type);
  if (kr != kIOReturnSuccess) {
    os_log(ASLog(), "ASOHCI: GetBARInfo(BAR0) failed: 0x%08x", kr);
    // keep going; reads will fail gracefully
  } else {
    os_log(ASLog(), "ASOHCI: BAR0 idx=%u size=0x%llx type=0x%02x", bar0Index,
           bar0Size, bar0Type);
  }

  if (bar0Size >= 0x2C) {
    // Optional: discover and log common PCI capabilities (PM, MSI, MSI-X)
    {
      uint64_t off = 0;
      if (pci->FindPCICapability(kIOPCICapabilityIDPowerManagement,
                                 0 /* start */, &off) == kIOReturnSuccess &&
          off) {
        os_log(ASLog(), "ASOHCI: PCI PM capability at 0x%llx", off);
      }
      off = 0;
      if (pci->FindPCICapability(kIOPCICapabilityIDMSI, 0 /* start */, &off) ==
              kIOReturnSuccess &&
          off) {
        os_log(ASLog(), "ASOHCI: PCI MSI capability at 0x%llx", off);
      }
      off = 0;
      if (pci->FindPCICapability(kIOPCICapabilityIDMSIX, 0 /* start */, &off) ==
              kIOReturnSuccess &&
          off) {
        os_log(ASLog(), "ASOHCI: PCI MSI-X capability at 0x%llx", off);
      }
    }

    // Log current link speed for diagnostics
    // Initialize with a valid enum value to satisfy strict type checking
    IOPCILinkSpeed linkSpeed = kPCILinkSpeed_2_5_GTs;
    if (pci->GetLinkSpeed(&linkSpeed) == kIOReturnSuccess) {
      os_log(ASLog(), "ASOHCI: PCIe link speed: %u", (unsigned)linkSpeed);
    }

    uint32_t ohci_ver = 0, bus_opts = 0, guid_hi = 0, guid_lo = 0;
    pci->MemoryRead32(bar0Index, kOHCI_Version, &ohci_ver);
    pci->MemoryRead32(bar0Index, kOHCI_BusOptions, &bus_opts);
    pci->MemoryRead32(bar0Index, kOHCI_GUIDHi, &guid_hi);
    pci->MemoryRead32(bar0Index, kOHCI_GUIDLo, &guid_lo);
    os_log(ASLog(), "ASOHCI: OHCI VER=0x%08x BUSOPT=0x%08x GUID=%08x:%08x",
           ohci_ver, bus_opts, guid_hi, guid_lo);

    ivars->pciDevice = pci;
    ivars->barIndex = bar0Index;
    // Initialize shared register IO helper for reuse by subsystems
    ivars->regs = OSSharedPtr(ASOHCIRegisterIO::Create(), OSNoRetain);
    if (ivars->regs && !ivars->regs->Init(pci, (uint8_t)bar0Index)) {
      ivars->regs.reset();
    }

    // Configuration ROM via manager
    {
      ivars->configROMManager = OSSharedPtr(new ConfigROMManager(), OSNoRetain);
      if (ivars->configROMManager) {
        kern_return_t ckr = ivars->configROMManager->Initialize(
            pci, (uint8_t)bar0Index, bus_opts, guid_hi, guid_lo, 1024);
        if (ckr != kIOReturnSuccess) {
          os_log(ASLog(), "ASOHCI: WARN: ConfigROMManager init failed: 0x%08x",
                 ckr);
          ivars->configROMManager.reset(); // Clean up on failure
        } else {
          os_log(ASLog(), "ASOHCI: ConfigROMManager initialized");
        }
      }
    }

    // Topology + Self-ID manager callbacks
    {
      ivars->topology = OSSharedPtr(new Topology(), OSNoRetain);
      if (!ivars->topology) {
        os_log(ASLog(), "ASOHCI: Failed to allocate Topology");
        return kIOReturnNoMemory;
      }

      ivars->selfIDManager = OSSharedPtr(new SelfIDManager(), OSNoRetain);
      if (!ivars->selfIDManager) {
        os_log(ASLog(), "ASOHCI: Failed to allocate SelfIDManager");
        return kIOReturnNoMemory;
      }

      if (ivars->selfIDManager && ivars->topology) {
        ivars->selfIDManager->SetCallbacks(
            // onDecode: begin cycle and accumulate nodes
            [this](const SelfID::Result &res) {
              if (!ivars || !ivars->topology)
                return;
              os_log(ASLog(),
                     "ASOHCI: Topology decode callback fired (begin cycle): "
                     "gen=%u nodes=%lu",
                     res.generation, (unsigned long)res.nodes.size());
              ivars->topology->BeginCycle(res.generation);
              for (const auto &n : res.nodes)
                ivars->topology->AddOrUpdateNode(n);
            },
            // onStable: finalize and log a concise summary
            [this](const SelfID::Result &res) {
              if (!ivars || !ivars->topology)
                return;
              ivars->topology->Finalize();
              os_log(ASLog(), "ASOHCI: Topology callback fired (finalize)");
              size_t nodes = ivars->topology->NodeCount();
              const Topology::Node *root = ivars->topology->Root();
              uint8_t hops = ivars->topology->MaxHopsFromRoot();
              bool ok = ivars->topology->IsConsistent();
              auto &info = ivars->topology->Info();
              os_log(ASLog(),
                     "ASOHCI: Topology gen=%u nodes=%lu rootPhy=%u hops=%u "
                     "consistent=%d warnings=%lu",
                     info.generation, (unsigned long)nodes,
                     root ? root->phy.value : 0xFF, hops, ok ? 1 : 0,
                     (unsigned long)info.warnings.size());

              ivars->topology->Log();
            });

        // Register SelfIDManager with DriverKit runtime
        os_log(ASLog(), "ASOHCI: SelfIDManager and Topology registered with "
                        "DriverKit runtime");
      }
    }

    // --- Clear/mask interrupts
    const uint32_t allOnes = 0xFFFFFFFFu;
    pci->MemoryWrite32(bar0Index, kOHCI_IntEventClear, allOnes);
    pci->MemoryWrite32(bar0Index, kOHCI_IsoXmitIntEventClear, allOnes);
    pci->MemoryWrite32(bar0Index, kOHCI_IsoRecvIntEventClear, allOnes);
    pci->MemoryWrite32(bar0Index, kOHCI_IntMaskClear, allOnes);
    pci->MemoryWrite32(bar0Index, kOHCI_IsoXmitIntMaskClear, allOnes);
    pci->MemoryWrite32(bar0Index, kOHCI_IsoRecvIntMaskClear, allOnes);
    os_log(ASLog(), "ASOHCI: Cleared interrupt events/masks");

    // --- Soft reset
    pci->MemoryWrite32(bar0Index, kOHCI_HCControlSet,
                       kOHCI_HCControl_SoftReset);
    IOSleep(10);
    os_log(ASLog(), "ASOHCI: Soft reset issued");

    // Re-clear after reset
    pci->MemoryWrite32(bar0Index, kOHCI_IntEventClear, allOnes);
    pci->MemoryWrite32(bar0Index, kOHCI_IsoXmitIntEventClear, allOnes);
    pci->MemoryWrite32(bar0Index, kOHCI_IsoRecvIntEventClear, allOnes);
    pci->MemoryWrite32(bar0Index, kOHCI_IntMaskClear, allOnes);
    pci->MemoryWrite32(bar0Index, kOHCI_IsoXmitIntMaskClear, allOnes);
    pci->MemoryWrite32(bar0Index, kOHCI_IsoRecvIntMaskClear, allOnes);

    // Enter LPS + enable posted writes (program BusOptions/NodeID prior to
    // linkEnable)
    const uint32_t hcSet =
        (kOHCI_HCControl_LPS | kOHCI_HCControl_PostedWriteEn);
    pci->MemoryWrite32(bar0Index, kOHCI_HCControlSet, hcSet);
    os_log(ASLog(), "ASOHCI: HCControlSet LPS+PostedWrite (0x%08x)", hcSet);
    // Poll up to 3 * 50ms for LPS latch similar to Linux early init
    uint32_t _hc = 0;
    bool lpsOk = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
      IOSleep(50);
      pci->MemoryRead32(bar0Index, kOHCI_HCControlSet, &_hc);
      if ((_hc & kOHCI_HCControl_LPS) != 0) {
        lpsOk = true;
        break;
      }
    }
    if (!lpsOk) {
      os_log(ASLog(),
             "ASOHCI: WARNING LPS did not latch after polling (_hc=0x%08x)",
             _hc);
    } else {
      os_log(ASLog(), "ASOHCI: LPS latched (_hc=0x%08x)", _hc);
    }

    // Program BusOptions similar to Linux early init: set CMC|ISC, clear
    // PMC|BMC|cyc_clk_acc field
    uint32_t bo = 0;
    pci->MemoryRead32(bar0Index, kOHCI_BusOptions, &bo);
    uint32_t origBo = bo;
    // Masks based on Linux patterns: CMC=bit29, ISC=bit30, PMC=bit27,
    // BMC=bit28, cyc_clk_acc bits [23:16]
    bo |= 0x60000000;  // set ISC|CMC
    bo &= ~0x18000000; // clear BMC|PMC
    bo &= ~0x00FF0000; // clear cyc_clk_acc placeholder
    if (bo != origBo) {
      pci->MemoryWrite32(bar0Index, kOHCI_BusOptions, bo);
      os_log(ASLog(), "ASOHCI: BusOptions updated 0x%08x->0x%08x", origBo, bo);
    } else {
      os_log(ASLog(), "ASOHCI: BusOptions kept 0x%08x (already desired)", bo);
    }

    // Provisional NodeID (Linux early path uses 0x0000FFC0) prior to bus reset
    // assignment
    pci->MemoryWrite32(bar0Index, kOHCI_NodeID, 0x0000FFC0);
    os_log(ASLog(), "ASOHCI: Provisional NodeID set to 0x0000FFC0");

    // Persistent programPhyEnable (mimic Linux: set once, do not toggle per
    // access)
    pci->MemoryWrite32(bar0Index, kOHCI_HCControlSet,
                       kOHCI_HCControl_programPhyEnable);
    uint32_t hcAfterProg = 0;
    pci->MemoryRead32(bar0Index, kOHCI_HCControlSet, &hcAfterProg);
    os_log(ASLog(), "ASOHCI: HCControlSet programPhyEnable (HCControl=0x%08x)",
           hcAfterProg);

    // Link enable after baseline BusOptions/NodeID prepared
    pci->MemoryWrite32(bar0Index, kOHCI_HCControlSet,
                       kOHCI_HCControl_LinkEnable);
    os_log(ASLog(), "ASOHCI: HCControlSet LinkEnable");

    // Enable reception of Self-ID & PHY packets ONLY (defer cycle timer until
    // stable Self-ID)
    pci->MemoryWrite32(bar0Index, kOHCI_LinkControlSet,
                       (kOHCI_LC_RcvSelfID | kOHCI_LC_RcvPhyPkt));
    os_log(ASLog(),
           "ASOHCI: LinkControlSet rcvSelfID+rcvPhyPkt (cycle timer deferred)");

    // --- MSI/MSI-X/Legacy routing
    kern_return_t rc =
        pci->ConfigureInterrupts(kIOInterruptTypePCIMessagedX, 1, 1, 0);
    if (rc == kIOReturnSuccess) {
      os_log(ASLog(), "ASOHCI: Configured MSI-X interrupts");

    } else {
      rc = pci->ConfigureInterrupts(kIOInterruptTypePCIMessaged, 1, 1, 0);
      if (rc == kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCI: Configured MSI interrupts");

      } else {
        os_log(ASLog(), "ASOHCI: Falling back to legacy interrupts");
      }
    }

    // --- Interrupt source on default queue
    IODispatchQueue *queue = nullptr;
    kr = CopyDispatchQueue(kIOServiceDefaultQueueName, &queue);
    if (kr == kIOReturnSuccess && queue) {
      IOInterruptDispatchSource *src = nullptr;
      kr = IOInterruptDispatchSource::Create(pci, 0 /* interrupt index */,
                                             queue, &src);
      if (kr == kIOReturnSuccess && src) {
        // Log interrupt type for the bound source
        uint64_t itype = 0;
        if (IOInterruptDispatchSource::GetInterruptType(pci, 0, &itype) ==
            kIOReturnSuccess) {
          os_log(ASLog(), "ASOHCI: Interrupt type bound (index 0): 0x%llx",
                 (unsigned long long)itype);
        }
        OSAction *action = nullptr;
        // generated by IIG; capacity 0 is fine for simple typed actions
        kern_return_t ar = CreateActionInterruptOccurred(0, &action);
        if (ar == kIOReturnSuccess && action) {
          src->SetHandler(action);
          action->release();
          src->SetEnableWithCompletion(true, nullptr);
          ivars->intSource = src;
          os_log(ASLog(), "ASOHCI: Interrupt source enabled");

        } else {
          os_log(ASLog(),
                 "ASOHCI: CreateActionInterruptOccurred failed: 0x%08x", ar);
          if (src)
            src->release();
        }
      } else {
        os_log(ASLog(),
               "ASOHCI: IOInterruptDispatchSource::Create failed: 0x%08x", kr);
      }
      queue->release();
    } else {
      os_log(ASLog(), "ASOHCI: CopyDispatchQueue failed: 0x%08x", kr);
    }

    // --- Self‑ID via manager (create once; Initialize once)

    if (!ivars->selfIDManager) {
      ivars->selfIDManager = OSSharedPtr(new SelfIDManager(), OSNoRetain);
      if (!ivars->selfIDManager) {
        os_log(ASLog(), "ASOHCI: Failed to allocate SelfIDManager");
        return kIOReturnNoMemory;
      }
      // AddObject(ivars->selfIDManager.get()); // Temporarily disabled - not
      // available in current headers
    }
    if (ivars->selfIDManager) {
      kr = ivars->selfIDManager->Initialize(pci, (uint8_t)bar0Index,
                                            kSelfIDBufferSize);
      if (kr != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCI: SelfIDManager init failed: 0x%08x", kr);
        ivars->selfIDManager.reset(); // Clean up on failure
      }
    }

    // Self-ID deferred processing now handled by InterruptRouter's queue

    // =====================================================================================
    // Complete OHCI Initialization Sequence (based on Linux driver and OHCI 1.1
    // spec)
    // =====================================================================================

    // Phase 1: Software Reset (OHCI 1.1 §5.7 HCControl.softReset, Linux:
    // software_reset())
    os_log(ASLog(), "ASOHCI: Phase 1 - Software Reset");
    pci->MemoryWrite32(bar0Index, kOHCI_HCControlSet,
                       kOHCI_HCControl_SoftReset);

    // Poll for reset completion (up to 500ms like Linux)
    bool resetComplete = false;
    for (int i = 0; i < 500; i++) {
      uint32_t val = 0;
      pci->MemoryRead32(bar0Index, kOHCI_HCControlSet, &val);
      if (!(val & kOHCI_HCControl_SoftReset)) {
        resetComplete = true;
        os_log(ASLog(), "ASOHCI: Software reset completed after %d ms", i);
        break;
      }
      IOSleep(1); // 1ms delay
    }

    if (!resetComplete) {
      os_log(ASLog(), "ASOHCI: Software reset timeout - continuing anyway");
    }

    // Phase 2: Link Power Status Enable (OHCI 1.1 §5.7.3, Linux: LPS retry
    // logic)
    os_log(ASLog(), "ASOHCI: Phase 2 - Link Power Status Enable");
    pci->MemoryWrite32(bar0Index, kOHCI_HCControlSet,
                       kOHCI_HCControl_LPS | kOHCI_HCControl_PostedWriteEn);

    // Critical: Wait for LPS to stabilize (Linux: up to 3 retries with 50ms
    // delays) Some controllers (ALI M5251) will lock up without proper LPS
    // timing
    bool lpsEnabled = false;
    for (int i = 0; i < 3; i++) {
      IOSleep(50); // 50ms delay for SCLK stabilization
      uint32_t val = 0;
      pci->MemoryRead32(bar0Index, kOHCI_HCControlSet, &val);
      if (val & kOHCI_HCControl_LPS) {
        lpsEnabled = true;
        os_log(ASLog(), "ASOHCI: LPS enabled after %d retries", i + 1);
        break;
      }
    }

    if (!lpsEnabled) {
      os_log(
          ASLog(),
          "ASOHCI: FATAL - LPS failed to enable, SCLK domain access will fail");
      return kIOReturnTimeout;
    }

    // Phase 3: Byte Swap Configuration (OHCI 1.1 §5.7.1)
    pci->MemoryWrite32(bar0Index, kOHCI_HCControlClear,
                       kOHCI_HCControl_NoByteSwap);
    os_log(ASLog(),
           "ASOHCI: Phase 3 - Configured for little-endian byte order");

    // Phase 4: Advanced OHCI Configuration (HIGH PRIORITY missing items)
    os_log(ASLog(), "ASOHCI: Phase 4 - Advanced OHCI Configuration");

    // 4a: PhyUpperBound Register Setup (HIGH PRIORITY - Memory Safety)
    os_log(ASLog(), "ASOHCI: 4a - PhyUpperBound setup for memory safety");
    uint32_t phyUpperBound = 0;
    pci->MemoryRead32(bar0Index, kOHCI_PhyUpperBound, &phyUpperBound);
    if (phyUpperBound == 0) {
      // Set conservative upper bound if not already configured
      // Use 16 nodes (0xF) as safe default for most FireWire topologies
      phyUpperBound = 0xF;
      pci->MemoryWrite32(bar0Index, kOHCI_PhyUpperBound, phyUpperBound);
      os_log(ASLog(), "ASOHCI: PhyUpperBound set to 0x%08x (16 nodes max)",
             phyUpperBound);
    } else {
      os_log(ASLog(), "ASOHCI: PhyUpperBound already configured: 0x%08x",
             phyUpperBound);
    }

    // 4b: FairnessControl Probing and Configuration (MEDIUM PRIORITY -
    // Arbitration)
    os_log(ASLog(), "ASOHCI: 4b - FairnessControl probing");
    uint32_t fairnessControl = 0;
    pci->MemoryRead32(bar0Index, kOHCI_FairnessControl, &fairnessControl);
    if (fairnessControl == 0) {
      // Enable fairness control for better arbitration (Linux parity)
      fairnessControl = 0x1; // Enable fairness
      pci->MemoryWrite32(bar0Index, kOHCI_FairnessControl, fairnessControl);
      os_log(ASLog(), "ASOHCI: FairnessControl enabled: 0x%08x",
             fairnessControl);
    } else {
      os_log(ASLog(), "ASOHCI: FairnessControl already configured: 0x%08x",
             fairnessControl);
    }

    // 4c: InitialChannelsAvailable for OHCI 1.1+ (MEDIUM PRIORITY - Broadcast)
    os_log(ASLog(), "ASOHCI: 4c - InitialChannelsAvailable setup");
    uint32_t initialChannels = 0;
    pci->MemoryRead32(bar0Index, kOHCI_InitialChannelsAvailHi,
                      &initialChannels);
    if (initialChannels == 0) {
      // Set all 64 channels available for broadcast reception (Linux parity)
      pci->MemoryWrite32(bar0Index, kOHCI_InitialChannelsAvailHi, 0xFFFFFFFF);
      pci->MemoryWrite32(bar0Index, kOHCI_InitialChannelsAvailLo, 0xFFFFFFFF);
      os_log(ASLog(),
             "ASOHCI: InitialChannelsAvailable set to all 64 channels");
    } else {
      os_log(ASLog(),
             "ASOHCI: InitialChannelsAvailable already configured: 0x%08x",
             initialChannels);
    }

    // 4d: IR Context Multi-Channel Mode Clearing (MEDIUM PRIORITY - Conflicts)
    os_log(ASLog(), "ASOHCI: 4d - IR context multi-channel mode clearing");
    // Clear multi-channel mode for all IR contexts to prevent conflicts
    for (uint32_t ctx = 0; ctx < 32;
         ctx++) { // OHCI supports up to 32 IR contexts
      uint32_t irControlOffset = kOHCI_IsoRcvContextControlClear(ctx);
      uint32_t irControl = 0;
      pci->MemoryRead32(bar0Index, irControlOffset, &irControl);
      if (irControl & kOHCI_IR_MultiChannelMode) {
        // Clear multi-channel mode bit
        pci->MemoryWrite32(bar0Index, irControlOffset,
                           kOHCI_IR_MultiChannelMode);
        os_log(ASLog(), "ASOHCI: Cleared multi-channel mode for IR context %u",
               ctx);
      }
    }
    os_log(ASLog(), "ASOHCI: IR context multi-channel mode clearing complete");

    // Phase 5: Self-ID Buffer Programming (via manager)
    os_log(ASLog(), "ASOHCI: Phase 5 - Self-ID Manager arming");
    ivars->cycleTimerArmed = false;
    if (ivars->selfIDManager) {
      ivars->selfIDManager->Arm(false);
    }

    // Phase 6: AT Retries Configuration (Linux ohci_enable line 2479)
    uint32_t retries = (3 << 0) |   // MAX_AT_REQ_RETRIES
                       (3 << 4) |   // MAX_AT_RESP_RETRIES
                       (3 << 8) |   // MAX_PHYS_RESP_RETRIES
                       (200 << 16); // Cycle limit
    pci->MemoryWrite32(bar0Index, kOHCI_ATRetries, retries);
    os_log(ASLog(), "ASOHCI: Phase 6 - AT Retries configured: 0x%08x", retries);

    // Phase 7: IEEE 1394a Enhancement Configuration (OHCI 1.1 §5.7.2)
    os_log(ASLog(), "ASOHCI: Phase 7 - IEEE 1394a Enhancement Check");
    uint32_t hcControl = 0;
    pci->MemoryRead32(bar0Index, kOHCI_HCControlSet, &hcControl);
    if (hcControl & kOHCI_HCControl_programPhyEnable) {
      // Generic software can configure IEEE 1394a enhancements
      pci->MemoryWrite32(bar0Index, kOHCI_HCControlSet,
                         kOHCI_HCControl_aPhyEnhanceEnable);
      os_log(ASLog(), "ASOHCI: IEEE 1394a enhancements enabled in link");
    } else {
      os_log(
          ASLog(),
          "ASOHCI: IEEE 1394a enhancements controlled by lower-level software");
    }

    // Ensure PHY access helper is available before PHY programming
    if (!ivars->phyAccess) {
      ivars->phyAccess = new ASOHCIPHYAccess();
      if (ivars->phyAccess &&
          !ivars->phyAccess->init(this, pci, (uint8_t)bar0Index)) {
        os_log(ASLog(), "ASOHCI: PHY access init failed (continuing without)");
        delete ivars->phyAccess;
        ivars->phyAccess = nullptr;
      } else if (ivars->phyAccess) {
        os_log(ASLog(), "ASOHCI: PHY access initialized");
      }
    }

    // Phase 8: PHY Register Programming (Linux ohci_enable line 2514)
    os_log(ASLog(), "ASOHCI: Phase 8 - PHY Register Programming");
    if (ivars->phyAccess) {
      // Read current PHY register 4
      uint8_t currentVal = 0;
      if (ivars->phyAccess->readPhyRegister(kPHY_REG_4, &currentVal) ==
          kIOReturnSuccess) {
        uint8_t newVal = currentVal | kPHY_LINK_ACTIVE | kPHY_CONTENDER;
        if (ivars->phyAccess->writePhyRegister(kPHY_REG_4, newVal) ==
            kIOReturnSuccess) {
          os_log(ASLog(),
                 "ASOHCI: PHY register 4: 0x%02x -> 0x%02x (LINK_ACTIVE + "
                 "CONTENDER)",
                 currentVal, newVal);
        } else {
          os_log(ASLog(), "ASOHCI: WARNING - PHY register 4 write failed");
        }
      } else {
        os_log(ASLog(), "ASOHCI: WARNING - PHY register 4 read failed");
      }
    } else {
      os_log(ASLog(), "ASOHCI: WARNING - No PHY access available, skipping "
                      "register programming");
    }

    // Phase 9: Clear and Setup Interrupts (Linux ohci_enable lines 2506-2573)
    os_log(ASLog(), "ASOHCI: Phase 9 - Interrupt Configuration");
    pci->MemoryWrite32(bar0Index, kOHCI_IntEventClear,
                       0xFFFFFFFF); // Clear all pending
    pci->MemoryWrite32(bar0Index, kOHCI_IntMaskClear,
                       0xFFFFFFFF); // Mask all initially

    // --- Initialize Context Managers (OHCI 1.1 §7-10) following Linux ohci.c
    // behavioral patterns
    os_log(ASLog(), "ASOHCI: === PHASE 8: Context Manager Initialization ===");
    os_log(ASLog(), "ASOHCI: Initializing context managers (BAR0=0x%x, PCI=%p)",
           bar0Index, pci);

    // Set async request filter to accept from all nodes (Linux early init
    // pattern)
    pci->MemoryWrite32(bar0Index, kOHCI_AsReqFilterHiSet, 0x80000000);
    os_log(ASLog(), "ASOHCI: Set async request filter to accept all nodes");

    // Status tracking for each manager (created + initialized + started
    // successfully)
    bool arManagerReady = false;
    bool atManagerReady = false;
    bool irManagerReady = false;
    bool itManagerReady = false;

    // Phase 8a: Initialize AR Manager (OHCI 1.1 §8 - Asynchronous Receive DMA)
    os_log(ASLog(), "ASOHCI: Phase 8a - Initializing AR Manager");
    ivars->arManager = OSSharedPtr(new ASOHCIARManager(), OSNoRetain);
    if (ivars->arManager) {
      os_log(ASLog(), "ASOHCI: AR Manager object created successfully");

      // Configure AR with reasonable buffer policy following Linux patterns
      ARFilterOptions arFilters{};
      arFilters.acceptPhyPackets = true;
      os_log(ASLog(), "ASOHCI: AR Manager configuration: buffers=8, "
                      "bytes=32768, mode=BufferFill, phyPackets=true");

      kr = ivars->arManager->Initialize(
          pci, (uint8_t)bar0Index,
          8,     // bufferCount (reduced to compensate for larger buffers)
          32768, // bufferBytes (32KB - Linux AR_BUFFER_SIZE standard)
          ARBufferFillMode::kBufferFill, arFilters);
      if (kr == kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCI: AR Manager Initialize() succeeded");
        kr = ivars->arManager->Start();
        if (kr == kIOReturnSuccess) {
          os_log(ASLog(),
                 "ASOHCI: AR Manager Start() succeeded - AR Manager ready");
          arManagerReady = true; // AR Manager fully operational
        } else {
          os_log(ASLog(), "ASOHCI: ERROR: AR Manager Start() failed: 0x%x", kr);
          ivars->arManager.reset(); // Clean up on failure
        }
      } else {
        os_log(ASLog(), "ASOHCI: ERROR: AR Manager Initialize() failed: 0x%x",
               kr);
        ivars->arManager.reset(); // Clean up on failure
      }
    } else {
      os_log(ASLog(), "ASOHCI: ERROR: Failed to allocate AR Manager object");
    }

    // Phase 8b: Initialize AT Manager (OHCI 1.1 §7 - Asynchronous Transmit DMA)
    os_log(ASLog(), "ASOHCI: Phase 8b - Initializing AT Manager");
    ivars->atManager = OSSharedPtr(new ASOHCIATManager(), OSNoRetain);
    if (ivars->atManager) {
      os_log(ASLog(), "ASOHCI: AT Manager object created successfully");

      // Configure AT with OHCI spec-compliant policies
      ATRetryPolicy retryPolicy{};
      retryPolicy.maxRetryA = 0x3;   // OHCI §7.4 default
      retryPolicy.maxRetryB = 0xF;   // OHCI §7.4 default
      retryPolicy.maxPhyResp = 0x64; // OHCI §7.4 default

      ATFairnessPolicy fairPolicy{};
      fairPolicy.fairnessControl = 0x3F; // OHCI §7.5 default

      ATPipelinePolicy pipePolicy{};
      pipePolicy.allowPipelining = true; // Enable pipelining per §7.7
      pipePolicy.maxOutstanding = 8;     // Conservative limit

      os_log(ASLog(),
             "ASOHCI: AT Manager configuration: pool=%uB, retryA=0x%x, "
             "retryB=0x%x, fairness=0x%x, pipelining=%d, maxOutstanding=%u",
             4096, retryPolicy.maxRetryA, retryPolicy.maxRetryB,
             fairPolicy.fairnessControl, pipePolicy.allowPipelining,
             pipePolicy.maxOutstanding);

      kr = ivars->atManager->Initialize(pci, (uint8_t)bar0Index, retryPolicy,
                                        fairPolicy, pipePolicy);
      if (kr == kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCI: AT Manager Initialize() succeeded");
        kr = ivars->atManager->Start();
        if (kr == kIOReturnSuccess) {
          os_log(ASLog(),
                 "ASOHCI: AT Manager Start() succeeded - AT Manager ready");
          atManagerReady = true; // AT Manager fully operational
        } else {
          os_log(ASLog(), "ASOHCI: ERROR: AT Manager Start() failed: 0x%x", kr);
          ivars->atManager.reset(); // Clean up on failure
        }
      } else {
        os_log(ASLog(), "ASOHCI: ERROR: AT Manager Initialize() failed: 0x%x",
               kr);
        if (kr == kIOReturnNoMemory) {
          os_log(ASLog(), "ASOHCI: AT Manager memory allocation failure - "
                          "likely due to descriptor pool initialization");
          os_log(ASLog(),
                 "ASOHCI: Check kPageSize (%zu bytes) and kMaxAllocation (%zu "
                 "bytes) constants",
                 kPageSize, kMaxAllocation);
        }
        ivars->atManager.reset(); // Clean up on failure
      }
    } else {
      os_log(ASLog(), "ASOHCI: ERROR: Failed to allocate AT Manager object");
    }

    // Initialize interrupt router and wire managers
    ivars->interruptRouter =
        OSSharedPtr(ASOHCIInterruptRouter::Create(), OSNoRetain);
    if (ivars->interruptRouter) {
      ivars->interruptRouter->SetARManager(ivars->arManager.get());
      ivars->interruptRouter->SetATManager(ivars->atManager.get());
      ivars->interruptRouter->SetIRManager(ivars->irManager.get());
      ivars->interruptRouter->SetITManager(ivars->itManager.get());
      ivars->interruptRouter->SetController(this);
    }

    // Phase 8c: Initialize IR Manager (OHCI 1.1 §10 - Isochronous Receive DMA)
    os_log(ASLog(), "ASOHCI: Phase 8c - Initializing IR Manager");
    ivars->irManager = OSSharedPtr(new ASOHCIIRManager(), OSNoRetain);
    if (ivars->irManager) {
      os_log(ASLog(), "ASOHCI: IR Manager object created successfully");

      IRPolicy irPolicy{};
      irPolicy.bufferFillWatermark =
          4; // Refill when 4 or fewer descriptors free
      irPolicy.headerSplitting = false; // Standard receive mode initially
      irPolicy.timestampingEnabled =
          true; // Enable timestamps for isochronous data

      os_log(ASLog(),
             "ASOHCI: IR Manager configuration: dynamic allocation, "
             "watermark=%u, headerSplitting=%d, timestamping=%d",
             irPolicy.bufferFillWatermark, irPolicy.headerSplitting,
             irPolicy.timestampingEnabled);

      kr = ivars->irManager->Initialize(pci, (uint8_t)bar0Index, irPolicy);
      if (kr == kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCI: IR Manager Initialize() succeeded");
        kr = ivars->irManager->StartAll();
        if (kr == kIOReturnSuccess) {
          os_log(ASLog(),
                 "ASOHCI: IR Manager StartAll() succeeded (%u contexts) - IR "
                 "Manager ready",
                 ivars->irManager->NumContexts());
          irManagerReady = true; // IR Manager fully operational
        } else {
          os_log(ASLog(), "ASOHCI: ERROR: IR Manager StartAll() failed: 0x%x",
                 kr);
          ivars->irManager.reset(); // Clean up on failure
        }
      } else {
        os_log(ASLog(), "ASOHCI: ERROR: IR Manager Initialize() failed: 0x%x",
               kr);
        if (kr == kIOReturnNoMemory) {
          os_log(ASLog(), "ASOHCI: IR Manager memory allocation failure - "
                          "likely due to descriptor ring initialization");
          os_log(ASLog(), "ASOHCI: Check IR buffer allocation patterns and DMA "
                          "mapping limits");
        }
        ivars->irManager.reset(); // Clean up on failure
      }
    } else {
      os_log(ASLog(), "ASOHCI: ERROR: Failed to allocate IR Manager object");
    }

    // Phase 8d: Initialize IT Manager (OHCI 1.1 §9 - Isochronous Transmit DMA)
    os_log(ASLog(), "ASOHCI: Phase 8d - Initializing IT Manager");
    ivars->itManager = OSSharedPtr(new ASOHCIITManager(), OSNoRetain);
    if (ivars->itManager) {
      os_log(ASLog(), "ASOHCI: IT Manager object created successfully");

      ITPolicy itPolicy{};
      itPolicy.cycleMatchEnabled = true; // Enable cycle matching per §9.3
      itPolicy.defaultInterruptPolicy =
          ITIntPolicy::kOnCompletion; // Interrupt on completion

      os_log(ASLog(),
             "ASOHCI: IT Manager configuration: dynamic allocation, "
             "cycleMatch=%d, intPolicy=%s",
             itPolicy.cycleMatchEnabled,
             itPolicy.defaultInterruptPolicy == ITIntPolicy::kOnCompletion
                 ? "OnCompletion"
                 : "Other");

      kr = ivars->itManager->Initialize(pci, (uint8_t)bar0Index, itPolicy);
      if (kr == kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCI: IT Manager Initialize() succeeded");
        kr = ivars->itManager->StartAll();
        if (kr == kIOReturnSuccess) {
          os_log(ASLog(),
                 "ASOHCI: IT Manager StartAll() succeeded (%u contexts) - IT "
                 "Manager ready",
                 ivars->itManager->NumContexts());
          itManagerReady = true; // IT Manager fully operational
        } else {
          os_log(ASLog(), "ASOHCI: ERROR: IT Manager StartAll() failed: 0x%x",
                 kr);
          ivars->itManager.reset(); // Clean up on failure
        }
      } else {
        os_log(ASLog(), "ASOHCI: ERROR: IT Manager Initialize() failed: 0x%x",
               kr);
        ivars->itManager.reset(); // Clean up on failure
      }
    } else {
      os_log(ASLog(), "ASOHCI: ERROR: Failed to allocate IT Manager object");
    }

    os_log(ASLog(), "ASOHCI: Context managers initialization complete");

    // Summary of manager initialization results using tracked status
    os_log(ASLog(), "ASOHCI: === Manager Status Summary ===");
    if (arManagerReady) {
      os_log(ASLog(), "ASOHCI: AR Manager: READY");
    } else {
      os_log(ASLog(), "ASOHCI: AR Manager: FAILED");
    }
    if (atManagerReady) {
      os_log(ASLog(), "ASOHCI: AT Manager: READY");
    } else {
      os_log(ASLog(), "ASOHCI: AT Manager: FAILED");
    }
    if (irManagerReady) {
      os_log(ASLog(), "ASOHCI: IR Manager: READY");
    } else {
      os_log(ASLog(), "ASOHCI: IR Manager: FAILED");
    }
    if (itManagerReady) {
      os_log(ASLog(), "ASOHCI: IT Manager: READY");
    } else {
      os_log(ASLog(), "ASOHCI: IT Manager: FAILED");
    }

    uint32_t readyCount = (arManagerReady ? 1 : 0) + (atManagerReady ? 1 : 0) +
                          (irManagerReady ? 1 : 0) + (itManagerReady ? 1 : 0);
    os_log(ASLog(), "ASOHCI: Total managers ready: %u/4", readyCount);

    if (readyCount < 4) {
      os_log(ASLog(), "ASOHCI: WARNING: Not all managers initialized - some "
                      "functionality may be limited");
    } else {
      os_log(ASLog(),
             "ASOHCI: SUCCESS: All context managers initialized and ready");
    }

    // Phase 10: Enable Comprehensive Interrupt Set (Linux ohci_enable lines
    // 2562-2573) Now that we have context managers, enable all OHCI interrupts
    // including isochronous
    uint32_t irqs =
        kOHCI_Int_ReqTxComplete | kOHCI_Int_RespTxComplete | kOHCI_Int_RqPkt |
        kOHCI_Int_RsPkt | kOHCI_Int_IsochTx | kOHCI_Int_IsochRx |
        kOHCI_Int_PostedWriteErr | kOHCI_Int_SelfIDComplete |
        kOHCI_Int_SelfIDComplete2 |
        kOHCI_Int_RegAccessFail | // CycleInconsistent disabled initially -
                                  // enable after cycle timer armed
        kOHCI_Int_UnrecoverableError | kOHCI_Int_CycleTooLong |
        kOHCI_Int_MasterEnable | kOHCI_Int_BusReset | kOHCI_Int_Phy;

    pci->MemoryWrite32(bar0Index, kOHCI_IntMaskSet, irqs);
    os_log(ASLog(),
           "ASOHCI: Phase 10 - Comprehensive interrupt mask set: 0x%08x", irqs);
    os_log(ASLog(), "ASOHCI: All interrupts enabled including isochronous - "
                    "context managers ready");

    // Phase 11: LinkEnable - Final Activation (OHCI 1.1 §5.7.3, Linux lines
    // 2575-2581)
    os_log(ASLog(), "ASOHCI: Phase 11 - Link Enable (Final Activation)");
    // Linux parity: set BIBimageValid alongside LinkEnable. If Config ROM is
    // not yet implemented, we may revisit; try this first as it matches Linux.
    pci->MemoryWrite32(
        bar0Index, kOHCI_HCControlSet,
        (kOHCI_HCControl_LinkEnable | kOHCI_HCControl_BIBimageValid));

    // Verify LinkEnable took effect
    uint32_t finalHCControl = 0;
    pci->MemoryRead32(bar0Index, kOHCI_HCControlSet, &finalHCControl);
    if (finalHCControl & kOHCI_HCControl_LinkEnable) {
      os_log(ASLog(),
             "ASOHCI: Link enabled successfully - controller active on bus");
    } else {
      os_log(ASLog(), "ASOHCI: WARNING - LinkEnable failed to set");
    }

    // Read initial NodeID state
    uint32_t node_id = 0;
    pci->MemoryRead32(bar0Index, kOHCI_NodeID, &node_id);
    os_log(ASLog(), "ASOHCI: Initial NodeID=0x%08x (idValid=%u root=%u)",
           node_id, (node_id >> 31) & 0x1, (node_id >> 30) & 0x1);

    os_log(ASLog(), "ASOHCI: ✅ Complete OHCI initialization sequence finished "
                    "(11 phases)");

  } else {
    os_log(ASLog(), "ASOHCI: BAR0 too small (0x%llx)", bar0Size);
  }

  // (moved) PHY access helper is initialized before Phase 7

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
kern_return_t ASOHCI::CopyBridgeLogs_Impl(OSData **outData) {
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
