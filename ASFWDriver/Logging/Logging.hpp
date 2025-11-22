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

// ============================================================================
// Runtime Verbosity-Aware Logging Macros
// ============================================================================
//
// These macros check runtime verbosity levels before logging.
// They work with any category (Async, Controller, Hardware, etc.)
//
// Usage:
//   ASFW_LOG_V0(Async, "Critical error");      // Level 0+ (always logs errors)
//   ASFW_LOG_V1(Async, "TX t5 OK");            // Level 1+ (compact summaries)
//   ASFW_LOG_V2(Async, "State transition");   // Level 2+ (key transitions)
//   ASFW_LOG_V3(Async, "Detailed flow");      // Level 3+ (verbose)
//   ASFW_LOG_V4(Async, "Debug dump");         // Level 4+ (full diagnostics)
//   ASFW_LOG_HEX(Async, "Packet: %02x", b);   // Hex dumps (respects flag + level)
//
// Verbosity levels are configured via Info.plist properties:
//   ASFWAsyncVerbosity, ASFWControllerVerbosity, ASFWHardwareVerbosity
//

// Forward declaration (LogConfig defined in LogConfig.hpp)
namespace ASFW {
class LogConfig;
}

// Helper macro to get verbosity for a specific category
// This uses token concatenation to call Get##category##Verbosity()
#define ASFW_GET_VERBOSITY(category) \
    (ASFW::LogConfig::Shared().Get##category##Verbosity())

// Level 0: Critical (errors, failures, timeouts - always logged at this level)
#define ASFW_LOG_V0(category, fmt, ...) \
    do { \
        if (ASFW_GET_VERBOSITY(category) >= 0) { \
            ASFW_LOG(category, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

// Level 1: Compact (one-line summaries, aggregate stats)
#define ASFW_LOG_V1(category, fmt, ...) \
    do { \
        if (ASFW_GET_VERBOSITY(category) >= 1) { \
            ASFW_LOG(category, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

// Level 2: Transitions (key state changes only)
#define ASFW_LOG_V2(category, fmt, ...) \
    do { \
        if (ASFW_GET_VERBOSITY(category) >= 2) { \
            ASFW_LOG(category, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

// Level 3: Verbose (all transitions, detailed flow)
#define ASFW_LOG_V3(category, fmt, ...) \
    do { \
        if (ASFW_GET_VERBOSITY(category) >= 3) { \
            ASFW_LOG(category, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

// Level 4: Debug (hex dumps, buffer dumps, full diagnostics)
#define ASFW_LOG_V4(category, fmt, ...) \
    do { \
        if (ASFW_GET_VERBOSITY(category) >= 4) { \
            ASFW_LOG(category, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

// Hex dumps: respects both explicit flag and verbosity level 4
// This allows forcing hex dumps on/off independent of verbosity level
#define ASFW_LOG_HEX(category, fmt, ...) \
    do { \
        if (ASFW::LogConfig::Shared().IsHexDumpsEnabled() || \
            ASFW_GET_VERBOSITY(category) >= 4) { \
            ASFW_LOG(category, fmt, ##__VA_ARGS__); \
        } \
    } while (0)
