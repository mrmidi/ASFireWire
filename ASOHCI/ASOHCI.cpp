//
//  ASOHCI.cpp
//  ASOHCI
//
//  Created by Aleksandr Shabelnikov on 23.08.2025.
//

#include <os/log.h>
#include <TargetConditionals.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

#include <DriverKit/IOUserServer.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSData.h>
#include <PCIDriverKit/IOPCIDevice.h>
// Apple PCI config register offsets and command bits
#include "../../AppleHeaders/PCI/IOPCIFamilyDefinitions.h"

// Minimal OHCI 1394 register offsets (from OHCI 1394 spec)
static constexpr uint32_t kOHCI_Version                 = 0x000;
static constexpr uint32_t kOHCI_BusOptions              = 0x020;
static constexpr uint32_t kOHCI_GUIDHi                  = 0x024;
static constexpr uint32_t kOHCI_GUIDLo                  = 0x028;
static constexpr uint32_t kOHCI_HCControlSet            = 0x050;
static constexpr uint32_t kOHCI_HCControlClear          = 0x054;
static constexpr uint32_t kOHCI_NodeID                  = 0x0E8;
static constexpr uint32_t kOHCI_IntEventClear           = 0x084;
static constexpr uint32_t kOHCI_IntMaskSet              = 0x088;
static constexpr uint32_t kOHCI_IntMaskClear            = 0x08C;
static constexpr uint32_t kOHCI_IsoXmitIntEventClear    = 0x094;
static constexpr uint32_t kOHCI_IsoXmitIntMaskClear     = 0x09C;
static constexpr uint32_t kOHCI_IsoRecvIntEventClear    = 0x0A4;
static constexpr uint32_t kOHCI_IsoRecvIntMaskClear     = 0x0AC;

// HCControl bits
static constexpr uint32_t kOHCI_HCControl_SoftReset     = 0x00010000;
static constexpr uint32_t kOHCI_HCControl_PostedWriteEn = 0x00040000;
static constexpr uint32_t kOHCI_HCControl_LinkEnable    = 0x00020000;
static constexpr uint32_t kOHCI_HCControl_LPS           = 0x00080000;

#include "ASOHCI.h"

//------------------------------------------------------------------------------
// Logging helper: dedicated subsystem/category for easy filtering.
// On DriverKit, fall back to OS_LOG_DEFAULT (os_log_create may be unavailable).
//------------------------------------------------------------------------------
static inline os_log_t ASLog()
{
#if defined(TARGET_OS_DRIVERKIT) && TARGET_OS_DRIVERKIT
    return OS_LOG_DEFAULT;
#else
    static os_log_t log = os_log_create("net.mrmidi.ASFireWire", "ASOHCI");
    return log;
#endif
}

// using constants from IOPCIFamilyDefinitions.h

// --------------------------------------------------------------------------
// Bridge logging: lightweight in-memory ring buffer + export via IIG method
// --------------------------------------------------------------------------

#define BRIDGE_LOG_MSG_MAX    160u
#define BRIDGE_LOG_CAPACITY   256u   // number of entries

typedef struct {
    uint64_t seq;
    uint64_t ts_nanos; // optional timestamp (monotonic), may be 0 if unavailable
    uint8_t  level;    // future use
    char     msg[BRIDGE_LOG_MSG_MAX];
} bridge_log_entry_t;

static bridge_log_entry_t g_bridge_log[BRIDGE_LOG_CAPACITY];
static volatile uint64_t g_bridge_seq = 0; // ever-incrementing

static inline uint64_t bridge_now_nanos()
{
#if defined(CLOCK_UPTIME_RAW)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    return 0;
#endif
}

