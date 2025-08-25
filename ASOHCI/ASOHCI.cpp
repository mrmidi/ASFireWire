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

// OHCI constants
#include "OHCIConstants.hpp"

// ------------------------ Globals for this TU (simple bring‑up) ------------------------
static IOInterruptDispatchSource * gIntSource     = nullptr;
static IOPCIDevice               * gPCIDevice     = nullptr;
static IOBufferMemoryDescriptor  * gSelfIDBuffer  = nullptr;
static IODMACommand              * gSelfIDDMA     = nullptr;
static IOAddressSegment            gSelfIDSeg     = {};
static IOMemoryMap               * gSelfIDMap     = nullptr; // CPU mapping of buffer
static uint8_t                     gBAR0Index     = 0;
static volatile uint32_t           gInterruptCount= 0;

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
    uint32_t lc=0; pci->MemoryRead32(bar0, kOHCI_LinkControl, &lc);
    os_log(ASLog(), "ASOHCI: Arm Self-ID (clearCount=%u) LinkControl=0x%08x", clearCount ? 1u : 0u, lc);
}

// Decode IntEvent bits for logs
static void DumpIntEvent(uint32_t ev)
{
    if (!ev) return;
    #define P(bit,name) do { if (ev & (bit)) os_log(ASLog(), "ASOHCI:  • %{public}s", name); } while(0)
    P(kOHCI_Int_SelfIDComplete,    "SelfIDComplete");
    P(kOHCI_Int_BusReset,          "BusReset");
    P(kOHCI_Int_Phy,               "PHY event");
    P(kOHCI_Int_PhyRegRcvd,        "PHY reg received");
    P(kOHCI_Int_CycleSynch,        "CycleSynch");
    P(kOHCI_Int_Cycle64Seconds,    "Cycle64Seconds");
    P(kOHCI_Int_CycleLost,         "CycleLost");
    P(kOHCI_Int_CycleInconsistent, "CycleInconsistent");
    P(kOHCI_Int_UnrecoverableError,"UnrecoverableError");
    P(kOHCI_Int_CycleTooLong,      "CycleTooLong");
    P(kOHCI_Int_RqPkt,             "AR Req packet");
    P(kOHCI_Int_RsPkt,             "AR Rsp packet");
    P(kOHCI_Int_IsochTx,           "IsochTx");
    P(kOHCI_Int_IsochRx,           "IsochRx");
    P(kOHCI_Int_PostedWriteErr,    "PostedWriteErr");
    P(kOHCI_Int_LockRespErr,       "LockRespErr");
    #undef P
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

        // Enter LPS + enable posted writes (no IRQs yet)
        const uint32_t hcSet = (kOHCI_HCControl_LPS | kOHCI_HCControl_PostedWriteEn);
        pci->MemoryWrite32(bar0Index, kOHCI_HCControlSet, hcSet);
        os_log(ASLog(), "ASOHCI: HCControlSet LPS+PostedWrite (0x%08x)", hcSet);
        // Posted-write flush for ordering
        uint32_t _hc = 0; pci->MemoryRead32(bar0Index, kOHCI_HCControlSet, &_hc);

        // Link enable
        pci->MemoryWrite32(bar0Index, kOHCI_HCControlSet, kOHCI_HCControl_LinkEnable);
        os_log(ASLog(), "ASOHCI: HCControlSet LinkEnable");

        // Enable reception of Self-ID & PHY packets and cycle timer (Link Control)
        pci->MemoryWrite32(bar0Index, kOHCI_LinkControlSet,
                           (kOHCI_LC_RcvSelfID | kOHCI_LC_RcvPhyPkt | kOHCI_LC_CycleTimerEnable));
        os_log(ASLog(), "ASOHCI: LinkControlSet rcvSelfID+rcvPhyPkt+cycleTimer");

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

        // Program Self-ID buffer once; initial arm clears count
        ArmSelfIDReceive(pci, bar0Index, /*clearCount=*/true);

        // --- Async RX/TX scaffolding (no DMA yet): accept all, keep contexts halted (OHCI 1.1 §7)
        auto initAsyncScaffold = [&](uint32_t ctrlClear, uint32_t /*ctrlSet*/, uint32_t cmdPtr) {
            // Clear Run bit to halt context and set command pointer to 0
            pci->MemoryWrite32(bar0Index, ctrlClear, kOHCI_Context_Run);
            pci->MemoryWrite32(bar0Index, cmdPtr, 0);
        };
        // Accept all AR/AT transactions (filters are 64-bit: hi+lo)
        pci->MemoryWrite32(bar0Index, kOHCI_AsReqFilterHiSet, 0xFFFFFFFF);
        pci->MemoryWrite32(bar0Index, kOHCI_AsReqFilterLoSet, 0xFFFFFFFF);
        pci->MemoryWrite32(bar0Index, kOHCI_AsRspFilterHiSet, 0xFFFFFFFF);
        pci->MemoryWrite32(bar0Index, kOHCI_AsRspFilterLoSet, 0xFFFFFFFF);
        // Ensure all four async contexts are halted (no programs yet)
        initAsyncScaffold(kOHCI_AsReqRcvContextControlC, kOHCI_AsReqRcvContextControlS, kOHCI_AsReqRcvCommandPtr);
        initAsyncScaffold(kOHCI_AsRspRcvContextControlC, kOHCI_AsRspRcvContextControlS, kOHCI_AsRspRcvCommandPtr);
        initAsyncScaffold(kOHCI_AsReqTrContextControlC,  kOHCI_AsReqTrContextControlS,  kOHCI_AsReqTrCommandPtr);
        initAsyncScaffold(kOHCI_AsRspTrContextControlC,  kOHCI_AsRspTrContextControlS,  kOHCI_AsRspTrCommandPtr);
        os_log(ASLog(), "ASOHCI: Async filters set (accept-all); AR/AT contexts halted");

        // --- Unmask minimal interrupts + a few more for visibility
        uint32_t mask = (kOHCI_Int_SelfIDComplete | kOHCI_Int_BusReset | kOHCI_Int_MasterEnable
                         | kOHCI_Int_Phy | kOHCI_Int_RegAccessFail);
        pci->MemoryWrite32(bar0Index, kOHCI_IntMaskSet, mask);
        os_log(ASLog(), "ASOHCI: IntMaskSet 0x%08x", mask);

        // Clear any pending events (belt-and-braces)
        uint32_t ev=0; pci->MemoryRead32(bar0Index, kOHCI_IntEvent, &ev);
        if (ev) {
            pci->MemoryWrite32(bar0Index, kOHCI_IntEventClear, ev);
            os_log(ASLog(), "ASOHCI: Cleared initial IntEvent: 0x%08x", ev);
            DumpIntEvent(ev);
        }

        // Probe NodeID
        uint32_t node_id = 0;
        pci->MemoryRead32(bar0Index, kOHCI_NodeID, &node_id);
        os_log(ASLog(), "ASOHCI: NodeID=0x%08x (idValid=%u root=%u)", node_id,
               (node_id >> 31) & 0x1, (node_id >> 30) & 0x1);
    } else {
        os_log(ASLog(), "ASOHCI: BAR0 too small (0x%llx)", bar0Size);
    }

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

    // DMA buffers
    if (gSelfIDBuffer) {
        gSelfIDBuffer->release();
        gSelfIDBuffer = nullptr;
        os_log(ASLog(), "ASOHCI: Self-ID buffer released");
        BRIDGE_LOG("Self-ID buffer released");
    }
    if (gSelfIDMap) {
        gSelfIDMap->release();
        gSelfIDMap = nullptr;
    }
    if (gSelfIDDMA) {
        gSelfIDDMA->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        gSelfIDDMA->release();
        gSelfIDDMA = nullptr;
    }

    // Interrupt source
    if (gIntSource) {
        gIntSource->SetEnableWithCompletion(false, nullptr);
        gIntSource->release();
        gIntSource = nullptr;
        os_log(ASLog(), "ASOHCI: Interrupt source disabled");
    }
    // Mask all device interrupts as courtesy
    if (gPCIDevice) {
        gPCIDevice->MemoryWrite32(gBAR0Index, kOHCI_IntMaskClear, 0xFFFFFFFFu);
    }

    // Best-effort: disable BM/MEM space and close
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

    // Bus reset
    if (intEvent & kOHCI_Int_BusReset) {
        os_log(ASLog(), "ASOHCI: Bus reset");
        BRIDGE_LOG("Bus reset");
        uint32_t nodeID=0;
        gPCIDevice->MemoryRead32(gBAR0Index, kOHCI_NodeID, &nodeID);
        bool idValid = ((nodeID >> 31) & 1) != 0;
        bool isRoot  = ((nodeID >> 30) & 1) != 0;
        uint8_t nAdr = (uint8_t)((nodeID >> 16) & 0x3F);
        os_log(ASLog(), "ASOHCI: NodeID=0x%08x valid=%d root=%d addr=%u",
               nodeID, idValid, isRoot, nAdr);
        BRIDGE_LOG("NodeID=%08x valid=%d root=%d addr=%u",
                   nodeID, idValid, isRoot, nAdr);

        // Keep RcvSelfID enabled; do not clear count during Self-ID window
        ArmSelfIDReceive(gPCIDevice, gBAR0Index, /*clearCount=*/false);
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
        // Safe to clear count now to prepare for next cycle
        ArmSelfIDReceive(gPCIDevice, gBAR0Index, /*clearCount=*/true);
    }

    // Others (debug)
    uint32_t other = intEvent & ~(kOHCI_Int_BusReset | kOHCI_Int_SelfIDComplete | kOHCI_Int_MasterEnable);
    if (other) {
        os_log(ASLog(), "ASOHCI: Other IRQ bits: 0x%08x", other);
        BRIDGE_LOG("Other IRQ bits: 0x%08x", other);
    }
}
