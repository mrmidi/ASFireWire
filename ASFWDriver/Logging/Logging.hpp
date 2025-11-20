#pragma once

#include <os/log.h>
#include <atomic>
#ifdef ASFW_HOST_TEST
#include <mach/mach_time.h>
#else
#include <DriverKit/IOLib.h> // mach_time
#endif

#ifndef OS_LOG_TYPE_DEFAULT
#define OS_LOG_TYPE_DEFAULT static_cast<os_log_type_t>(0x00)
#endif

#ifndef OS_LOG_TYPE_INFO
#define OS_LOG_TYPE_INFO static_cast<os_log_type_t>(0x01)
#endif

#ifndef OS_LOG_TYPE_ERROR
#define OS_LOG_TYPE_ERROR static_cast<os_log_type_t>(0x10)
#endif

#ifndef OS_LOG_TYPE_FAULT
#define OS_LOG_TYPE_FAULT static_cast<os_log_type_t>(0x11)
#endif

#ifndef OS_LOG_TYPE_DEBUG
#define OS_LOG_TYPE_DEBUG OS_LOG_TYPE_DEFAULT
#endif

#ifndef os_log_info
#define os_log_info(log, fmt, ...) os_log_with_type((log), OS_LOG_TYPE_INFO, fmt, ##__VA_ARGS__)
#endif

#ifndef os_log_error
#define os_log_error(log, fmt, ...) os_log_with_type((log), OS_LOG_TYPE_ERROR, fmt, ##__VA_ARGS__)
#endif

#ifndef os_log_debug
#define os_log_debug(log, fmt, ...) os_log_with_type((log), OS_LOG_TYPE_DEBUG, fmt, ##__VA_ARGS__)
#endif

#ifndef os_log_warning
#define os_log_warning(log, fmt, ...) os_log_with_type((log), OS_LOG_TYPE_DEFAULT, fmt, ##__VA_ARGS__)
#endif

#ifndef os_log_with_type
#define os_log_with_type(log, type, fmt, ...) os_log((log), fmt, ##__VA_ARGS__)
#endif

#ifndef ASFW_DEBUG_BUS_RESET_PACKET
#define ASFW_DEBUG_BUS_RESET_PACKET 0
#endif

#ifndef ASFW_DEBUG_CONFIG_ROM
#define ASFW_DEBUG_CONFIG_ROM 0
#endif

#ifndef ASFW_DEBUG_PHY_INIT
#define ASFW_DEBUG_PHY_INIT 1
#endif

#ifndef ASFW_DEBUG_SELF_ID
#define ASFW_DEBUG_SELF_ID 1
#endif

#ifndef ASFW_DEBUG_TOPOLOGY
#define ASFW_DEBUG_TOPOLOGY 1
#endif

#ifndef ASFW_DEBUG_BUS_RESET
#define ASFW_DEBUG_BUS_RESET 0
#endif

//
// Keep compatibility with your existing Logging.cpp (ASFW::Driver::Logging:*)
// and add header-only rate-limited macros + stable category prefixes.
//

namespace ASFW::Driver::Logging {
os_log_t Controller();  // defined in Logging.cpp you shared
os_log_t Hardware();
os_log_t BusReset();
os_log_t Topology();
os_log_t Metrics();
os_log_t Async();
os_log_t UserClient();
os_log_t Discovery();
os_log_t IRM();
os_log_t BusManager();
os_log_t ConfigROM();
} // namespace ASFW::Driver::Logging

// ----- time helpers (header-only, safe in DriverKit) -----
namespace ASFW::LogDetail {
inline uint64_t NowNs() {
    static mach_timebase_info_data_t tb{0,0};
    if (!tb.denom) mach_timebase_info(&tb);
    const uint64_t t = mach_absolute_time();
    return (t * tb.numer) / tb.denom;
}
struct RlState {
    std::atomic<uint64_t> last_ns{0};
    std::atomic<uint64_t> suppressed{0};
};
} // namespace ASFW::LogDetail

