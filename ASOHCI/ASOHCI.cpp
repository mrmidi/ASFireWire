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
// Config ROM
#include "ASOHCIConfigROM.hpp"
// ------------------------ Logging -----------------------------------
#include "LogHelper.hpp"
#include "ASOHCIInterruptDump.hpp"

// BRIDGE_LOG macro/functionality provided by BridgeLog.hpp

// Helper: dump a memory region as hex lines to os_log without sprintf/snprintf
static inline char _hex_digit(uint8_t v) { return (v < 10) ? char('0' + v) : char('a' + (v - 10)); }
static inline void DumpHexBigEndian(const void* data, size_t length, const char* title)
{
if (!data || length == 0) return;
const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
// Determine effective length by trimming trailing zeros; keep at least 64 bytes, round up to 16
size_t eff = length;
while (eff > 0 && p[eff - 1] == 0) {
    --eff;
}
const size_t kMinDump = 64; // keep at least 64 bytes (header + small root dir)
if (eff < kMinDump) eff = (length < kMinDump) ? length : kMinDump;
// round up to 16-byte boundary for clean lines
if (eff % 16) {
    size_t rounded = ((eff + 15) / 16) * 16;
    if (rounded <= length) eff = rounded; else eff = length;
}

os_log(ASLog(), "ASOHCI: === %{public}s (BIG-ENDIAN) === size=%lu dump=%lu", title ? title : "DUMP", (unsigned long)length, (unsigned long)eff);
char line[80];
for (size_t off = 0; off < eff; off += 16) {
    size_t pos = 0;
    // 4 hex digits of offset
    uint16_t o = (uint16_t)off;
    line[pos++] = _hex_digit(uint8_t((o >> 12) & 0xF));
    line[pos++] = _hex_digit(uint8_t((o >> 8) & 0xF));
    line[pos++] = _hex_digit(uint8_t((o >> 4) & 0xF));
    line[pos++] = _hex_digit(uint8_t(o & 0xF));
    line[pos++] = ':';
    // up to 16 bytes
    for (size_t i = 0; i < 16 && (off + i) < length; ++i) {
        uint8_t b = p[off + i];
        line[pos++] = ' ';
        line[pos++] = _hex_digit(uint8_t((b >> 4) & 0xF));
        line[pos++] = _hex_digit(uint8_t(b & 0xF));
    }
    line[pos] = '\0';
    os_log(ASLog(), "ASOHCI: %{public}s", line);
}
os_log(ASLog(), "ASOHCI: === END OF DUMP ===");
}

// Provide the concrete definition for ASOHCI_IVars matching ASOHCI.iig ivars.
// This enables safe member access in this TU while keeping layout identical.
struct ASOHCI_IVars {
IOPCIDevice               * pciDevice     = nullptr;
IOMemoryMap               * bar0Map       = nullptr;
uint8_t                     barIndex      = 0;
IOInterruptDispatchSource * intSource     = nullptr;
IODispatchQueue           * defaultQ      = nullptr;
uint64_t                    interruptCount = 0;
IOBufferMemoryDescriptor  * selfIDBuffer  = nullptr;
IODMACommand              * selfIDDMA     = nullptr;
IOAddressSegment            selfIDSeg     = {};
IOMemoryMap               * selfIDMap     = nullptr;
// Config ROM resources
IOBufferMemoryDescriptor  * configROMBuffer = nullptr;
IOMemoryMap               * configROMMap    = nullptr;
IODMACommand              * configROMDMA    = nullptr;
IOAddressSegment            configROMSeg    = {};
uint32_t                    configROMHeaderQuad = 0;
uint32_t                    configROMBusOptions = 0;
bool                        configROMHeaderNeedsCommit = false;
bool                        cycleTimerArmed   = false;
bool                        selfIDInProgress  = false;
bool                        selfIDArmed       = false;
uint32_t                    collapsedBusResets= 0;
uint32_t                    lastLoggedNodeID  = 0xFFFFFFFFu;
bool                        lastLoggedValid   = false;
bool                        lastLoggedRoot    = false;
bool                        didInitialPhyScan = false;
ASOHCIPHYAccess           * phyAccess     = nullptr;
ASOHCIARContext           * arRequestContext  = nullptr;
ASOHCIARContext           * arResponseContext = nullptr;
ASOHCIATContext           * atRequestContext  = nullptr;
ASOHCIATContext           * atResponseContext = nullptr;
};

// Helper: program 32-bit Self-ID IOVA and enable packet reception
void ASOHCI::ArmSelfIDReceive(bool clearCount)
{
if (!ivars || !ivars->pciDevice) return;
// Keep buffer pointer programmed; optionally clear count; ensure receive bits are set
ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_SelfIDBuffer, (uint32_t) ivars->selfIDSeg.address);
if (clearCount) {
    ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_SelfIDCount, 0);
}
ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_LinkControlSet, (kOHCI_LC_RcvSelfID | kOHCI_LC_RcvPhyPkt));
uint32_t lc=0; ivars->pciDevice->MemoryRead32(ivars->barIndex, kOHCI_LinkControlSet, &lc);
os_log(ASLog(), "ASOHCI: Arm Self-ID (clearCount=%u) LinkControl=0x%08x", clearCount ? 1u : 0u, lc);
ivars->selfIDArmed = true;
}

