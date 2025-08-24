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

// PCI Configuration offsets
#define kIOPCIConfigurationOffsetVendorID         0x00
#define kIOPCIConfigurationOffsetDeviceID         0x02
#define kIOPCIConfigurationOffsetCommand          0x04

// PCI Command register bits
#define kIOPCICommandMemorySpace                  0x0002
#define kIOPCICommandBusMaster                    0x0004

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

#if DEBUG
    os_log(ASLog(), "[BRIDGE] %s", e.msg);
#endif
}

#define BRIDGE_LOG(fmt, ...) do { bridge_logf(fmt, ##__VA_ARGS__); } while(0)

// Minimal Start to diagnose restart loop: call super and return
kern_return_t
IMPL(ASOHCI, Start)
{
    kern_return_t kr = Start(provider, SUPERDISPATCH);
    if (kr != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCI: Start superdispatch failed: 0x%08x", kr);
        return kr;
    }
    os_log(ASLog(), "ASOHCI: Minimal Start() reached");
    BRIDGE_LOG("Minimal Start()");
    return kIOReturnSuccess;
}

kern_return_t
IMPL(ASOHCI, Stop)
{
    os_log(ASLog(), "ASOHCI: Minimal Stop() reached");
    BRIDGE_LOG("Minimal Stop()");
    return Stop(provider, SUPERDISPATCH);
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
