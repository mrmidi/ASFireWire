//
//  ASOHCI.cpp
//  ASOHCI
//
//  Created by Aleksandr Shabelnikov on 23.08.2025.
//

#include <os/log.h>
#include <TargetConditionals.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

#include <DriverKit/IOUserServer.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSData.h>
#include <DriverKit/OSAction.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOMemoryMap.h>
#include <PCIDriverKit/IOPCIFamilyDefinitions.h>

// Generated Header
#include <net.mrmidi.ASFireWire.ASOHCI/ASOHCI.h>
#include "BridgeLog.hpp"

// OHCI constants and contexts
#include "OHCIConstants.hpp"
#include "PhyAccess.hpp"
#include "ASOHCIARContext.hpp"
#include "ASOHCIATContext.hpp"

// ------------------------ Globals for this TU (simple bring‑up) ------------------------
static IOInterruptDispatchSource * gIntSource     = nullptr;
static IOPCIDevice               * gPCIDevice     = nullptr;
static IOBufferMemoryDescriptor  * gSelfIDBuffer  = nullptr;
static IODMACommand              * gSelfIDDMA     = nullptr;
static IOAddressSegment            gSelfIDSeg     = {};
static IOMemoryMap               * gSelfIDMap     = nullptr; // CPU mapping of buffer
static uint8_t                     gBAR0Index     = 0;
static volatile uint32_t           gInterruptCount= 0;
static bool                        gCycleTimerArmed = false; // deferred enable
static bool                        gSelfIDInProgress = false; // between BusReset and SelfIDComplete
static bool                        gSelfIDArmed      = false; // Self-ID buffer armed for current cycle
static uint32_t                    gCollapsedBusResets = 0;   // BusReset signals ignored while in-progress
static uint32_t                    gLastLoggedNodeID = 0xFFFFFFFF; // last logged raw NodeID value
static bool                        gLastLoggedValid  = false;
static bool                        gLastLoggedRoot   = false;
static ASOHCIPHYAccess           * gPhyAccess        = nullptr; // serialized PHY register access
static bool                        gDidInitialPhyScan = false;  // one-shot PHY scan after first stable Self-ID

// AR/AT DMA Context Management
static ASOHCIARContext           * gARRequestContext  = nullptr; // AR Request context
static ASOHCIARContext           * gARResponseContext = nullptr; // AR Response context  
static ASOHCIATContext           * gATRequestContext  = nullptr; // AT Request context
static ASOHCIATContext           * gATResponseContext = nullptr; // AT Response context

// ------------------------ Logging -----------------------------------
#include "LogHelper.hpp"

// BRIDGE_LOG macro/functionality provided by BridgeLog.hpp

// Helper: program 32-bit Self-ID IOVA and enable packet reception
static inline void ArmSelfIDReceive(IOPCIDevice *pci, uint8_t bar0, bool clearCount)
{
// Keep buffer pointer programmed; optionally clear count; ensure receive bits are set
pci->MemoryWrite32(bar0, kOHCI_SelfIDBuffer, (uint32_t) gSelfIDSeg.address);
if (clearCount) {
    pci->MemoryWrite32(bar0, kOHCI_SelfIDCount, 0);
}
pci->MemoryWrite32(bar0, kOHCI_LinkControlSet, (kOHCI_LC_RcvSelfID | kOHCI_LC_RcvPhyPkt));
uint32_t lc=0; pci->MemoryRead32(bar0, kOHCI_LinkControlSet, &lc);
os_log(ASLog(), "ASOHCI: Arm Self-ID (clearCount=%u) LinkControl=0x%08x", clearCount ? 1u : 0u, lc);
gSelfIDArmed = true;
}

