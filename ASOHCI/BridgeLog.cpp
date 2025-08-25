// BridgeLog.cpp
// Internal ring buffer logger for DriverKit

#include "BridgeLog.hpp"

#include <os/log.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

#if defined(TARGET_OS_DRIVERKIT)
#include <TargetConditionals.h>
#endif

#if defined(CLOCK_UPTIME_RAW)
#include <time.h>
#endif

// Capacity and message sizing
#define BRIDGE_LOG_MSG_MAX    160u
#define BRIDGE_LOG_CAPACITY   256u

typedef struct {
    uint64_t seq;
    uint64_t ts_nanos;
    uint8_t  level;
    char     msg[BRIDGE_LOG_MSG_MAX];
} bridge_log_entry_t;

static bridge_log_entry_t g_bridge_log[BRIDGE_LOG_CAPACITY];
static volatile uint64_t  g_bridge_seq = 0;
static bool               g_bridge_inited = false;

static inline uint64_t bridge_now_nanos() {
#if defined(CLOCK_UPTIME_RAW)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    return 0;
#endif
}

void bridge_log_init()
{
    if (__atomic_exchange_n(&g_bridge_inited, true, __ATOMIC_ACQ_REL)) {
        return; // already initialized
    }
    memset((void*)g_bridge_log, 0, sizeof(g_bridge_log));
    __atomic_store_n(&g_bridge_seq, 0, __ATOMIC_RELAXED);
}

void bridge_logf(const char* fmt, ...)
{
    if (!g_bridge_inited) bridge_log_init();

    char buf[BRIDGE_LOG_MSG_MAX];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    uint64_t seq = __atomic_add_fetch(&g_bridge_seq, 1, __ATOMIC_RELAXED);
    uint32_t idx = (uint32_t)(seq % BRIDGE_LOG_CAPACITY);
    bridge_log_entry_t &e = g_bridge_log[idx];
    e.seq = seq;
    e.ts_nanos = bridge_now_nanos();
    e.level = 0;
    size_t n = strnlen(buf, sizeof(buf));
    if (n >= sizeof(e.msg)) n = sizeof(e.msg) - 1;
    memcpy(e.msg, buf, n);
    e.msg[n] = '\0';
}

kern_return_t bridge_log_copy(OSData** outData)
{
    if (outData == nullptr) {
        return kIOReturnBadArgument;
    }
    *outData = nullptr;

    if (!g_bridge_inited) bridge_log_init();
    uint64_t seqNow = __atomic_load_n(&g_bridge_seq, __ATOMIC_RELAXED);
    const size_t maxLines = (seqNow < BRIDGE_LOG_CAPACITY) ? (size_t)seqNow : (size_t)BRIDGE_LOG_CAPACITY;
    if (maxLines == 0) {
        OSData *empty = OSData::withBytes("", 1);
        if (empty) { *outData = empty; return kIOReturnSuccess; }
        return kIOReturnNoMemory;
    }

    const size_t maxBytes = maxLines * (BRIDGE_LOG_MSG_MAX + 32);
    char *buf = (char *)IOMalloc(maxBytes);
    if (!buf) return kIOReturnNoMemory;
    size_t written = 0;

    uint64_t minSeq = (seqNow > BRIDGE_LOG_CAPACITY) ? (seqNow - BRIDGE_LOG_CAPACITY) : 0;
    uint64_t startSeq = (minSeq ? minSeq + 1 : 1);

    for (uint64_t s = startSeq; s <= seqNow; ++s) {
        uint32_t idx = (uint32_t)(s % BRIDGE_LOG_CAPACITY);
        bridge_log_entry_t e = g_bridge_log[idx];
        if (e.seq != s) continue;
        char line[BRIDGE_LOG_MSG_MAX + 32];
        int n = snprintf(line, sizeof(line), "%llu %s\n",
                         (unsigned long long)e.seq, e.msg);
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