static void bridge_logf(const char *fmt, ...)
{
    char buf[BRIDGE_LOG_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    uint64_t seq = __atomic_add_fetch(&g_bridge_seq, 1, __ATOMIC_RELAXED);
    uint32_t idx = (uint32_t)(seq % BRIDGE_LOG_CAPACITY);
    bridge_log_entry_t &e = g_bridge_log[idx];
    e.seq = seq;
    e.ts_nanos = bridge_now_nanos();
    e.level = 0;
    // Ensure msg is always NUL-terminated
    size_t n = strnlen(buf, sizeof(buf));
    if (n >= sizeof(e.msg)) n = sizeof(e.msg) - 1;
    memcpy(e.msg, buf, n);
    e.msg[n] = '\0';

// Temporarily disable bridge os_log output to reduce noise
#if 0
    os_log(ASLog(), "[BRIDGE] %s", e.msg);
#endif
}

#define BRIDGE_LOG(fmt, ...) do { bridge_logf(fmt, ##__VA_ARGS__); } while(0)

// Guided Start: Open PCI, enable command bits, log BARs, read OHCI regs
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

    // Cast provider to IOPCIDevice
    auto pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) {
        os_log(ASLog(), "ASOHCI: Provider is not IOPCIDevice");
        return kIOReturnBadArgument;
    }

    // Open the device for exclusive access
    kr = pci->Open(this, 0);
    if (kr != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCI: PCI Open failed: 0x%08x", kr);
        return kr;
    }

    // Read IDs
    uint16_t vendorID = 0, deviceID = 0;
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetVendorID, &vendorID);
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetDeviceID, &deviceID);
    os_log(ASLog(), "ASOHCI: PCI IDs V:0x%04x D:0x%04x", vendorID, deviceID);
    BRIDGE_LOG("PCI IDs V=%04x D=%04x", vendorID, deviceID);

    // Enable BusMaster/MemorySpace
    uint16_t cmd = 0;
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &cmd);
    uint16_t newCmd = cmd | (kIOPCICommandBusLead | kIOPCICommandMemorySpace);
    if (newCmd != cmd) {
        pci->ConfigurationWrite16(kIOPCIConfigurationOffsetCommand, newCmd);
        pci->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &newCmd);
    }
    os_log(ASLog(), "ASOHCI: PCI CMD=0x%04x (was 0x%04x)", newCmd, cmd);
    BRIDGE_LOG("PCI CMD=0x%04x->0x%04x", cmd, newCmd);

    // BAR enumeration and BAR0 memory index
    uint8_t bar0Index = 0;
    uint64_t bar0Size = 0;
    uint8_t bar0Type = 0;
    kr = pci->GetBARInfo(0 /* BAR0 */, &bar0Index, &bar0Size, &bar0Type);
    if (kr == kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCI: BAR0 idx=%u size=0x%llx type=0x%02x", bar0Index, bar0Size, bar0Type);
        BRIDGE_LOG("BAR0 idx=%u size=0x%llx type=0x%02x", bar0Index, bar0Size, bar0Type);
    } else {
        os_log(ASLog(), "ASOHCI: GetBARInfo(BAR0) failed: 0x%08x", kr);
    }

    // Try reading a few OHCI registers if BAR0 present
    if (bar0Size >= 0x2C) {
        uint32_t ohci_ver = 0, bus_opts = 0, guid_hi = 0, guid_lo = 0;
        pci->MemoryRead32(bar0Index, kOHCI_Version, &ohci_ver);
        pci->MemoryRead32(bar0Index, kOHCI_BusOptions, &bus_opts);
        pci->MemoryRead32(bar0Index, kOHCI_GUIDHi, &guid_hi);
        pci->MemoryRead32(bar0Index, kOHCI_GUIDLo, &guid_lo);
        os_log(ASLog(), "ASOHCI: OHCI VER=0x%08x BUSOPT=0x%08x GUID=%08x:%08x", ohci_ver, bus_opts, guid_hi, guid_lo);
        BRIDGE_LOG("OHCI VER=%08x BUSOPT=%08x GUID=%08x:%08x", ohci_ver, bus_opts, guid_hi, guid_lo);

        // --- Minimal controller init: clear pending interrupts and masks ---
        uint32_t allOnes = 0xFFFFFFFFu;
        pci->MemoryWrite32(bar0Index, kOHCI_IntEventClear,        allOnes);
        pci->MemoryWrite32(bar0Index, kOHCI_IsoXmitIntEventClear,  allOnes);
        pci->MemoryWrite32(bar0Index, kOHCI_IsoRecvIntEventClear,  allOnes);
        pci->MemoryWrite32(bar0Index, kOHCI_IntMaskClear,          allOnes);
        pci->MemoryWrite32(bar0Index, kOHCI_IsoXmitIntMaskClear,   allOnes);
        pci->MemoryWrite32(bar0Index, kOHCI_IsoRecvIntMaskClear,   allOnes);
        os_log(ASLog(), "ASOHCI: Cleared interrupt events/masks");
        BRIDGE_LOG("IRQ clear/mask done");

        // Optional: issue a soft reset and wait briefly for completion
        pci->MemoryWrite32(bar0Index, kOHCI_HCControlSet,   kOHCI_HCControl_SoftReset);
        IOSleep(10); // give hardware time to settle
        os_log(ASLog(), "ASOHCI: Soft reset issued");
        BRIDGE_LOG("Soft reset issued");

        // Re-clear masks/events after reset just to be safe
        pci->MemoryWrite32(bar0Index, kOHCI_IntEventClear,        allOnes);
        pci->MemoryWrite32(bar0Index, kOHCI_IsoXmitIntEventClear,  allOnes);
        pci->MemoryWrite32(bar0Index, kOHCI_IsoRecvIntEventClear,  allOnes);
        pci->MemoryWrite32(bar0Index, kOHCI_IntMaskClear,          allOnes);
        pci->MemoryWrite32(bar0Index, kOHCI_IsoXmitIntMaskClear,   allOnes);
        pci->MemoryWrite32(bar0Index, kOHCI_IsoRecvIntMaskClear,   allOnes);
        os_log(ASLog(), "ASOHCI: Post-reset interrupt clear complete");
        BRIDGE_LOG("IRQ clear after reset done");

        // Bring controller to link power state, enable posted writes (no interrupts yet)
        uint32_t hcSet = (kOHCI_HCControl_LPS | kOHCI_HCControl_PostedWriteEn);
        pci->MemoryWrite32(bar0Index, kOHCI_HCControlSet, hcSet);
        os_log(ASLog(), "ASOHCI: HCControlSet LPS+PostedWrite (0x%08x)", hcSet);

        // Probe NodeID
        uint32_t node_id = 0;
        pci->MemoryRead32(bar0Index, kOHCI_NodeID, &node_id);
        os_log(ASLog(), "ASOHCI: NodeID=0x%08x (idValid=%u root=%u)", node_id,
               (node_id >> 31) & 0x1, (node_id >> 30) & 0x1);

        // Optionally enable the Link state now; keep interrupts masked until MSI is wired
        pci->MemoryWrite32(bar0Index, kOHCI_HCControlSet, kOHCI_HCControl_LinkEnable);
        os_log(ASLog(), "ASOHCI: HCControlSet LinkEnable");
    } else {
        os_log(ASLog(), "ASOHCI: BAR0 too small (0x%llx) to read OHCI regs", bar0Size);
    }

    os_log(ASLog(), "ASOHCI: Start() bring-up complete");
    BRIDGE_LOG("Bring-up complete");
    return kIOReturnSuccess;
}