// OHCI 1.1 §6.1 compliant interrupt bit analysis with specification references
static void DumpIntEvent(uint32_t ev)
{
    if (!ev) return;
    
    // Group 1: DMA Completion Events (OHCI 1.1 §6.1 Table 6-1 bits 0-7)
    uint32_t dmaEvents = ev & 0x000000FF;
    if (dmaEvents) {
        os_log(ASLog(), "ASOHCI: === DMA Completion Interrupts (OHCI §6.1) ===");
        if (ev & kOHCI_Int_ReqTxComplete)   os_log(ASLog(), "ASOHCI:  • AT Request Tx Complete (bit 0) - §7.6");
        if (ev & kOHCI_Int_RespTxComplete)  os_log(ASLog(), "ASOHCI:  • AT Response Tx Complete (bit 1) - §7.6");
        if (ev & kOHCI_Int_ARRQ)            os_log(ASLog(), "ASOHCI:  • AR Request DMA Complete (bit 2) - §8.6");
        if (ev & kOHCI_Int_ARRS)            os_log(ASLog(), "ASOHCI:  • AR Response DMA Complete (bit 3) - §8.6");
        if (ev & kOHCI_Int_RqPkt)           os_log(ASLog(), "ASOHCI:  • AR Request Packet Received (bit 4) - §8.6");
        if (ev & kOHCI_Int_RsPkt)           os_log(ASLog(), "ASOHCI:  • AR Response Packet Received (bit 5) - §8.6");
        if (ev & kOHCI_Int_IsochTx)         os_log(ASLog(), "ASOHCI:  • Isochronous Tx Interrupt (bit 6) - §6.3");
        if (ev & kOHCI_Int_IsochRx)         os_log(ASLog(), "ASOHCI:  • Isochronous Rx Interrupt (bit 7) - §6.4");
    }
    
    // Group 2: Error Conditions (OHCI 1.1 §6.1 Table 6-1 bits 8-9, 18, 24-25)
    uint32_t errors = ev & (kOHCI_Int_PostedWriteErr | kOHCI_Int_LockRespErr |
                           kOHCI_Int_RegAccessFail | kOHCI_Int_UnrecoverableError |
                           kOHCI_Int_CycleTooLong);
    if (errors) {
        os_log(ASLog(), "ASOHCI: === ERROR CONDITIONS (OHCI §6.1) ===");
        if (ev & kOHCI_Int_PostedWriteErr)     os_log(ASLog(), "ASOHCI:  • Posted Write Error (bit 8) - host bus error §13.2.8.1");
        if (ev & kOHCI_Int_LockRespErr)        os_log(ASLog(), "ASOHCI:  • Lock Response Error (bit 9) - no ack_complete §5.5.1");
        if (ev & kOHCI_Int_RegAccessFail)      os_log(ASLog(), "ASOHCI:  • Register Access Failed (bit 18) - missing SCLK clock");
        if (ev & kOHCI_Int_UnrecoverableError) os_log(ASLog(), "ASOHCI:  • UNRECOVERABLE ERROR (bit 24) - context dead, operations stopped");
        if (ev & kOHCI_Int_CycleTooLong)       os_log(ASLog(), "ASOHCI:  • Cycle Too Long (bit 25) - >120μs cycle, cycleMaster cleared");
    }
    
    // Group 3: Bus Management & Timing (OHCI 1.1 §6.1 Table 6-1 bits 15-17, 19-23)
    uint32_t busEvents = ev & (kOHCI_Int_SelfIDComplete2 | kOHCI_Int_SelfIDComplete |
                              kOHCI_Int_BusReset | kOHCI_Int_Phy | kOHCI_Int_CycleSynch |
                              kOHCI_Int_Cycle64Seconds | kOHCI_Int_CycleLost |
                              kOHCI_Int_CycleInconsistent);
    if (busEvents) {
        os_log(ASLog(), "ASOHCI: === Bus Management & Timing (OHCI §6.1) ===");
        if (ev & kOHCI_Int_BusReset)          os_log(ASLog(), "ASOHCI:  • Bus Reset (bit 17) - PHY entered reset mode §6.1.1");
        if (ev & kOHCI_Int_SelfIDComplete)    os_log(ASLog(), "ASOHCI:  • Self-ID Complete (bit 16) - packet stream received §11.5");
        if (ev & kOHCI_Int_SelfIDComplete2)   os_log(ASLog(), "ASOHCI:  • Self-ID Complete Secondary (bit 15) - independent of busReset §11.5");
        if (ev & kOHCI_Int_Phy)               os_log(ASLog(), "ASOHCI:  • PHY Interrupt (bit 19) - status transfer request");
        if (ev & kOHCI_Int_CycleSynch)        os_log(ASLog(), "ASOHCI:  • Cycle Start (bit 20) - new isochronous cycle begun");
        if (ev & kOHCI_Int_Cycle64Seconds)    os_log(ASLog(), "ASOHCI:  • 64 Second Tick (bit 21) - cycle second counter bit 7 changed");
        if (ev & kOHCI_Int_CycleLost)         os_log(ASLog(), "ASOHCI:  • Cycle Lost (bit 22) - no cycle_start between cycleSynch events");
        if (ev & kOHCI_Int_CycleInconsistent) os_log(ASLog(), "ASOHCI:  • Cycle Inconsistent (bit 23) - timer mismatch §5.13, §9.5.1, §10.5.1");
    }
    
    // Group 4: High-Order Interrupts (OHCI 1.1 §6.1 Table 6-1 bits 26-31)
    uint32_t highBits = ev & 0xFC000000;
    if (highBits) {
        os_log(ASLog(), "ASOHCI: === High-Order Interrupts (OHCI §6.1) ===");
        if (ev & kOHCI_Int_PhyRegRcvd)        os_log(ASLog(), "ASOHCI:  • PHY Register Received (bit 26) - PHY register packet");
        if (ev & kOHCI_Int_AckTardy)          os_log(ASLog(), "ASOHCI:  • Acknowledgment Tardy (bit 27) - late ack received");
        if (ev & kOHCI_Int_SoftInterrupt)     os_log(ASLog(), "ASOHCI:  • Software Interrupt (bit 28) - host-initiated");
        if (ev & kOHCI_Int_VendorSpecific)    os_log(ASLog(), "ASOHCI:  • Vendor Specific (bit 29) - implementation-defined");
        if (ev & kOHCI_Int_MasterEnable)      os_log(ASLog(), "ASOHCI:  • Master Interrupt Enable (bit 31) - global enable bit");
    }
    
    // Check for unexpected bits in reserved ranges
    uint32_t reserved = ev & 0x00007C00;  // Bits 10-14 are reserved per OHCI spec
    if (reserved) {
        os_log(ASLog(), "ASOHCI: WARNING: Reserved interrupt bits set: 0x%08x (bits 10-14)", reserved);
    }
    
    uint32_t bit30 = ev & 0x40000000;  // Bit 30 is reserved per OHCI spec
    if (bit30) {
        os_log(ASLog(), "ASOHCI: WARNING: Reserved interrupt bit 30 set: 0x%08x", bit30);
    }
}

// Self-ID parsing
#include "SelfIDParser.hpp"

// Init
bool ASOHCI::init()
{
if (!super::init()) {
    return false;
}
// (Optional: zero / prepare any driver-local state if needed)
os_log(ASLog(), "ASOHCI: init()");
return true;
}

// =====================================================================================
// Start
// =====================================================================================
kern_return_t
IMPL(ASOHCI, Start)
{
kern_return_t kr = Start(provider, SUPERDISPATCH);
if (kr != kIOReturnSuccess) {
    os_log(ASLog(), "ASOHCI: Start superdispatch failed: 0x%08x", kr);
    return kr;
}
os_log(ASLog(), "ASOHCI: Start() begin bring-up");
BRIDGE_LOG("Start bring-up");
bridge_log_init();

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
BRIDGE_LOG("PCI IDs V=%04x D=%04x", vendorID, deviceID);

// Enable BusMaster|MemorySpace
uint16_t cmd = 0, newCmd = 0;
pci->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &cmd);
newCmd = (uint16_t)(cmd | kIOPCICommandBusMaster | kIOPCICommandMemorySpace);
if (newCmd != cmd) {
    pci->ConfigurationWrite16(kIOPCIConfigurationOffsetCommand, newCmd);
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &newCmd);
}
os_log(ASLog(), "ASOHCI: PCI CMD=0x%04x (was 0x%04x)", newCmd, cmd);
BRIDGE_LOG("PCI CMD=0x%04x->0x%04x", cmd, newCmd);

// BAR0 info
uint8_t  bar0Index = 0;
uint64_t bar0Size  = 0;
uint8_t  bar0Type  = 0;
kr = pci->GetBARInfo(0 /* BAR0 */, &bar0Index, &bar0Size, &bar0Type);
if (kr != kIOReturnSuccess) {
    os_log(ASLog(), "ASOHCI: GetBARInfo(BAR0) failed: 0x%08x", kr);
    // keep going; reads will fail gracefully
} else {
    os_log(ASLog(), "ASOHCI: BAR0 idx=%u size=0x%llx type=0x%02x", bar0Index, bar0Size, bar0Type);
    BRIDGE_LOG("BAR0 idx=%u size=0x%llx type=0x%02x", bar0Index, bar0Size, bar0Type);
}