// Self-ID parsing
#include "SelfIDParser.hpp"

// Init
bool ASOHCI::init()
{
if (!super::init()) return false;
if (!ivars) {
    ivars = new ASOHCI_IVars{}; // value-init → zero-initialize
    if (!ivars) return false;
}
os_log(ASLog(), "ASOHCI: init()");
return true;
}

void ASOHCI::free()
{
os_log(ASLog(), "ASOHCI: free()");
if (ivars) {
    os_log(ASLog(), "ASOHCI: free step A - stop contexts if present");
    // Safety: in case Stop() was not invoked, ensure contexts are torn down
    if (ivars->arRequestContext)  { ivars->arRequestContext->Stop();  delete ivars->arRequestContext;  ivars->arRequestContext  = nullptr; }
    if (ivars->arResponseContext) { ivars->arResponseContext->Stop(); delete ivars->arResponseContext; ivars->arResponseContext = nullptr; }
    if (ivars->atRequestContext)  { ivars->atRequestContext->Stop();  delete ivars->atRequestContext;  ivars->atRequestContext  = nullptr; }
    if (ivars->atResponseContext) { ivars->atResponseContext->Stop(); delete ivars->atResponseContext; ivars->atResponseContext = nullptr; }
    // Disable interrupt source first to stop callbacks
    os_log(ASLog(), "ASOHCI: free step B - disable/release interrupt source");
    if (ivars->intSource) {
        ivars->intSource->SetEnableWithCompletion(false, nullptr);
        ivars->intSource->release();
        ivars->intSource = nullptr;
    }
    // Release DMA resources (safe even if Start failed mid-way)
    os_log(ASLog(), "ASOHCI: free step C - release Self-ID DMA/map/buffer");
    if (ivars->selfIDDMA) {
        ivars->selfIDDMA->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        ivars->selfIDDMA->release();
        ivars->selfIDDMA = nullptr;
    }
    if (ivars->selfIDMap)    { ivars->selfIDMap->release();    ivars->selfIDMap    = nullptr; }
    if (ivars->selfIDBuffer) { ivars->selfIDBuffer->release(); ivars->selfIDBuffer = nullptr; }
    // Release Config ROM resources
    if (ivars->configROMDMA) {
        ivars->configROMDMA->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        ivars->configROMDMA->release();
        ivars->configROMDMA = nullptr;
    }
    if (ivars->configROMMap)    { ivars->configROMMap->release();    ivars->configROMMap    = nullptr; }
    if (ivars->configROMBuffer) { ivars->configROMBuffer->release(); ivars->configROMBuffer = nullptr; }
    // Release default queue if we retained it (see §4)
    if (ivars->defaultQ)     { ivars->defaultQ->release();     ivars->defaultQ     = nullptr; }
    // Delete helpers
    os_log(ASLog(), "ASOHCI: free step D - delete helpers and ivars");
    if (ivars->phyAccess)    { delete ivars->phyAccess;        ivars->phyAccess    = nullptr; }
    // Delete ivars (run C++ dtors if introduced later)
    delete ivars;
    ivars = nullptr;
}
super::free();
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
if (!ivars) {
    os_log(ASLog(), "ASOHCI: ivars not allocated");
    return kIOReturnNoResources;
}
os_log(ASLog(), "ASOHCI: Start() begin bring-up");
BRIDGE_LOG("Start bring-up");
bridge_log_init();

// Reset bring-up/lifecycle flags to known defaults
ivars->cycleTimerArmed    = false;
ivars->selfIDInProgress   = false;
ivars->selfIDArmed        = false;
ivars->collapsedBusResets = 0;
ivars->didInitialPhyScan  = false;

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
// Optional: discover and log common PCI capabilities (PM, MSI, MSI-X)
{
    uint64_t off = 0;
    if (pci->FindPCICapability(kIOPCICapabilityIDPowerManagement, 0 /* start */, &off) == kIOReturnSuccess && off) {
        os_log(ASLog(), "ASOHCI: PCI PM capability at 0x%llx", off);
    }
    off = 0;
    if (pci->FindPCICapability(kIOPCICapabilityIDMSI, 0 /* start */, &off) == kIOReturnSuccess && off) {
        os_log(ASLog(), "ASOHCI: PCI MSI capability at 0x%llx", off);
    }
    off = 0;
    if (pci->FindPCICapability(kIOPCICapabilityIDMSIX, 0 /* start */, &off) == kIOReturnSuccess && off) {
        os_log(ASLog(), "ASOHCI: PCI MSI-X capability at 0x%llx", off);
    }
}

// Log current link speed for diagnostics
// Initialize with a valid enum value to satisfy strict type checking
IOPCILinkSpeed linkSpeed = kPCILinkSpeed_2_5_GTs;
if (pci->GetLinkSpeed(&linkSpeed) == kIOReturnSuccess) {
    os_log(ASLog(), "ASOHCI: PCIe link speed: %u", (unsigned)linkSpeed);
}

uint32_t ohci_ver=0, bus_opts=0, guid_hi=0, guid_lo=0;
pci->MemoryRead32(bar0Index, kOHCI_Version, &ohci_ver);
pci->MemoryRead32(bar0Index, kOHCI_BusOptions, &bus_opts);
pci->MemoryRead32(bar0Index, kOHCI_GUIDHi,   &guid_hi);
pci->MemoryRead32(bar0Index, kOHCI_GUIDLo,   &guid_lo);
os_log(ASLog(), "ASOHCI: OHCI VER=0x%08x BUSOPT=0x%08x GUID=%08x:%08x",
        ohci_ver, bus_opts, guid_hi, guid_lo);
BRIDGE_LOG("OHCI VER=%08x BUSOPT=%08x GUID=%08x:%08x", ohci_ver, bus_opts, guid_hi, guid_lo);

ivars->pciDevice = pci;
ivars->barIndex  = bar0Index;

// -----------------------------------------------------------------
// Minimal Configuration ROM: allocate 1KB, populate BIB, map, program
// -----------------------------------------------------------------
{
    // Allocate 1KB ROM buffer and map for CPU access
    // Device will READ this buffer (host -> device), so direction = Out
    kern_return_t ckr = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOut,
                                                            1024,
                                                            4,
                                                            &ivars->configROMBuffer);
    if (ckr == kIOReturnSuccess && ivars->configROMBuffer) {
        ckr = ivars->configROMBuffer->CreateMapping(0, 0, 0, 0, 0, &ivars->configROMMap);
    }
    if (ckr != kIOReturnSuccess || !ivars->configROMMap) {
        os_log(ASLog(), "ASOHCI: WARN: Config ROM map failed: 0x%08x", ckr);
    } else {
        // Build ROM (BIB + minimal root directory) and write big-endian into buffer
        ASOHCIConfigROM rom;
        rom.buildFromHardware(bus_opts, guid_hi, guid_lo, /*includeRootDirectory*/true, /*includeNodeCaps*/true);
        void* romPtr = (void*)ivars->configROMMap->GetAddress();
        size_t romLen = (size_t)ivars->configROMMap->GetLength();
        rom.writeToBufferBE(romPtr, romLen);

        // Full ROM dump for debugging and verification
        DumpHexBigEndian(romPtr, romLen, "CONFIG ROM DUMP HEX");

        // DMA-map to obtain 32-bit IOVA for ConfigROMmap register
        IODMACommandSpecification spec{};
        spec.options = kIODMACommandSpecificationNoOptions;
        spec.maxAddressBits = 32; // 32-bit IOVA required for OHCI
        IODMACommand* dma = nullptr;
        ckr = IODMACommand::Create(pci, kIODMACommandCreateNoOptions, &spec, &dma);
        if (ckr == kIOReturnSuccess && dma) {
            uint64_t dflags = 0;
            uint32_t dsegs = 32;
            IOAddressSegment segs[32] = {};
            ckr = dma->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                                        ivars->configROMBuffer,
                                        0,
                                        1024,
                                        &dflags,
                                        &dsegs,
                                        segs);
            if (ckr == kIOReturnSuccess) {
                ivars->configROMDMA = dma;
                ivars->configROMSeg = segs[0];
                // Program OHCI registers: header=0 (workaround), BusOptions from ROM[2], map address
                // Stage BusOptions and header for commit on next BusReset per OHCI §5.5 (Linux parity)
                ivars->configROMHeaderQuad = rom.headerQuad();
                ivars->configROMBusOptions = rom.romQuad(2);
                ivars->configROMHeaderNeedsCommit = true;
                // Initial workaround: header=0 until next BusReset; program BusOptions mirror for completeness
                pci->MemoryWrite32(bar0Index, kOHCI_ConfigROMhdr, 0);
                pci->MemoryWrite32(bar0Index, kOHCI_BusOptions, ivars->configROMBusOptions);
                pci->MemoryWrite32(bar0Index, kOHCI_ConfigROMmap, (uint32_t)ivars->configROMSeg.address);
                os_log(ASLog(), "ASOHCI: ConfigROM mapped @ 0x%08x, BusOptions=0x%08x, VendorID=0x%06x, EUI64=%08x%08x",
                        (unsigned)ivars->configROMSeg.address, ivars->configROMBusOptions,
                        (unsigned)rom.vendorID(), (unsigned)guid_hi, (unsigned)guid_lo);
            } else {
                os_log(ASLog(), "ASOHCI: WARN: Config ROM DMA prepare failed: 0x%08x", ckr);
                dma->release();
            }
        } else {
            os_log(ASLog(), "ASOHCI: WARN: Config ROM DMA create failed: 0x%08x", ckr);
        }
    }
}

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
        // Log interrupt type for the bound source
        uint64_t itype = 0;
        if (IOInterruptDispatchSource::GetInterruptType(pci, 0, &itype) == kIOReturnSuccess) {
            os_log(ASLog(), "ASOHCI: Interrupt type bound (index 0): 0x%llx", (unsigned long long)itype);
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
    &ivars->selfIDBuffer
);
if (kr != kIOReturnSuccess || !ivars->selfIDBuffer) {
    os_log(ASLog(), "ASOHCI: IOBufferMemoryDescriptor::Create failed: 0x%08x", kr);
    return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;
}