// ----- Plain logging (category-stable prefixes via your cpp) -----
#define ASFW_LOG(cat, fmt, ...) \
    os_log(ASFW::Driver::Logging::cat(), "[%{public}s] " fmt, #cat, ##__VA_ARGS__)

#define ASFW_LOG_TYPE(cat, os_type, fmt, ...) \
    os_log(ASFW::Driver::Logging::cat(), "[%{public}s] " fmt, #cat, ##__VA_ARGS__)

// ----- Rate-limited logging -----
// key: per-callsite stable string (e.g. "tx/ack_tardy"); interval_ms: throttle window
#define ASFW_LOG_RL(cat, key, interval_ms, os_type, fmt, ...)                                  \
    do {                                                                                        \
        static ASFW::LogDetail::RlState _s;                                                     \
        const uint64_t _now = ASFW::LogDetail::NowNs();                                         \
        const uint64_t _intv = (uint64_t)(interval_ms) * 1000000ull;                            \
        uint64_t _last = _s.last_ns.load(std::memory_order_relaxed);                            \
        if (_now - _last >= _intv || _last==0) {                                                \
            if (_s.last_ns.exchange(_now, std::memory_order_relaxed) != 0) {                    \
                uint64_t _lost = _s.suppressed.exchange(0, std::memory_order_relaxed);          \
                if (_lost) {                                                                    \
                    os_log(ASFW::Driver::Logging::cat(),                     \
                        "[%{public}s][%{public}s] (suppressed=%llu prior)", #cat, key, _lost);  \
                }                                                                               \
            }                                                                                   \
            os_log(ASFW::Driver::Logging::cat(),                             \
                "[%{public}s][%{public}s] " fmt, #cat, key, ##__VA_ARGS__);                     \
        } else {                                                                                \
            _s.suppressed.fetch_add(1, std::memory_order_relaxed);                              \
        }                                                                                       \
    } while (0)

// Convenience shorthands
#define ASFW_LOG_INFO(cat, fmt, ...)    ASFW_LOG_TYPE(cat, OS_LOG_TYPE_INFO,    fmt, ##__VA_ARGS__)
#define ASFW_LOG_ERROR(cat, fmt, ...)   ASFW_LOG_TYPE(cat, OS_LOG_TYPE_ERROR,   fmt, ##__VA_ARGS__)
#define ASFW_LOG_DEBUG(cat, fmt, ...)   ASFW_LOG_TYPE(cat, OS_LOG_TYPE_DEFAULT, fmt, ##__VA_ARGS__)
#define ASFW_LOG_FAULT(cat, fmt, ...)   ASFW_LOG_TYPE(cat, OS_LOG_TYPE_FAULT,   fmt, ##__VA_ARGS__)

// ----- Site-aware structured logging (for AT state correlation) -----
// Adds source file/line/function for debugging
#ifndef __FILE_NAME__
#define __FILE_NAME__ __FILE__
#endif

#define ASFW_LOG_SITE(cat, fmt, ...) \
    os_log(ASFW::Driver::Logging::cat(), "[%{public}s] %{public}s:%d %{public}s | " fmt, \
           #cat, __FILE_NAME__, __LINE__, __func__, ##__VA_ARGS__)

// ----- Correlated logging with txid/gen (parseable k=v format) -----
#define ASFW_LOG_KV(cat, ctxName, txid, gen, fmt, ...) \
    ASFW_LOG_SITE(cat, "ctx=%{public}s txid=%u gen=%u " fmt, ctxName, (unsigned)(txid), (unsigned)(gen), ##__VA_ARGS__)

#if ASFW_DEBUG_SELF_ID
#define ASFW_LOG_SELF_ID(fmt, ...) ASFW_LOG_DEBUG(Hardware, fmt, ##__VA_ARGS__)
#else
#define ASFW_LOG_SELF_ID(fmt, ...)
#endif

#if ASFW_DEBUG_TOPOLOGY
#define ASFW_LOG_TOPOLOGY_DETAIL(fmt, ...) ASFW_LOG_DEBUG(Topology, fmt, ##__VA_ARGS__)
#else
#define ASFW_LOG_TOPOLOGY_DETAIL(fmt, ...)
#endif

#if ASFW_DEBUG_BUS_RESET
#define ASFW_LOG_BUSRESET_DETAIL(fmt, ...) ASFW_LOG_DEBUG(BusReset, fmt, ##__VA_ARGS__)
#else
#define ASFW_LOG_BUSRESET_DETAIL(fmt, ...)
#endif

#if ASFW_DEBUG_BUS_RESET_PACKET
#define ASFW_LOG_BUS_RESET_PACKET(fmt, ...) ASFW_LOG_DEBUG(Async, fmt, ##__VA_ARGS__)
#else
#define ASFW_LOG_BUS_RESET_PACKET(fmt, ...)
#endif

#if ASFW_DEBUG_CONFIG_ROM
#define ASFW_LOG_CONFIG_ROM(fmt, ...) ASFW_LOG_DEBUG(ConfigROM, fmt, ##__VA_ARGS__)
#else
#define ASFW_LOG_CONFIG_ROM(fmt, ...)
#endif

#if ASFW_DEBUG_PHY_INIT
#define ASFW_LOG_PHY(fmt, ...) ASFW_LOG_DEBUG(Hardware, fmt, ##__VA_ARGS__)
#else
#define ASFW_LOG_PHY(fmt, ...)
#endif