if (bar0Size >= 0x2C) {
    uint32_t ohci_ver=0, bus_opts=0, guid_hi=0, guid_lo=0;
    pci->MemoryRead32(bar0Index, kOHCI_Version, &ohci_ver);
    pci->MemoryRead32(bar0Index, kOHCI_BusOptions, &bus_opts);
    pci->MemoryRead32(bar0Index, kOHCI_GUIDHi,   &guid_hi);
    pci->MemoryRead32(bar0Index, kOHCI_GUIDLo,   &guid_lo);
    os_log(ASLog(), "ASOHCI: OHCI VER=0x%08x BUSOPT=0x%08x GUID=%08x:%08x",
            ohci_ver, bus_opts, guid_hi, guid_lo);
    BRIDGE_LOG("OHCI VER=%08x BUSOPT=%08x GUID=%08x:%08x", ohci_ver, bus_opts, guid_hi, guid_lo);

    gPCIDevice = pci;
    gBAR0Index = bar0Index;

    // --- Clear/mask interrupts
    const uint32_t allOnes = 0xFFFFFFFFu;
    pci->MemoryWrite32(bar0Index, kOHCI_IntEventClear,        allOnes);
    pci->MemoryWrite32(bar0Index, kOHCI_IsoXmitIntEventClear,  allOnes);
    pci->MemoryWrite32(bar0Index, kOHCI_IsoRecvIntEventClear,  allOnes);
    pci->MemoryWrite32(bar0Index, kOHCI_IntMaskClear,          allOnes);
    pci->MemoryWrite32(bar0Index, kOHCI_IsoXmitIntMaskClear,   allOnes);
    pci->MemoryWrite32(bar0Index, kOHCI_IsoRecvIntMaskClear,   allOnes);
    os_log(ASLog(), "ASOHCI: Cleared interrupt events/masks");
    BRIDGE_LOG("IRQ clear/mask done");

    // --- Soft reset
    pci->MemoryWrite32(bar0Index, kOHCI_HCControlSet, kOHCI_HCControl_SoftReset);
    IOSleep(10);
    os_log(ASLog(), "ASOHCI: Soft reset issued");
    BRIDGE_LOG("Soft reset issued");

    // Re-clear after reset
    pci->MemoryWrite32(bar0Index, kOHCI_IntEventClear,        allOnes);
    pci->MemoryWrite32(bar0Index, kOHCI_IsoXmitIntEventClear,  allOnes);
    pci->MemoryWrite32(bar0Index, kOHCI_IsoRecvIntEventClear,  allOnes);
    pci->MemoryWrite32(bar0Index, kOHCI_IntMaskClear,          allOnes);
    pci->MemoryWrite32(bar0Index, kOHCI_IsoXmitIntMaskClear,   allOnes);
    pci->MemoryWrite32(bar0Index, kOHCI_IsoRecvIntMaskClear,   allOnes);

    // Enter LPS + enable posted writes (program BusOptions/NodeID prior to linkEnable)
    const uint32_t hcSet = (kOHCI_HCControl_LPS | kOHCI_HCControl_PostedWriteEn);
    pci->MemoryWrite32(bar0Index, kOHCI_HCControlSet, hcSet);
    os_log(ASLog(), "ASOHCI: HCControlSet LPS+PostedWrite (0x%08x)", hcSet);
    // Poll up to 3 * 50ms for LPS latch similar to Linux early init
    uint32_t _hc = 0; bool lpsOk = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        IOSleep(50);
        pci->MemoryRead32(bar0Index, kOHCI_HCControlSet, &_hc);
        if ((_hc & kOHCI_HCControl_LPS) != 0) { lpsOk = true; break; }
    }
    if (!lpsOk) {
        os_log(ASLog(), "ASOHCI: WARNING LPS did not latch after polling (_hc=0x%08x)", _hc);
    } else {
        os_log(ASLog(), "ASOHCI: LPS latched (_hc=0x%08x)", _hc);
    }

    // Program BusOptions similar to Linux early init: set CMC|ISC, clear PMC|BMC|cyc_clk_acc field
    uint32_t bo=0; pci->MemoryRead32(bar0Index, kOHCI_BusOptions, &bo);
    uint32_t origBo = bo;
    // Masks based on Linux patterns: CMC=bit29, ISC=bit30, PMC=bit27, BMC=bit28, cyc_clk_acc bits [23:16]
    bo |=  0x60000000;          // set ISC|CMC
    bo &= ~0x18000000;          // clear BMC|PMC
    bo &= ~0x00FF0000;          // clear cyc_clk_acc placeholder
    if (bo != origBo) {
        pci->MemoryWrite32(bar0Index, kOHCI_BusOptions, bo);
        os_log(ASLog(), "ASOHCI: BusOptions updated 0x%08x->0x%08x", origBo, bo);
    } else {
        os_log(ASLog(), "ASOHCI: BusOptions kept 0x%08x (already desired)", bo);
    }

    // Provisional NodeID (Linux early path uses 0x0000FFC0) prior to bus reset assignment
    pci->MemoryWrite32(bar0Index, kOHCI_NodeID, 0x0000FFC0);
    os_log(ASLog(), "ASOHCI: Provisional NodeID set to 0x0000FFC0");