// Map buffer into CPU address space for parsing
if (!ivars->selfIDMap) {
    kern_return_t mr = ivars->selfIDBuffer->CreateMapping(0, 0, 0, 0, 0, &ivars->selfIDMap);
    if (mr != kIOReturnSuccess || !ivars->selfIDMap) {
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
                        ivars->selfIDBuffer,
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
ivars->selfIDDMA = dma;
ivars->selfIDSeg = segs[0];
os_log(ASLog(), "ASOHCI: Self-ID IOVA=0x%llx len=0x%llx", (unsigned long long)ivars->selfIDSeg.address, (unsigned long long)ivars->selfIDSeg.length);
BRIDGE_LOG("Self-ID IOVA=0x%llx", (unsigned long long)ivars->selfIDSeg.address);

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
pci->MemoryWrite32(bar0Index, kOHCI_SelfIDBuffer, (uint32_t)ivars->selfIDSeg.address);
// Deferred cycle timer policy: only enable after first stable Self-ID
ivars->cycleTimerArmed = false;
// Enable reception of Self-ID and PHY packets now
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
if (!ivars->phyAccess) {
    ivars->phyAccess = new ASOHCIPHYAccess();
    if (ivars->phyAccess && !ivars->phyAccess->init(this, pci, (uint8_t)bar0Index)) {
        os_log(ASLog(), "ASOHCI: PHY access init failed (continuing without)" );
        delete ivars->phyAccess;
        ivars->phyAccess = nullptr;
    } else if (ivars->phyAccess) {
        os_log(ASLog(), "ASOHCI: PHY access initialized");
    }
}

// Phase 7: PHY Register Programming (Linux ohci_enable line 2514)
os_log(ASLog(), "ASOHCI: Phase 7 - PHY Register Programming");
if (ivars->phyAccess) {
    // Read current PHY register 4
    uint8_t currentVal = 0;
    if (ivars->phyAccess->readPhyRegister(kPHY_REG_4, &currentVal) == kIOReturnSuccess) {
        uint8_t newVal = currentVal | kPHY_LINK_ACTIVE | kPHY_CONTENDER;
        if (ivars->phyAccess->writePhyRegister(kPHY_REG_4, newVal) == kIOReturnSuccess) {
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
ivars->arRequestContext = new ASOHCIARContext();
if (ivars->arRequestContext) {
    kr = ivars->arRequestContext->Initialize(pci, ASOHCIARContext::AR_REQUEST_CONTEXT, (uint8_t)bar0Index);
    if (kr == kIOReturnSuccess) {
        kr = ivars->arRequestContext->Start();
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
ivars->arResponseContext = new ASOHCIARContext();
if (ivars->arResponseContext) {
    kr = ivars->arResponseContext->Initialize(pci, ASOHCIARContext::AR_RESPONSE_CONTEXT, (uint8_t)bar0Index);
    if (kr == kIOReturnSuccess) {
        kr = ivars->arResponseContext->Start();
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
ivars->atRequestContext = new ASOHCIATContext();
if (ivars->atRequestContext) {
    kr = ivars->atRequestContext->Initialize(pci, ASOHCIATContext::AT_REQUEST_CONTEXT, (uint8_t)bar0Index);
    if (kr == kIOReturnSuccess) {
        kr = ivars->atRequestContext->Start();
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
ivars->atResponseContext = new ASOHCIATContext();
if (ivars->atResponseContext) {
    kr = ivars->atResponseContext->Initialize(pci, ASOHCIATContext::AT_RESPONSE_CONTEXT, (uint8_t)bar0Index);
    if (kr == kIOReturnSuccess) {
        kr = ivars->atResponseContext->Start();
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
// Interrupt self-test: synthesize a CycleTooLong event to validate path
pci->MemoryWrite32(bar0Index, kOHCI_IntEventSet, kOHCI_Int_CycleTooLong);
IOSleep(1);
pci->MemoryWrite32(bar0Index, kOHCI_IntEventClear, kOHCI_Int_CycleTooLong);

// Phase 10: LinkEnable - Final Activation (OHCI 1.1 §5.7.3, Linux lines 2575-2581)
os_log(ASLog(), "ASOHCI: Phase 10 - Link Enable (Final Activation)");
// Linux parity: set BIBimageValid alongside LinkEnable. If Config ROM is not
// yet implemented, we may revisit; try this first as it matches Linux.
pci->MemoryWrite32(bar0Index, kOHCI_HCControlSet,
                    (kOHCI_HCControl_LinkEnable | kOHCI_HCControl_BIBimageValid));

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
// Align with Apple samples: call super Stop first to quiesce framework state
kern_return_t r = Stop(provider, SUPERDISPATCH);
os_log(ASLog(), "ASOHCI: Stop() begin - Total interrupts received: %llu", (unsigned long long)(ivars ? ivars->interruptCount : 0));
BRIDGE_LOG("Stop - IRQ count: %llu", (unsigned long long)(ivars ? ivars->interruptCount : 0));

// 1) Disable our dispatch source first to stop callbacks
if (ivars && ivars->intSource) {
os_log(ASLog(), "ASOHCI: Stop step 1 - disabling interrupt source");
ivars->intSource->SetEnableWithCompletion(false, nullptr);
ivars->intSource->release();
ivars->intSource = nullptr;
os_log(ASLog(), "ASOHCI: Interrupt source disabled");
}

// 2) Quiesce the controller: mask and clear all interrupts, stop link RX paths
if (ivars && ivars->pciDevice) {
os_log(ASLog(), "ASOHCI: Stop step 2 - mask/clear IRQs and link RX");
// Mask all interrupt sources including MasterEnable
ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IntMaskClear, 0xFFFFFFFFu);
// Clear any pending events to avoid later spurious IRQs on re-enable
ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IntEventClear,        0xFFFFFFFFu);
ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IsoXmitIntEventClear, 0xFFFFFFFFu);
ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IsoRecvIntEventClear, 0xFFFFFFFFu);
// Also clear iso masks for completeness
ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IsoXmitIntMaskClear,  0xFFFFFFFFu);
ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IsoRecvIntMaskClear,  0xFFFFFFFFu);

// Disable Self-ID/PHY receive and cycle timer
ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_LinkControlClear,
                            (kOHCI_LC_RcvSelfID | kOHCI_LC_RcvPhyPkt | kOHCI_LC_CycleTimerEnable));
// Readback to flush posted writes
uint32_t _lc=0; ivars->pciDevice->MemoryRead32(ivars->barIndex, kOHCI_LinkControlSet, &_lc);
// Clear Config ROM map to decouple DMA before freeing memory
ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_ConfigROMmap, 0);
}

// 3) Stop AR/AT contexts gracefully (clear run, wait inactive) before freeing backing memory
if (ivars && ivars->arRequestContext) {
os_log(ASLog(), "ASOHCI: Stop step 3a - stopping AR Request");
ivars->arRequestContext->Stop();
delete ivars->arRequestContext;
ivars->arRequestContext = nullptr;
os_log(ASLog(), "ASOHCI: AR Request context stopped and released");
}
if (ivars && ivars->arResponseContext) {
os_log(ASLog(), "ASOHCI: Stop step 3b - stopping AR Response");
ivars->arResponseContext->Stop();
delete ivars->arResponseContext;
ivars->arResponseContext = nullptr;
os_log(ASLog(), "ASOHCI: AR Response context stopped and released");
}
if (ivars && ivars->atRequestContext) {
os_log(ASLog(), "ASOHCI: Stop step 3c - stopping AT Request");
ivars->atRequestContext->Stop();
delete ivars->atRequestContext;
ivars->atRequestContext = nullptr;
os_log(ASLog(), "ASOHCI: AT Request context stopped and released");
}
if (ivars && ivars->atResponseContext) {
os_log(ASLog(), "ASOHCI: Stop step 3d - stopping AT Response");
ivars->atResponseContext->Stop();
delete ivars->atResponseContext;
ivars->atResponseContext = nullptr;
os_log(ASLog(), "ASOHCI: AT Response context stopped and released");
}

// 4) Disarm Self-ID receive and scrub registers before freeing buffers
if (ivars && ivars->pciDevice) {
os_log(ASLog(), "ASOHCI: Stop step 4 - disarm Self-ID registers");
ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_SelfIDCount, 0);
ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_SelfIDBuffer, 0);
}

// 5) Assert SoftReset and drop LinkEnable to stop the link state machine
if (ivars && ivars->pciDevice) {
os_log(ASLog(), "ASOHCI: Stop step 5 - soft reset + drop LinkEnable");
// Clear LinkEnable and aPhyEnhanceEnable bits
ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_HCControlClear,
                            (kOHCI_HCControl_LinkEnable | kOHCI_HCControl_aPhyEnhanceEnable));
// Soft reset the controller
ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_HCControlSet, kOHCI_HCControl_SoftReset);
IOSleep(10);
// Readback to ensure reset posted
uint32_t _hc=0; ivars->pciDevice->MemoryRead32(ivars->barIndex, kOHCI_HCControlSet, &_hc);
os_log(ASLog(), "ASOHCI: HC soft reset during Stop (HCControl=0x%08x)", _hc);
}

// 6) Now free DMA resources in safe order
if (ivars && ivars->selfIDDMA) {
os_log(ASLog(), "ASOHCI: Stop step 6a - release Self-ID DMA");
ivars->selfIDDMA->CompleteDMA(kIODMACommandCompleteDMANoOptions);
ivars->selfIDDMA->release();
ivars->selfIDDMA = nullptr;
}
if (ivars && ivars->selfIDMap) {
os_log(ASLog(), "ASOHCI: Stop step 6b - release Self-ID map");
ivars->selfIDMap->release();
ivars->selfIDMap = nullptr;
}
if (ivars && ivars->selfIDBuffer) {
os_log(ASLog(), "ASOHCI: Stop step 6c - release Self-ID buffer");
ivars->selfIDBuffer->release();
ivars->selfIDBuffer = nullptr;
os_log(ASLog(), "ASOHCI: Self-ID buffer released");
BRIDGE_LOG("Self-ID buffer released");
}
// 7) Release Config ROM resources
if (ivars && ivars->configROMDMA) {
os_log(ASLog(), "ASOHCI: Stop step 7 - release Config ROM DMA/map/buffer");
ivars->configROMDMA->CompleteDMA(kIODMACommandCompleteDMANoOptions);
ivars->configROMDMA->release();
ivars->configROMDMA = nullptr;
}
if (ivars && ivars->configROMMap) {
ivars->configROMMap->release();
ivars->configROMMap = nullptr;
}
if (ivars && ivars->configROMBuffer) {
ivars->configROMBuffer->release();
ivars->configROMBuffer = nullptr;
}

// 7) Release PHY helper
if (ivars && ivars->phyAccess) {
os_log(ASLog(), "ASOHCI: Stop step 7 - delete PHY access helper");
delete ivars->phyAccess;
ivars->phyAccess = nullptr;
}

// 8) Best-effort: disable BM/MEM space and close
if (auto pci = OSDynamicCast(IOPCIDevice, provider)) {
os_log(ASLog(), "ASOHCI: Stop step 8 - clear PCI CMD and Close");
uint16_t cmd=0; pci->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &cmd);
uint16_t clr = (uint16_t)(cmd & ~(kIOPCICommandBusMaster | kIOPCICommandMemorySpace));
if (clr != cmd) pci->ConfigurationWrite16(kIOPCIConfigurationOffsetCommand, clr);
pci->Close(this, 0);
}