kern_return_t
IMPL(ASOHCI, Stop)
{
    os_log(ASLog(), "ASOHCI: Stop() begin");
    // Best-effort clean up
    auto pci = OSDynamicCast(IOPCIDevice, provider);
    if (pci) {
        uint16_t cmd = 0;
        pci->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &cmd);
        uint16_t clr = (uint16_t)(cmd & ~(kIOPCICommandBusLead | kIOPCICommandMemorySpace));
        if (clr != cmd) {
            pci->ConfigurationWrite16(kIOPCIConfigurationOffsetCommand, clr);
        }
        pci->Close(this, 0);
    }
    kern_return_t r = Stop(provider, SUPERDISPATCH);
    os_log(ASLog(), "ASOHCI: Stop() complete: 0x%08x", r);
    return r;
}

// CopyBridgeLogs implementation: return newline-delimited UTF-8 lines via OSData
kern_return_t
IMPL(ASOHCI, CopyBridgeLogs)
{
    if (outData == nullptr) {
        return kIOReturnBadArgument;
    }
    *outData = nullptr;

    // Worst-case buffer: all entries with max line length
    uint64_t seqNow = __atomic_load_n(&g_bridge_seq, __ATOMIC_RELAXED);
    const size_t maxLines = (seqNow < BRIDGE_LOG_CAPACITY) ? (size_t)seqNow : (size_t)BRIDGE_LOG_CAPACITY;
    const size_t maxBytes = maxLines * (BRIDGE_LOG_MSG_MAX + 32);
    if (maxLines == 0) {
        OSData *empty = OSData::withBytes("", 1);
        if (empty) {
            *outData = empty;
            return kIOReturnSuccess;
        }
        return kIOReturnNoMemory;
    }

    // Assemble into a temporary buffer, then wrap into OSData
    // Allocate on heap to avoid large stack usage
    char *buf = (char *)IOMalloc(maxBytes);
    if (!buf) return kIOReturnNoMemory;
    size_t written = 0;

    uint64_t minSeq = (seqNow > BRIDGE_LOG_CAPACITY) ? (seqNow - BRIDGE_LOG_CAPACITY) : 0;
    uint64_t startSeq = (minSeq ? minSeq + 1 : 1);

    for (uint64_t s = startSeq; s <= seqNow; ++s) {
        uint32_t idx = (uint32_t)(s % BRIDGE_LOG_CAPACITY);
        bridge_log_entry_t e = g_bridge_log[idx];
        if (e.seq != s) continue; // wrapped
        char line[BRIDGE_LOG_MSG_MAX + 32];
        int n = snprintf(line, sizeof(line), "%llu %s\n", (unsigned long long)e.seq, e.msg);
        if (n <= 0) continue;
        if (written + (size_t)n > maxBytes) break;
        memcpy(buf + written, line, (size_t)n);
        written += (size_t)n;
    }

    OSData *data = OSData::withBytes(buf, written);
    IOFree(buf, maxBytes);
    if (!data) return kIOReturnNoMemory;
    *outData = data;
    return kIOReturnSuccess;
}