// Persistent programPhyEnable (mimic Linux: set once, do not toggle per access)
pci->MemoryWrite32(bar0Index, kOHCI_HCControlSet, kOHCI_HCControl_programPhyEnable);
uint32_t hcAfterProg = 0; pci->MemoryRead32(bar0Index, kOHCI_HCControlSet, &hcAfterProg);
os_log(ASLog(), "ASOHCI: HCControlSet programPhyEnable (HCControl=0x%08x)", hcAfterProg);

    // Link enable after baseline BusOptions/NodeID prepared
    pci->MemoryWrite32(bar0Index, kOHCI_HCControlSet, kOHCI_HCControl_LinkEnable);
    os_log(ASLog(), "ASOHCI: HCControlSet LinkEnable");

    // Enable reception of Self-ID & PHY packets ONLY (defer cycle timer until stable Self-ID)
    pci->MemoryWrite32(bar0Index, kOHCI_LinkControlSet,
                        (kOHCI_LC_RcvSelfID | kOHCI_LC_RcvPhyPkt));
    os_log(ASLog(), "ASOHCI: LinkControlSet rcvSelfID+rcvPhyPkt (cycle timer deferred)");

    // --- MSI/MSI-X/Legacy routing
    kern_return_t rc = pci->ConfigureInterrupts(kIOInterruptTypePCIMessagedX, 1, 1, 0);
    if (rc == kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCI: Configured MSI-X interrupts");
        BRIDGE_LOG("Configured MSI-X");
    } else {
        rc = pci->ConfigureInterrupts(kIOInterruptTypePCIMessaged, 1, 1, 0);
        if (rc == kIOReturnSuccess) {
            os_log(ASLog(), "ASOHCI: Configured MSI interrupts");
            BRIDGE_LOG("Configured MSI");
        } else {
            os_log(ASLog(), "ASOHCI: Falling back to legacy interrupts");
            BRIDGE_LOG("Legacy IRQ");
        }
    }

    // --- Interrupt source on default queue
    IODispatchQueue *queue = nullptr;
    kr = CopyDispatchQueue(kIOServiceDefaultQueueName, &queue);
    if (kr == kIOReturnSuccess && queue) {
        IOInterruptDispatchSource *src = nullptr;
        kr = IOInterruptDispatchSource::Create(pci, 0 /* interrupt index */, queue, &src);
        if (kr == kIOReturnSuccess && src) {
            OSAction *action = nullptr;
            // generated by IIG; capacity 0 is fine for simple typed actions
            kern_return_t ar = CreateActionInterruptOccurred(0, &action);
            if (ar == kIOReturnSuccess && action) {
                src->SetHandler(action);
                action->release();
                src->SetEnableWithCompletion(true, nullptr);
                gIntSource = src;
                os_log(ASLog(), "ASOHCI: Interrupt source enabled");
                BRIDGE_LOG("IRQ source enabled");
            } else {
                os_log(ASLog(), "ASOHCI: CreateActionInterruptOccurred failed: 0x%08x", ar);
                if (src) src->release();
            }
        } else {
            os_log(ASLog(), "ASOHCI: IOInterruptDispatchSource::Create failed: 0x%08x", kr);
        }
        queue->release();
    } else {
        os_log(ASLog(), "ASOHCI: CopyDispatchQueue failed: 0x%08x", kr);
    }

    // --- Self‑ID DMA buffer setup (allocate, map to 32-bit IOVA, map to CPU)
    BRIDGE_LOG("Setting up Self-ID DMA buffer");
    kr = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionIn, // device writes into it
        kSelfIDBufferSize,
        kSelfIDBufferAlign,
        &gSelfIDBuffer
    );
    if (kr != kIOReturnSuccess || !gSelfIDBuffer) {
        os_log(ASLog(), "ASOHCI: IOBufferMemoryDescriptor::Create failed: 0x%08x", kr);
        return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;
    }

    // Map buffer into CPU address space for parsing
    if (!gSelfIDMap) {
        kern_return_t mr = gSelfIDBuffer->CreateMapping(0, 0, 0, 0, 0, &gSelfIDMap);
        if (mr != kIOReturnSuccess || !gSelfIDMap) {
            os_log(ASLog(), "ASOHCI: CreateMapping for Self-ID buffer failed: 0x%08x", mr);
        }
    }

    // Create 32-bit DMA mapping
    IODMACommandSpecification spec{};
    spec.options = kIODMACommandSpecificationNoOptions;
    spec.maxAddressBits = 32; // critical: 32-bit IOVA
    IODMACommand *dma = nullptr;
    kr = IODMACommand::Create(pci, kIODMACommandCreateNoOptions, &spec, &dma);
    if (kr != kIOReturnSuccess || !dma) {
        os_log(ASLog(), "ASOHCI: IODMACommand::Create failed: 0x%08x", kr);
        return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;
    }

    uint64_t flags = 0;
    uint32_t segCount = 32;
    IOAddressSegment segs[32] = {};
    kr = dma->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                            gSelfIDBuffer,
                            0,
                            kSelfIDBufferSize,
                            &flags,
                            &segCount,
                            segs);
    if (kr != kIOReturnSuccess || segCount < 1 || segs[0].address == 0) {
        os_log(ASLog(), "ASOHCI: PrepareForDMA failed: 0x%08x segs=%u addr=0x%llx", kr, segCount, (unsigned long long)segs[0].address);
        dma->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        dma->release();
        return (kr != kIOReturnSuccess) ? kr : kIOReturnNoResources;
    }
    gSelfIDDMA = dma;
    gSelfIDSeg = segs[0];
    os_log(ASLog(), "ASOHCI: Self-ID IOVA=0x%llx len=0x%llx", (unsigned long long)gSelfIDSeg.address, (unsigned long long)gSelfIDSeg.length);
    BRIDGE_LOG("Self-ID IOVA=0x%llx", (unsigned long long)gSelfIDSeg.address);

    // =====================================================================================
    // Complete OHCI Initialization Sequence (based on Linux driver and OHCI 1.1 spec)
    // =====================================================================================
    
    // Phase 1: Software Reset (OHCI 1.1 §5.7 HCControl.softReset, Linux: software_reset())
    os_log(ASLog(), "ASOHCI: Phase 1 - Software Reset");
    pci->MemoryWrite32(bar0Index, kOHCI_HCControlSet, kOHCI_HCControl_SoftReset);
    
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

    // Phase 2: Link Power Status Enable (OHCI 1.1 §5.7.3, Linux: LPS retry logic)  
    os_log(ASLog(), "ASOHCI: Phase 2 - Link Power Status Enable");
    pci->MemoryWrite32(bar0Index, kOHCI_HCControlSet, 
                       kOHCI_HCControl_LPS | kOHCI_HCControl_PostedWriteEn);
    
    // Critical: Wait for LPS to stabilize (Linux: up to 3 retries with 50ms delays)
    // Some controllers (ALI M5251) will lock up without proper LPS timing
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
        os_log(ASLog(), "ASOHCI: FATAL - LPS failed to enable, SCLK domain access will fail");
        return kIOReturnTimeout;
    }

    // Phase 3: Byte Swap Configuration (OHCI 1.1 §5.7.1)
    pci->MemoryWrite32(bar0Index, kOHCI_HCControlClear, kOHCI_HCControl_NoByteSwap);
    os_log(ASLog(), "ASOHCI: Phase 3 - Configured for little-endian byte order");

    // Phase 4: Self-ID Buffer Programming (OHCI 1.1 §11)
    os_log(ASLog(), "ASOHCI: Phase 4 - Self-ID Buffer Configuration");
    pci->MemoryWrite32(bar0Index, kOHCI_SelfIDBuffer, (uint32_t)gSelfIDSeg.address);
    // Gate the cycle timer until after a stable Self-ID is observed (handled in IRQ path)
    pci->MemoryWrite32(bar0Index, kOHCI_LinkControlSet,
                       (kOHCI_LC_RcvSelfID | kOHCI_LC_RcvPhyPkt));

    // Phase 5: AT Retries Configuration (Linux ohci_enable line 2479)
    uint32_t retries = (3 << 0) |        // MAX_AT_REQ_RETRIES 
                       (3 << 4) |        // MAX_AT_RESP_RETRIES
                       (3 << 8) |        // MAX_PHYS_RESP_RETRIES  
                       (200 << 16);      // Cycle limit
    pci->MemoryWrite32(bar0Index, kOHCI_ATRetries, retries);
    os_log(ASLog(), "ASOHCI: Phase 5 - AT Retries configured: 0x%08x", retries);

    // Phase 6: IEEE 1394a Enhancement Configuration (OHCI 1.1 §5.7.2)
    os_log(ASLog(), "ASOHCI: Phase 6 - IEEE 1394a Enhancement Check");
    uint32_t hcControl = 0;
    pci->MemoryRead32(bar0Index, kOHCI_HCControlSet, &hcControl);
    if (hcControl & kOHCI_HCControl_programPhyEnable) {
        // Generic software can configure IEEE 1394a enhancements
        pci->MemoryWrite32(bar0Index, kOHCI_HCControlSet, kOHCI_HCControl_aPhyEnhanceEnable);
        os_log(ASLog(), "ASOHCI: IEEE 1394a enhancements enabled in link");
    } else {
        os_log(ASLog(), "ASOHCI: IEEE 1394a enhancements controlled by lower-level software");
    }

    // Ensure PHY access helper is available before PHY programming
    if (!gPhyAccess) {
        gPhyAccess = new ASOHCIPHYAccess();
        if (gPhyAccess && !gPhyAccess->init(this, pci, (uint8_t)bar0Index)) {
            os_log(ASLog(), "ASOHCI: PHY access init failed (continuing without)" );
            delete gPhyAccess;
            gPhyAccess = nullptr;
        } else if (gPhyAccess) {
            os_log(ASLog(), "ASOHCI: PHY access initialized");
        }
    }

    // Phase 7: PHY Register Programming (Linux ohci_enable line 2514)
    os_log(ASLog(), "ASOHCI: Phase 7 - PHY Register Programming");
    if (gPhyAccess) {
        // Read current PHY register 4
        uint8_t currentVal = 0;
        if (gPhyAccess->readPhyRegister(kPHY_REG_4, &currentVal) == kIOReturnSuccess) {
            uint8_t newVal = currentVal | kPHY_LINK_ACTIVE | kPHY_CONTENDER;
            if (gPhyAccess->writePhyRegister(kPHY_REG_4, newVal) == kIOReturnSuccess) {
                os_log(ASLog(), "ASOHCI: PHY register 4: 0x%02x -> 0x%02x (LINK_ACTIVE + CONTENDER)", 
                       currentVal, newVal);
            } else {
                os_log(ASLog(), "ASOHCI: WARNING - PHY register 4 write failed");
            }
        } else {
            os_log(ASLog(), "ASOHCI: WARNING - PHY register 4 read failed");
        }
    } else {
        os_log(ASLog(), "ASOHCI: WARNING - No PHY access available, skipping register programming");
    }

    // Phase 8: Clear and Setup Interrupts (Linux ohci_enable lines 2506-2573)
    os_log(ASLog(), "ASOHCI: Phase 8 - Interrupt Configuration");
    pci->MemoryWrite32(bar0Index, kOHCI_IntEventClear, 0xFFFFFFFF);  // Clear all pending
    pci->MemoryWrite32(bar0Index, kOHCI_IntMaskClear, 0xFFFFFFFF);   // Mask all initially

    // --- Initialize AR/AT DMA Contexts (OHCI 1.1 §7-8)
    os_log(ASLog(), "ASOHCI: Initializing AR/AT DMA contexts");
    
    // Set async request filter to accept from all nodes (Linux early init pattern)
    pci->MemoryWrite32(bar0Index, kOHCI_AsReqFilterHiSet, 0x80000000);
    // Leave other filters (lo + response filters) at reset defaults for now
    
    // Initialize AR Request Context  
    gARRequestContext = new ASOHCIARContext();
    if (gARRequestContext) {
        kr = gARRequestContext->Initialize(pci, ASOHCIARContext::AR_REQUEST_CONTEXT, (uint8_t)bar0Index);
        if (kr == kIOReturnSuccess) {
            kr = gARRequestContext->Start();
            if (kr == kIOReturnSuccess) {
                os_log(ASLog(), "ASOHCI: AR Request context initialized and started");
            } else {
                os_log(ASLog(), "ASOHCI: ERROR: Failed to start AR Request context: 0x%x", kr);
            }
        } else {
            os_log(ASLog(), "ASOHCI: ERROR: Failed to initialize AR Request context: 0x%x", kr);
        }
    } else {
        os_log(ASLog(), "ASOHCI: ERROR: Failed to allocate AR Request context");
    }
    
    // Initialize AR Response Context
    gARResponseContext = new ASOHCIARContext();
    if (gARResponseContext) {
        kr = gARResponseContext->Initialize(pci, ASOHCIARContext::AR_RESPONSE_CONTEXT, (uint8_t)bar0Index);
        if (kr == kIOReturnSuccess) {
            kr = gARResponseContext->Start();
            if (kr == kIOReturnSuccess) {
                os_log(ASLog(), "ASOHCI: AR Response context initialized and started");
            } else {
                os_log(ASLog(), "ASOHCI: ERROR: Failed to start AR Response context: 0x%x", kr);
            }
        } else {
            os_log(ASLog(), "ASOHCI: ERROR: Failed to initialize AR Response context: 0x%x", kr);
        }
    } else {
        os_log(ASLog(), "ASOHCI: ERROR: Failed to allocate AR Response context");
    }
    
    // Initialize AT Request Context (foundation only)
    gATRequestContext = new ASOHCIATContext();
    if (gATRequestContext) {
        kr = gATRequestContext->Initialize(pci, ASOHCIATContext::AT_REQUEST_CONTEXT);
        if (kr == kIOReturnSuccess) {
            kr = gATRequestContext->Start();
            if (kr == kIOReturnSuccess) {
                os_log(ASLog(), "ASOHCI: AT Request context initialized and started");
            } else {
                os_log(ASLog(), "ASOHCI: ERROR: Failed to start AT Request context: 0x%x", kr);
            }
        } else {
            os_log(ASLog(), "ASOHCI: ERROR: Failed to initialize AT Request context: 0x%x", kr);
        }
    } else {
        os_log(ASLog(), "ASOHCI: ERROR: Failed to allocate AT Request context");
    }
    
    // Initialize AT Response Context (foundation only)
    gATResponseContext = new ASOHCIATContext();
    if (gATResponseContext) {
        kr = gATResponseContext->Initialize(pci, ASOHCIATContext::AT_RESPONSE_CONTEXT);
        if (kr == kIOReturnSuccess) {
            kr = gATResponseContext->Start();
            if (kr == kIOReturnSuccess) {
                os_log(ASLog(), "ASOHCI: AT Response context initialized and started");
            } else {
                os_log(ASLog(), "ASOHCI: ERROR: Failed to start AT Response context: 0x%x", kr);
            }
        } else {
            os_log(ASLog(), "ASOHCI: ERROR: Failed to initialize AT Response context: 0x%x", kr);
        }
    } else {
        os_log(ASLog(), "ASOHCI: ERROR: Failed to allocate AT Response context");
    }
    
    os_log(ASLog(), "ASOHCI: AR/AT context initialization complete");

    // Phase 9: Enable Comprehensive Interrupt Set (Linux ohci_enable lines 2562-2573)
    uint32_t irqs = kOHCI_Int_ReqTxComplete | kOHCI_Int_RespTxComplete |
                    kOHCI_Int_RqPkt | kOHCI_Int_RsPkt |
                    kOHCI_Int_IsochTx | kOHCI_Int_IsochRx |
                    kOHCI_Int_PostedWriteErr | kOHCI_Int_SelfIDComplete |
                    kOHCI_Int_RegAccessFail | kOHCI_Int_CycleInconsistent |
                    kOHCI_Int_UnrecoverableError | kOHCI_Int_CycleTooLong |
                    kOHCI_Int_MasterEnable | kOHCI_Int_BusReset | kOHCI_Int_Phy;
    
    pci->MemoryWrite32(bar0Index, kOHCI_IntMaskSet, irqs);
    os_log(ASLog(), "ASOHCI: Phase 9 - Comprehensive interrupt mask set: 0x%08x", irqs);

    // Phase 10: LinkEnable - Final Activation (OHCI 1.1 §5.7.3, Linux lines 2575-2581)
    os_log(ASLog(), "ASOHCI: Phase 10 - Link Enable (Final Activation)");
    // Defer BIBimageValid until Config ROM is programmed per OHCI §5.5
    pci->MemoryWrite32(bar0Index, kOHCI_HCControlSet, kOHCI_HCControl_LinkEnable);
    
    // Verify LinkEnable took effect
    uint32_t finalHCControl = 0;
    pci->MemoryRead32(bar0Index, kOHCI_HCControlSet, &finalHCControl);
    if (finalHCControl & kOHCI_HCControl_LinkEnable) {
        os_log(ASLog(), "ASOHCI: Link enabled successfully - controller active on bus");
    } else {
        os_log(ASLog(), "ASOHCI: WARNING - LinkEnable failed to set");
    }

    // Read initial NodeID state
    uint32_t node_id = 0;
    pci->MemoryRead32(bar0Index, kOHCI_NodeID, &node_id);
    os_log(ASLog(), "ASOHCI: Initial NodeID=0x%08x (idValid=%u root=%u)", node_id,
            (node_id >> 31) & 0x1, (node_id >> 30) & 0x1);

    os_log(ASLog(), "ASOHCI: ✅ Complete OHCI initialization sequence finished");
    BRIDGE_LOG("Complete OHCI initialization finished");
} else {
    os_log(ASLog(), "ASOHCI: BAR0 too small (0x%llx)", bar0Size);
}