if (ivars) {
ivars->pciDevice = nullptr;
ivars->barIndex = 0;
ivars->interruptCount = 0;
}

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
if (!ivars) {
    return;
}
uint64_t seq = __atomic_add_fetch(&ivars->interruptCount, 1, __ATOMIC_RELAXED);
os_log(ASLog(), "ASOHCI: InterruptOccurred #%llu (count=%llu time=%llu)",
        (unsigned long long)seq, (unsigned long long)count, (unsigned long long)time);
BRIDGE_LOG("IRQ #%llu hwcount=%llu", (unsigned long long)seq, (unsigned long long)count);

if (!ivars->pciDevice) {
    os_log(ASLog(), "ASOHCI: No PCI device bound; spurious?");
    return;
}

uint32_t intEvent = 0;
ivars->pciDevice->MemoryRead32(ivars->barIndex, kOHCI_IntEvent, &intEvent);
if (intEvent == 0) {
os_log(ASLog(), "ASOHCI: Spurious MSI (IntEvent=0)");
return;
}

// Ack/clear what we saw (write-1-to-clear), but per OHCI 1.1 and Linux parity
// do not clear busReset or postedWriteErr in this bulk clear.
uint32_t clearMask = intEvent & ~(kOHCI_Int_BusReset | kOHCI_Int_PostedWriteErr);
if (clearMask)
ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IntEventClear, clearMask);
os_log(ASLog(), "ASOHCI: IntEvent=0x%08x", intEvent);
BRIDGE_LOG("IRQ events=0x%08x", intEvent);
LogUtils::DumpIntEvent(intEvent);

