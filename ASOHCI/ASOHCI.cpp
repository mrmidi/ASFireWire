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
#include "PhyAccess.hpp"

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

        // Program Self-ID buffer once; initial arm clears count
        ArmSelfIDReceive(pci, bar0Index, /*clearCount=*/true);

        // --- Async RX/TX scaffolding (no DMA yet): accept all, keep contexts halted (OHCI 1.1 §7)
        auto initAsyncScaffold = [&](uint32_t ctrlClear, uint32_t /*ctrlSet*/, uint32_t cmdPtr) {
            // Clear Run bit to halt context and set command pointer to 0
            pci->MemoryWrite32(bar0Index, ctrlClear, kOHCI_Context_Run);
            pci->MemoryWrite32(bar0Index, cmdPtr, 0);
        };
    // Async request filter baseline (Linux early init sets hi=0x80000000 to accept from all nodes)
    pci->MemoryWrite32(bar0Index, kOHCI_AsReqFilterHiSet, 0x80000000);
    // Leave other filters (lo + response filters) at reset defaults for now (can widen later under policy)
        // Ensure all four async contexts are halted (no programs yet)
        initAsyncScaffold(kOHCI_AsReqRcvContextControlC, kOHCI_AsReqRcvContextControlS, kOHCI_AsReqRcvCommandPtr);
        initAsyncScaffold(kOHCI_AsRspRcvContextControlC, kOHCI_AsRspRcvContextControlS, kOHCI_AsRspRcvCommandPtr);
        initAsyncScaffold(kOHCI_AsReqTrContextControlC,  kOHCI_AsReqTrContextControlS,  kOHCI_AsReqTrCommandPtr);
        initAsyncScaffold(kOHCI_AsRspTrContextControlC,  kOHCI_AsRspTrContextControlS,  kOHCI_AsRspTrCommandPtr);
        os_log(ASLog(), "ASOHCI: Async filters set (accept-all); AR/AT contexts halted");

    // --- Unmask minimal interrupts (defer cycle-related noise until stable bus)
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

    // Create PHY access helper after BAR mapping & PCI device known
    if (!gPhyAccess) {
        gPhyAccess = OSTypeAlloc(ASOHCIPHYAccess);
        if (gPhyAccess && !gPhyAccess->init(this, pci, (uint8_t)bar0Index)) {
            os_log(ASLog(), "ASOHCI: PHY access init failed (continuing without)" );
            gPhyAccess->release();
            gPhyAccess = nullptr;
        } else if (gPhyAccess) {
            os_log(ASLog(), "ASOHCI: PHY access initialized");
        }
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

    if (gPhyAccess) {
        gPhyAccess->release();
        gPhyAccess = nullptr;
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
            uint8_t phyIdReg = 0;
            if (gPhyAccess->readPhyRegister(0, &phyIdReg) == kIOReturnSuccess) {
                uint8_t localPhyId = phyIdReg & 0x3F;
                BRIDGE_LOG("PHY scan start localPhyId=%u raw0=0x%02x", localPhyId, phyIdReg);
                os_log(ASLog(), "ASOHCI: PHY scan start localPhyId=%u raw0=0x%02x", localPhyId, phyIdReg);
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
            } else {
                BRIDGE_LOG("PHY scan failed: register 0 read error");
                os_log(ASLog(), "ASOHCI: PHY scan failed: register 0 read error");
            }
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

    // Others (debug)
    uint32_t other = intEvent & ~(kOHCI_Int_BusReset | kOHCI_Int_SelfIDComplete | kOHCI_Int_MasterEnable);
    if (other) {
        os_log(ASLog(), "ASOHCI: Other IRQ bits: 0x%08x", other);
        BRIDGE_LOG("Other IRQ bits: 0x%08x", other);
    }
}