// (moved) PHY access helper is initialized before Phase 7

os_log(ASLog(), "ASOHCI: Start() bring-up complete");
BRIDGE_LOG("Bring-up complete");
return kIOReturnSuccess;
}

// =====================================================================================
// Stop
// =====================================================================================
kern_return_t
IMPL(ASOHCI, Stop)
{
os_log(ASLog(), "ASOHCI: Stop() begin - Total interrupts received: %u", gInterruptCount);
BRIDGE_LOG("Stop - IRQ count: %u", gInterruptCount);

// 1) Disable our dispatch source first to stop callbacks
if (gIntSource) {
    gIntSource->SetEnableWithCompletion(false, nullptr);
    gIntSource->release();
    gIntSource = nullptr;
    os_log(ASLog(), "ASOHCI: Interrupt source disabled");
}

// 2) Quiesce the controller: mask and clear all interrupts, stop link RX paths
if (gPCIDevice) {
    // Mask all interrupt sources including MasterEnable
    gPCIDevice->MemoryWrite32(gBAR0Index, kOHCI_IntMaskClear, 0xFFFFFFFFu);
    // Clear any pending events to avoid later spurious IRQs on re-enable
    gPCIDevice->MemoryWrite32(gBAR0Index, kOHCI_IntEventClear,        0xFFFFFFFFu);
    gPCIDevice->MemoryWrite32(gBAR0Index, kOHCI_IsoXmitIntEventClear, 0xFFFFFFFFu);
    gPCIDevice->MemoryWrite32(gBAR0Index, kOHCI_IsoRecvIntEventClear, 0xFFFFFFFFu);
    // Also clear iso masks for completeness
    gPCIDevice->MemoryWrite32(gBAR0Index, kOHCI_IsoXmitIntMaskClear,  0xFFFFFFFFu);
    gPCIDevice->MemoryWrite32(gBAR0Index, kOHCI_IsoRecvIntMaskClear,  0xFFFFFFFFu);

    // Disable Self-ID/PHY receive and cycle timer
    gPCIDevice->MemoryWrite32(gBAR0Index, kOHCI_LinkControlClear,
                              (kOHCI_LC_RcvSelfID | kOHCI_LC_RcvPhyPkt | kOHCI_LC_CycleTimerEnable));
    // Readback to flush posted writes
    uint32_t _lc=0; gPCIDevice->MemoryRead32(gBAR0Index, kOHCI_LinkControlSet, &_lc);
}

// 3) Stop AR/AT contexts gracefully (clear run, wait inactive) before freeing backing memory
if (gARRequestContext) {
    gARRequestContext->Stop();
    delete gARRequestContext;
    gARRequestContext = nullptr;
    os_log(ASLog(), "ASOHCI: AR Request context stopped and released");
}
if (gARResponseContext) {
    gARResponseContext->Stop();
    delete gARResponseContext;
    gARResponseContext = nullptr;
    os_log(ASLog(), "ASOHCI: AR Response context stopped and released");
}
if (gATRequestContext) {
    gATRequestContext->Stop();
    delete gATRequestContext;
    gATRequestContext = nullptr;
    os_log(ASLog(), "ASOHCI: AT Request context stopped and released");
}
if (gATResponseContext) {
    gATResponseContext->Stop();
    delete gATResponseContext;
    gATResponseContext = nullptr;
    os_log(ASLog(), "ASOHCI: AT Response context stopped and released");
}

// 4) Disarm Self-ID receive and scrub registers before freeing buffers
if (gPCIDevice) {
    gPCIDevice->MemoryWrite32(gBAR0Index, kOHCI_SelfIDCount, 0);
    gPCIDevice->MemoryWrite32(gBAR0Index, kOHCI_SelfIDBuffer, 0);
}

// 5) Assert SoftReset and drop LinkEnable to stop the link state machine
if (gPCIDevice) {
    // Clear LinkEnable and aPhyEnhanceEnable bits
    gPCIDevice->MemoryWrite32(gBAR0Index, kOHCI_HCControlClear,
                              (kOHCI_HCControl_LinkEnable | kOHCI_HCControl_aPhyEnhanceEnable));
    // Soft reset the controller
    gPCIDevice->MemoryWrite32(gBAR0Index, kOHCI_HCControlSet, kOHCI_HCControl_SoftReset);
    IOSleep(10);
    // Readback to ensure reset posted
    uint32_t _hc=0; gPCIDevice->MemoryRead32(gBAR0Index, kOHCI_HCControlSet, &_hc);
    os_log(ASLog(), "ASOHCI: HC soft reset during Stop (HCControl=0x%08x)", _hc);
}

// 6) Now free DMA resources in safe order
if (gSelfIDDMA) {
    gSelfIDDMA->CompleteDMA(kIODMACommandCompleteDMANoOptions);
    gSelfIDDMA->release();
    gSelfIDDMA = nullptr;
}
if (gSelfIDMap) {
    gSelfIDMap->release();
    gSelfIDMap = nullptr;
}
if (gSelfIDBuffer) {
    gSelfIDBuffer->release();
    gSelfIDBuffer = nullptr;
    os_log(ASLog(), "ASOHCI: Self-ID buffer released");
    BRIDGE_LOG("Self-ID buffer released");
}

// 7) Release PHY helper
if (gPhyAccess) {
    delete gPhyAccess;
    gPhyAccess = nullptr;
}

// 8) Best-effort: disable BM/MEM space and close
if (auto pci = OSDynamicCast(IOPCIDevice, provider)) {
    uint16_t cmd=0; pci->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &cmd);
    uint16_t clr = (uint16_t)(cmd & ~(kIOPCICommandBusMaster | kIOPCICommandMemorySpace));
    if (clr != cmd) pci->ConfigurationWrite16(kIOPCIConfigurationOffsetCommand, clr);
    pci->Close(this, 0);
}