// Handle Posted Write Error per spec: read posted write address regs then clear
if (intEvent & kOHCI_Int_PostedWriteErr) {
uint32_t _hi=0, _lo=0;
ivars->pciDevice->MemoryRead32(ivars->barIndex, kOHCI_PostedWriteAddressHi, &_hi);
ivars->pciDevice->MemoryRead32(ivars->barIndex, kOHCI_PostedWriteAddressLo, &_lo);
ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IntEventClear, kOHCI_Int_PostedWriteErr);
os_log(ASLog(), "ASOHCI: Posted Write Error addr=%08x:%08x (cleared)", _hi, _lo);
}

// Bus reset (coalesce repeated resets until SelfIDComplete)
if (intEvent & kOHCI_Int_BusReset) {
// Mask busReset until bus reset handling completes (Linux parity)
ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IntMaskClear, kOHCI_Int_BusReset);
// Per OHCI §5.5, if a Config ROM image is staged, update BusOptions then Header now.
if (ivars->configROMHeaderNeedsCommit && ivars->configROMHeaderQuad != 0) {
    ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_BusOptions, ivars->configROMBusOptions);
    ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_ConfigROMhdr, ivars->configROMHeaderQuad);
    os_log(ASLog(), "ASOHCI: ConfigROM header committed on BusReset (BusOptions=0x%08x, Header=0x%08x)",
            ivars->configROMBusOptions, ivars->configROMHeaderQuad);
    // Optional: short post-commit dump (trimmed) for verification
    if (ivars->configROMMap) {
        void* romPtr = (void*)ivars->configROMMap->GetAddress();
        size_t romLen = (size_t)ivars->configROMMap->GetLength();
        DumpHexBigEndian(romPtr, romLen, "CONFIG ROM POST-COMMIT DUMP HEX");
    }
    ivars->configROMHeaderNeedsCommit = false;
}
// Stop/flush AT contexts during reset window (parity with Linux behavior)
if (ivars->atRequestContext)  { ivars->atRequestContext->Stop(); }
if (ivars->atResponseContext) { ivars->atResponseContext->Stop(); }
if (ivars->selfIDInProgress) {
    // Already in a reset/self-id cycle; collapse this extra signal
    ++ivars->collapsedBusResets;
    BRIDGE_LOG("Collapsed BusReset (total collapsed=%u)", ivars->collapsedBusResets);
} else {
    ivars->selfIDInProgress = true;
    ivars->collapsedBusResets = 0;
    BRIDGE_LOG("Bus reset (new cycle)");
    os_log(ASLog(), "ASOHCI: Bus reset (new cycle)");
    // Arm Self-ID fresh (clearCount true at start of cycle)
    ArmSelfIDReceive(/*clearCount=*/true);
}
// Ack the BusReset event bit before re-enabling its mask later
ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IntEventClear, kOHCI_Int_BusReset);
// NodeID logging only when it changes or validity/root status toggles
uint32_t nodeID=0; ivars->pciDevice->MemoryRead32(ivars->barIndex, kOHCI_NodeID, &nodeID);
bool idValid = ((nodeID >> 31) & 1) != 0;
bool isRoot  = ((nodeID >> 30) & 1) != 0;
if (nodeID != ivars->lastLoggedNodeID || idValid != ivars->lastLoggedValid || isRoot != ivars->lastLoggedRoot) {
    uint8_t nAdr = (uint8_t)((nodeID >> 16) & 0x3F);
    os_log(ASLog(), "ASOHCI: NodeID=0x%08x valid=%d root=%d addr=%u (changed)", nodeID, idValid, isRoot, nAdr);
    BRIDGE_LOG("NodeID change %08x v=%d r=%d addr=%u", nodeID, idValid, isRoot, nAdr);
    ivars->lastLoggedNodeID = nodeID;
    ivars->lastLoggedValid  = idValid;
    ivars->lastLoggedRoot   = isRoot;
}
}