gPCIDevice = nullptr;
gBAR0Index = 0;
gInterruptCount = 0;

kern_return_t r = Stop(provider, SUPERDISPATCH);
os_log(ASLog(), "ASOHCI: Stop() complete: 0x%08x", r);
return r;
}

// =====================================================================================
// CopyBridgeLogs
// =====================================================================================
kern_return_t
IMPL(ASOHCI, CopyBridgeLogs)
{
return bridge_log_copy(outData);
}

// =====================================================================================
// Interrupt handler (typed action). IMPORTANT: complete the OSAction.
// =====================================================================================
void
ASOHCI::InterruptOccurred_Impl(ASOHCI_InterruptOccurred_Args)
{
uint32_t seq = __atomic_add_fetch(&gInterruptCount, 1, __ATOMIC_RELAXED);
os_log(ASLog(), "ASOHCI: InterruptOccurred #%u (count=%llu time=%llu)",
        seq, (unsigned long long)count, (unsigned long long)time);
BRIDGE_LOG("IRQ #%u hwcount=%llu", seq, (unsigned long long)count);

if (!gPCIDevice) {
    os_log(ASLog(), "ASOHCI: No PCI device bound; spurious?");
    return;
}

uint32_t intEvent = 0;
gPCIDevice->MemoryRead32(gBAR0Index, kOHCI_IntEvent, &intEvent);
if (intEvent == 0) {
    os_log(ASLog(), "ASOHCI: Spurious MSI (IntEvent=0)");
    return;
}

// Ack/clear what we saw (write-1-to-clear)
gPCIDevice->MemoryWrite32(gBAR0Index, kOHCI_IntEventClear, intEvent);
os_log(ASLog(), "ASOHCI: IntEvent=0x%08x", intEvent);
BRIDGE_LOG("IRQ events=0x%08x", intEvent);
DumpIntEvent(intEvent);

// Bus reset (coalesce repeated resets until SelfIDComplete)
if (intEvent & kOHCI_Int_BusReset) {
    if (gSelfIDInProgress) {
        // Already in a reset/self-id cycle; collapse this extra signal
        ++gCollapsedBusResets;
        BRIDGE_LOG("Collapsed BusReset (total collapsed=%u)", gCollapsedBusResets);
    } else {
        gSelfIDInProgress = true;
        gCollapsedBusResets = 0;
        BRIDGE_LOG("Bus reset (new cycle)");
        os_log(ASLog(), "ASOHCI: Bus reset (new cycle)");
        // Arm Self-ID fresh (clearCount true at start of cycle)
        ArmSelfIDReceive(gPCIDevice, gBAR0Index, /*clearCount=*/true);
    }
    // NodeID logging only when it changes or validity/root status toggles
    uint32_t nodeID=0; gPCIDevice->MemoryRead32(gBAR0Index, kOHCI_NodeID, &nodeID);
    bool idValid = ((nodeID >> 31) & 1) != 0;
    bool isRoot  = ((nodeID >> 30) & 1) != 0;
    if (nodeID != gLastLoggedNodeID || idValid != gLastLoggedValid || isRoot != gLastLoggedRoot) {
        uint8_t nAdr = (uint8_t)((nodeID >> 16) & 0x3F);
        os_log(ASLog(), "ASOHCI: NodeID=0x%08x valid=%d root=%d addr=%u (changed)", nodeID, idValid, isRoot, nAdr);
        BRIDGE_LOG("NodeID change %08x v=%d r=%d addr=%u", nodeID, idValid, isRoot, nAdr);
        gLastLoggedNodeID = nodeID;
        gLastLoggedValid  = idValid;
        gLastLoggedRoot   = isRoot;
    }
}

// NOTE: Self-ID complete will deliver alpha self-ID quadlets (#0 and optional #1/#2).
// Parser implements IEEE 1394-2008 §16.3.2.1 (Alpha). Beta support can be added later.

// Self-ID complete
if (intEvent & kOHCI_Int_SelfIDComplete) {
    os_log(ASLog(), "ASOHCI: Self-ID phase complete");
    BRIDGE_LOG("Self-ID complete");

    uint32_t selfIDCount=0;
    gPCIDevice->MemoryRead32(gBAR0Index, kOHCI_SelfIDCount, &selfIDCount);

    uint32_t quads = (selfIDCount >> 2) & 0x1FF;
    bool     err   = (selfIDCount >> 31) & 0x1;
    os_log(ASLog(), "ASOHCI: SelfID count=%u quads, error=%d", quads, err);
    BRIDGE_LOG("SelfID count=%u error=%d", quads, err);

    if (!err && quads > 0 && gSelfIDMap) {
        uint32_t *ptr = (uint32_t *)(uintptr_t) gSelfIDMap->GetAddress();
        size_t     len = (size_t) gSelfIDMap->GetLength();
        if (ptr && len >= quads * sizeof(uint32_t)) {
            SelfIDParser::Process(ptr, quads);
        } else {
            os_log(ASLog(), "ASOHCI: Self-ID CPU mapping invalid for parse");
        }
    }
    // First stable Self-ID -> enable cycle timer if not yet
    if (!gCycleTimerArmed && gPCIDevice) {
        gPCIDevice->MemoryWrite32(gBAR0Index, kOHCI_LinkControlSet, kOHCI_LC_CycleTimerEnable);
        uint32_t lcPost=0; gPCIDevice->MemoryRead32(gBAR0Index, kOHCI_LinkControlSet, &lcPost);
        os_log(ASLog(), "ASOHCI: CycleTimerEnable asserted post Self-ID (LinkControl=0x%08x)", lcPost);
        BRIDGE_LOG("CycleTimerEnable now set (LC=%08x)", lcPost);
        gCycleTimerArmed = true;
    }
    // Perform one-time PHY scan (read-only) after first stable Self-ID
    if (!gDidInitialPhyScan && gPhyAccess) {
        // Inline small scan to avoid extra TU for now
        const uint8_t kMaxPhyPorts = 16; // hard cap
        
        // OHCI 5.12: "Software shall not issue a read of PHY register 0. 
        // The most recently available contents of this register shall be reflected in the NodeID register"
        uint32_t nodeIdReg = 0;
        gPCIDevice->MemoryRead32(gBAR0Index, kOHCI_NodeID, &nodeIdReg);
        uint8_t localPhyId = (uint8_t)((nodeIdReg >> 24) & 0x3F); // NodeID register bits 29:24 contain PHY ID
        
        BRIDGE_LOG("PHY scan start localPhyId=%u (from NodeID=0x%08x)", localPhyId, nodeIdReg);
        os_log(ASLog(), "ASOHCI: PHY scan start localPhyId=%u (from NodeID=0x%08x)", localPhyId, nodeIdReg);
        
        uint32_t connectedCount = 0, enabledCount = 0, contenderCount = 0;
        uint8_t portBaseReg = 4; // typical start of port status regs
        for (uint8_t p = 0; p < kMaxPhyPorts; ++p) {
                uint8_t raw = 0; uint8_t reg = (uint8_t)(portBaseReg + p);
                if (gPhyAccess->readPhyRegister(reg, &raw) != kIOReturnSuccess) {
                    BRIDGE_LOG("PHY port reg %u read timeout - stopping scan", reg);
                    os_log(ASLog(), "ASOHCI: PHY port reg %u read timeout - stopping scan", reg);
                    break; // abort further scanning
                }
                // Heuristic: if raw reads as all ones (0xFF) or all zeros beyond first gap, assume no more ports
                if (raw == 0xFF || raw == 0x00) {
                    if (p == 0) {
                        // ambiguous, still treat as potentially valid first port; continue
                    } else {
                        BRIDGE_LOG("PHY port %u raw=0x%02x sentinel -> end", p, raw);
                        break;
                    }
                }
                bool connected = (raw & 0x01) != 0;      // Connected
                bool child     = (raw & 0x02) != 0;      // Child
                bool parent    = (raw & 0x04) != 0;      // Parent
                bool contender = (raw & 0x08) != 0;      // Contender
                bool power     = (raw & 0x10) != 0;      // Power (arbitration participation / bias)
                bool disabled  = (raw & 0x40) != 0;      // Disabled (spec dependent bit position)
                bool enabled   = !disabled;              // Derived
                if (connected) ++connectedCount;
                if (enabled)   ++enabledCount;
                if (contender) ++contenderCount;
                BRIDGE_LOG("PHY port %u raw=0x%02x conn=%u en=%u child=%u parent=%u cont=%u pwr=%u", p, raw, connected, enabled, child, parent, contender, power);
                os_log(ASLog(), "ASOHCI: PHY port %u raw=0x%02x conn=%u en=%u child=%u parent=%u cont=%u pwr=%u", p, raw, connected, enabled, child, parent, contender, power);
        }
        BRIDGE_LOG("PHY scan summary connected=%u enabled=%u contender=%u", connectedCount, enabledCount, contenderCount);
        os_log(ASLog(), "ASOHCI: PHY scan summary connected=%u enabled=%u contender=%u", connectedCount, enabledCount, contenderCount);
        gDidInitialPhyScan = true;
    }
    // Mark cycle complete; prepare for future resets but do not override in-progress flags until next BusReset
    gSelfIDInProgress = false;
    gSelfIDArmed = false;
    if (gCollapsedBusResets) {
        os_log(ASLog(), "ASOHCI: Collapsed %u BusReset IRQs in cycle", gCollapsedBusResets);
        BRIDGE_LOG("Collapsed %u BusResets", gCollapsedBusResets);
    }
    // Re-arm to listen for next cycle (do not clear count again until next BusReset)
    ArmSelfIDReceive(gPCIDevice, gBAR0Index, /*clearCount=*/false);
}

// AR/AT Context Interrupt Handling (OHCI 1.1 §6.1 bits 0-3)
if (intEvent & (kOHCI_Int_ARRQ | kOHCI_Int_ARRS | kOHCI_Int_ReqTxComplete | kOHCI_Int_RespTxComplete)) {
    
    // AR Request context interrupt (bit 2)
    if ((intEvent & kOHCI_Int_ARRQ) && gARRequestContext) {
        gARRequestContext->HandleInterrupt();
    }
    
    // AR Response context interrupt (bit 3) 
    if ((intEvent & kOHCI_Int_ARRS) && gARResponseContext) {
        gARResponseContext->HandleInterrupt();
    }
    
    // AT Request context interrupt (bit 0)
    if ((intEvent & kOHCI_Int_ReqTxComplete) && gATRequestContext) {
        gATRequestContext->HandleInterrupt();
    }
    
    // AT Response context interrupt (bit 1)
    if ((intEvent & kOHCI_Int_RespTxComplete) && gATResponseContext) {
        gATResponseContext->HandleInterrupt();
    }
}

// All interrupt bits are now handled by the comprehensive DumpIntEvent function
// No need for generic "Other IRQ bits" logging as every bit is properly identified per OHCI §6.1
}