// NOTE: Self-ID complete will deliver alpha self-ID quadlets (#0 and optional #1/#2).
// Parser implements IEEE 1394-2008 §16.3.2.1 (Alpha). Beta support can be added later.

// Self-ID complete
if (intEvent & kOHCI_Int_SelfIDComplete) {
os_log(ASLog(), "ASOHCI: Self-ID phase complete");
BRIDGE_LOG("Self-ID complete");

uint32_t selfIDCount1=0, selfIDCount2=0;
ivars->pciDevice->MemoryRead32(ivars->barIndex, kOHCI_SelfIDCount, &selfIDCount1);

uint32_t quads = (selfIDCount1 & kOHCI_SelfIDCount_selfIDSize) >> 2;
bool     err   = (selfIDCount1 & kOHCI_SelfIDCount_selfIDError) != 0;
os_log(ASLog(), "ASOHCI: SelfID count=%u quads, error=%d", quads, err);
BRIDGE_LOG("SelfID count=%u error=%d", quads, err);

if (!err && quads > 0 && ivars->selfIDMap) {
    uint32_t *ptr = (uint32_t *)(uintptr_t) ivars->selfIDMap->GetAddress();
    size_t     len = (size_t) ivars->selfIDMap->GetLength();
    if (ptr && len >= quads * sizeof(uint32_t)) {
        SelfIDParser::Process(ptr, quads);
    } else {
        os_log(ASLog(), "ASOHCI: Self-ID CPU mapping invalid for parse");
    }
}
// Generation consistency check: ensure no new bus reset occurred during parse
ivars->pciDevice->MemoryRead32(ivars->barIndex, kOHCI_SelfIDCount, &selfIDCount2);
if ((selfIDCount1 & kOHCI_SelfIDCount_selfIDGeneration) != (selfIDCount2 & kOHCI_SelfIDCount_selfIDGeneration)) {
    os_log(ASLog(), "ASOHCI: New bus reset detected during Self-ID parse; discarding results");
    // Re-arm Self-ID for next cycle, keep busReset masked until next event handler run
    ArmSelfIDReceive(/*clearCount=*/false);
    ivars->selfIDInProgress = false;
    ivars->selfIDArmed = true;
    return;
}
// First stable Self-ID -> enable cycle timer if not yet (deferred policy)
if (!ivars->cycleTimerArmed && ivars->pciDevice) {
    ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_LinkControlSet, kOHCI_LC_CycleTimerEnable);
    uint32_t lcPost=0; ivars->pciDevice->MemoryRead32(ivars->barIndex, kOHCI_LinkControlSet, &lcPost);
    os_log(ASLog(), "ASOHCI: CycleTimerEnable asserted post Self-ID (LinkControl=0x%08x)", lcPost);
    BRIDGE_LOG("CycleTimerEnable now set (LC=%08x)", lcPost);
    ivars->cycleTimerArmed = true;
}
// Restart AT contexts after successful Self-ID processing
if (ivars->atRequestContext)  { ivars->atRequestContext->Start(); }
if (ivars->atResponseContext) { ivars->atResponseContext->Start(); }
// Perform one-time PHY scan (read-only) after first stable Self-ID
if (!ivars->didInitialPhyScan && ivars->phyAccess) {
    // Inline small scan to avoid extra TU for now
    const uint8_t kMaxPhyPorts = 16; // hard cap
    
    // OHCI 5.12: "Software shall not issue a read of PHY register 0. 
    // The most recently available contents of this register shall be reflected in the NodeID register"
    uint32_t nodeIdReg = 0;
    ivars->pciDevice->MemoryRead32(ivars->barIndex, kOHCI_NodeID, &nodeIdReg);
    uint8_t localPhyId = (uint8_t)((nodeIdReg >> 24) & 0x3F); // NodeID register bits 29:24 contain PHY ID
    
    BRIDGE_LOG("PHY scan start localPhyId=%u (from NodeID=0x%08x)", localPhyId, nodeIdReg);
    os_log(ASLog(), "ASOHCI: PHY scan start localPhyId=%u (from NodeID=0x%08x)", localPhyId, nodeIdReg);
    
    uint32_t connectedCount = 0, enabledCount = 0, contenderCount = 0;
    uint8_t portBaseReg = 4; // typical start of port status regs
    for (uint8_t p = 0; p < kMaxPhyPorts; ++p) {
            uint8_t raw = 0; uint8_t reg = (uint8_t)(portBaseReg + p);
            if (ivars->phyAccess->readPhyRegister(reg, &raw) != kIOReturnSuccess) {
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
    ivars->didInitialPhyScan = true;
}
// Mark cycle complete; prepare for future resets but do not override in-progress flags until next BusReset
ivars->selfIDInProgress = false;
ivars->selfIDArmed = false;
if (ivars->collapsedBusResets) {
    os_log(ASLog(), "ASOHCI: Collapsed %u BusReset IRQs in cycle", ivars->collapsedBusResets);
    BRIDGE_LOG("Collapsed %u BusResets", ivars->collapsedBusResets);
}
// Re-arm to listen for next cycle (do not clear count again until next BusReset)
ArmSelfIDReceive(/*clearCount=*/false);
// Re-enable busReset interrupt now that reset cycle finished
ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_IntMaskSet, kOHCI_Int_BusReset);
}

// AR/AT Context Interrupt Handling (OHCI 1.1 §6.1 bits 0-3)
if (intEvent & (kOHCI_Int_ARRQ | kOHCI_Int_ARRS | kOHCI_Int_ReqTxComplete | kOHCI_Int_RespTxComplete)) {

// AR Request context interrupt (bit 2)
if ((intEvent & kOHCI_Int_ARRQ) && ivars->arRequestContext) {
    ivars->arRequestContext->HandleInterrupt();
}

// AR Response context interrupt (bit 3) 
if ((intEvent & kOHCI_Int_ARRS) && ivars->arResponseContext) {
    ivars->arResponseContext->HandleInterrupt();
}

// AT Request context interrupt (bit 0)
if ((intEvent & kOHCI_Int_ReqTxComplete) && ivars->atRequestContext) {
    ivars->atRequestContext->HandleInterrupt();
}

// AT Response context interrupt (bit 1)
if ((intEvent & kOHCI_Int_RespTxComplete) && ivars->atResponseContext) {
    ivars->atResponseContext->HandleInterrupt();
}
}

// If cycle too long, assert cycle master again (Linux parity)
if (intEvent & kOHCI_Int_CycleTooLong) {
ivars->pciDevice->MemoryWrite32(ivars->barIndex, kOHCI_LinkControlSet, kOHCI_LC_CycleMaster);
}

// All interrupt bits are now handled by the comprehensive DumpIntEvent function
// No need for generic "Other IRQ bits" logging as every bit is properly identified per OHCI §6.1
}
