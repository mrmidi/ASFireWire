#pragma once

// Host-side stub for DriverKit/IOLib.h
// Intended for unit/integration tests outside of DriverKit (macOS or Linux).

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <random>
#include <unordered_map>
#include <cstdio>
#include <cstdarg>
#include <cassert>

#if defined(__APPLE__)
    #include <mach/mach_time.h>
#else
    // Prefer our mock mach_time.h on non-Apple hosts
    #include "mach/mach_time.h"
    inline uint64_t mach_continuous_time() { return mach_absolute_time(); }
#endif

#if defined(__linux__)
    #include <byteswap.h>
#endif

// If you have the DriverKit SDK headers available, it’s nice to reuse IOReturn
// and types. If not, you can replace these includes with your own typedefs.
#include <DriverKit/IOReturn.h>
#include <DriverKit/IOTypes.h>
#include <TargetConditionals.h>

//------------------------------------------------------------------------------
// Byte swap / OSByteOrder-style macros
//------------------------------------------------------------------------------

#ifndef __BYTE_ORDER__
    #if defined(__ORDER_LITTLE_ENDIAN__) && defined(__LITTLE_ENDIAN__)
        #define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
    #elif defined(__ORDER_BIG_ENDIAN__) && defined(__BIG_ENDIAN__)
        #define __BYTE_ORDER__ __ORDER_BIG_ENDIAN__
    #endif
#endif

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__

    #ifndef OSSwapLittleToHostInt16
    #define OSSwapLittleToHostInt16(x) static_cast<uint16_t>(x)
    #endif
    #ifndef OSSwapHostToLittleInt16
    #define OSSwapHostToLittleInt16(x) static_cast<uint16_t>(x)
    #endif
    #ifndef OSSwapLittleToHostInt32
    #define OSSwapLittleToHostInt32(x) static_cast<uint32_t>(x)
    #endif
    #ifndef OSSwapHostToLittleInt32
    #define OSSwapHostToLittleInt32(x) static_cast<uint32_t>(x)
    #endif
    #ifndef OSSwapLittleToHostInt64
    #define OSSwapLittleToHostInt64(x) static_cast<uint64_t>(x)
    #endif
    #ifndef OSSwapHostToLittleInt64
    #define OSSwapHostToLittleInt64(x) static_cast<uint64_t>(x)
    #endif

    #ifndef OSSwapBigToHostInt16
    #define OSSwapBigToHostInt16(x) __builtin_bswap16(static_cast<uint16_t>(x))
    #endif
    #ifndef OSSwapHostToBigInt16
    #define OSSwapHostToBigInt16(x) __builtin_bswap16(static_cast<uint16_t>(x))
    #endif
    #ifndef OSSwapBigToHostInt32
    #define OSSwapBigToHostInt32(x) __builtin_bswap32(static_cast<uint32_t>(x))
    #endif
    #ifndef OSSwapHostToBigInt32
    #define OSSwapHostToBigInt32(x) __builtin_bswap32(static_cast<uint32_t>(x))
    #endif
    #ifndef OSSwapBigToHostInt64
    #define OSSwapBigToHostInt64(x) __builtin_bswap64(static_cast<uint64_t>(x))
    #endif
    #ifndef OSSwapHostToBigInt64
    #define OSSwapHostToBigInt64(x) __builtin_bswap64(static_cast<uint64_t>(x))
    #endif

#else // big-endian host

    #ifndef OSSwapLittleToHostInt16
    #define OSSwapLittleToHostInt16(x) __builtin_bswap16(static_cast<uint16_t>(x))
    #endif
    #ifndef OSSwapHostToLittleInt16
    #define OSSwapHostToLittleInt16(x) __builtin_bswap16(static_cast<uint16_t>(x))
    #endif
    #ifndef OSSwapLittleToHostInt32
    #define OSSwapLittleToHostInt32(x) __builtin_bswap32(static_cast<uint32_t>(x))
    #endif
    #ifndef OSSwapHostToLittleInt32
    #define OSSwapHostToLittleInt32(x) __builtin_bswap32(static_cast<uint32_t>(x))
    #endif
    #ifndef OSSwapLittleToHostInt64
    #define OSSwapLittleToHostInt64(x) __builtin_bswap64(static_cast<uint64_t>(x))
    #endif
    #ifndef OSSwapHostToLittleInt64
    #define OSSwapHostToLittleInt64(x) __builtin_bswap64(static_cast<uint64_t>(x))
    #endif

    #ifndef OSSwapBigToHostInt16
    #define OSSwapBigToHostInt16(x) static_cast<uint16_t>(x)
    #endif
    #ifndef OSSwapHostToBigInt16
    #define OSSwapHostToBigInt16(x) static_cast<uint16_t>(x)
    #endif
    #ifndef OSSwapBigToHostInt32
    #define OSSwapBigToHostInt32(x) static_cast<uint32_t>(x)
    #endif
    #ifndef OSSwapHostToBigInt32
    #define OSSwapHostToBigInt32(x) static_cast<uint32_t>(x)
    #endif
    #ifndef OSSwapBigToHostInt64
    #define OSSwapBigToHostInt64(x) static_cast<uint64_t>(x)
    #endif
    #ifndef OSSwapHostToBigInt64
    #define OSSwapHostToBigInt64(x) static_cast<uint64_t>(x)
    #endif

#endif

//------------------------------------------------------------------------------
// Mach time helpers (portable wrappers for host)
//------------------------------------------------------------------------------

using mach_timebase_info_t = mach_timebase_info_data_t *;

inline int mach_timebase_info_stub(mach_timebase_info_data_t *info) {
    if (!info) {
        return -1;
    }
    return mach_timebase_info(info);
}

inline uint64_t mach_absolute_time_stub() {
    return mach_absolute_time();
}

inline uint64_t mach_continuous_time_stub() {
    // For tests, treat continuous and absolute the same.
    return mach_absolute_time();
}

//------------------------------------------------------------------------------
// IOLock shim for host tests (maps to std::mutex)
//------------------------------------------------------------------------------

struct IOLock {
    std::mutex m;
};

inline IOLock * IOLockAlloc() {
    return new IOLock();
}

inline void IOLockFree(IOLock *lock) {
    delete lock;
}

inline void IOLockLock(IOLock *lock) {
    if (lock) {
        lock->m.lock();
    }
}

inline void IOLockUnlock(IOLock *lock) {
    if (lock) {
        lock->m.unlock();
    }
}

inline bool IOLockTryLock(IOLock *lock) {
    if (!lock) {
        return false;
    }
    return lock->m.try_lock();
}

// A very lightweight assert stub; you can make this stricter if you want.
enum IOLockAssertState {
    kIOLockAssertOwned    = 1,
    kIOLockAssertNotOwned = 2
};

inline void IOLockAssert(IOLock *, IOLockAssertState) {
    // In host tests we don't track ownership; no-op.
}

//------------------------------------------------------------------------------
// IORecursiveLock shim (std::recursive_mutex)
//------------------------------------------------------------------------------

struct IORecursiveLock {
    std::recursive_mutex m;
};

inline IORecursiveLock * IORecursiveLockAlloc(void) {
    return new IORecursiveLock();
}

inline void IORecursiveLockFree(IORecursiveLock *lock) {
    delete lock;
}

inline void IORecursiveLockLock(IORecursiveLock *lock) {
    if (lock) {
        lock->m.lock();
    }
}

inline bool IORecursiveLockTryLock(IORecursiveLock *lock) {
    if (!lock) {
        return false;
    }
    return lock->m.try_lock();
}

inline void IORecursiveLockUnlock(IORecursiveLock *lock) {
    if (lock) {
        lock->m.unlock();
    }
}

inline bool IORecursiveLockHaveLock(IORecursiveLock *) {
    // std::recursive_mutex doesn't expose ownership; assume false.
    return false;
}

//------------------------------------------------------------------------------
// IORWLock shim (std::shared_mutex)
//------------------------------------------------------------------------------

struct IORWLock {
    std::shared_mutex m;
};

inline IORWLock * IORWLockAlloc(void) {
    return new IORWLock();
}

inline void IORWLockFree(IORWLock *lock) {
    delete lock;
}

inline void IORWLockRead(IORWLock *lock) {
    if (lock) {
        lock->m.lock_shared();
    }
}

inline void IORWLockWrite(IORWLock *lock) {
    if (lock) {
        lock->m.lock();
    }
}

inline void IORWLockUnlock(IORWLock *lock) {
    if (!lock) {
        return;
    }
    // In tests we don't distinguish read vs write unlock; this is good enough.
    // We will try to unlock as writer first; if that throws, unlock as reader.
    lock->m.unlock();
}

//------------------------------------------------------------------------------
// Memory allocation: IOMalloc / IOMallocZero / IOFree (+ typed variants)
//------------------------------------------------------------------------------

using malloc_type_id_t = unsigned long long;

inline void * IOMalloc(size_t length) {
    return std::malloc(length);
}

inline void * IOMallocZero(size_t length) {
    void *ptr = std::malloc(length);
    if (ptr) {
        std::memset(ptr, 0, length);
    }
    return ptr;
}

inline void * IOMallocTyped(size_t length, malloc_type_id_t) {
    return IOMalloc(length);
}

inline void * IOMallocZeroTyped(size_t length, malloc_type_id_t) {
    return IOMallocZero(length);
}

inline void IOFree(void *address, size_t) {
    std::free(address);
}

//------------------------------------------------------------------------------
// Sleep / delay
//------------------------------------------------------------------------------

inline void IOSleep(uint64_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

inline void IODelay(uint64_t us) {
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

//------------------------------------------------------------------------------
// Logging: IOLog / IOLogv / IOLogBuffer
//------------------------------------------------------------------------------

inline int IOLogv(const char *format, va_list ap) {
    if (!format) {
        return 0;
    }
    int result = std::vfprintf(stderr, format, ap);
    std::fflush(stderr);
    return result;
}

inline int IOLog(const char *format, ...) {
    if (!format) {
        return 0;
    }
    va_list ap;
    va_start(ap, format);
    int result = IOLogv(format, ap);
    va_end(ap);
    return result;
}

inline void IOLogBuffer(const char *title, const void *buffer, size_t size) {
    const auto *bytes = static_cast<const uint8_t *>(buffer);
    std::fprintf(stderr, "%s (%zu bytes):\n", title ? title : "IOLogBuffer", size);
    for (size_t i = 0; i < size; ++i) {
        if (i % 16 == 0) {
            std::fprintf(stderr, "%04zx: ", i);
        }
        std::fprintf(stderr, "%02x ", bytes[i]);
        if (i % 16 == 15 || i + 1 == size) {
            std::fprintf(stderr, "\n");
        }
    }
    std::fflush(stderr);
}

// If you want logging completely off in some configs, you can redefine IOLog:
// #define IOLog(...) ((void)0)

//------------------------------------------------------------------------------
// CRC32 (simple software implementation, polynomial 0xEDB88320)
//------------------------------------------------------------------------------

inline uint32_t crc32(uint32_t crc, const void *buf, size_t size) {
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) {
            uint32_t mask = (crc & 1u) ? 0xFFFFFFFFu : 0u;
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

//------------------------------------------------------------------------------
// OSSynchronizeIO / panic / OSReportWithBacktrace
//------------------------------------------------------------------------------

inline void OSSynchronizeIO(void) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

inline void OSReportWithBacktrace(const char *str, ...) {
    std::fprintf(stderr, "OSReportWithBacktrace: ");
    if (str) {
        va_list ap;
        va_start(ap, str);
        std::vfprintf(stderr, str, ap);
        va_end(ap);
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    // No real backtrace on host; could integrate with libunwind if you want.
}

inline void panic(const char *string, ...) {
    std::fprintf(stderr, "panic: ");
    if (string) {
        va_list ap;
        va_start(ap, string);
        std::vfprintf(stderr, string, ap);
        va_end(ap);
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    std::abort();
}

//------------------------------------------------------------------------------
// Boot-args parsing (host: always "not found")
//------------------------------------------------------------------------------

inline bool IOParseBootArgNumber(const char *, void *, int) {
    return false;
}

inline bool IOParseBootArgString(const char *, char *, int) {
    return false;
}

//------------------------------------------------------------------------------
// read_random
//------------------------------------------------------------------------------

inline void read_random(void *buffer, size_t numBytes) {
    if (!buffer || numBytes == 0) {
        return;
    }
    std::random_device rd;
    auto *out = static_cast<uint8_t *>(buffer);
    for (size_t i = 0; i < numBytes; ++i) {
        out[i] = static_cast<uint8_t>(rd());
    }
}

//------------------------------------------------------------------------------
// Thread-local storage
//------------------------------------------------------------------------------

inline kern_return_t IOThreadLocalStorageKeyCreate(uint64_t *key) {
    if (!key) {
        return kIOReturnBadArgument;
    }
    static std::atomic<uint64_t> nextKey{1};
    *key = nextKey.fetch_add(1, std::memory_order_relaxed);
    return kIOReturnSuccess;
}

inline kern_return_t IOThreadLocalStorageKeyDelete(uint64_t /*key*/) {
    // For tests, we don't actually reclaim per-key data.
    return kIOReturnSuccess;
}

inline kern_return_t IOThreadLocalStorageSet(uint64_t key, const void *value) {
    thread_local std::unordered_map<uint64_t, const void *> tlsMap;
    tlsMap[key] = value;
    return kIOReturnSuccess;
}

inline void * IOThreadLocalStorageGet(uint64_t key) {
    thread_local std::unordered_map<uint64_t, const void *> tlsMap;
    auto it = tlsMap.find(key);
    if (it == tlsMap.end()) {
        return nullptr;
    }
    return const_cast<void *>(it->second);
}

//------------------------------------------------------------------------------
// IOCallOnce
//------------------------------------------------------------------------------

typedef void (^IOCallOnceBlock)(void);

struct IOCallOnceFlag {
    intptr_t opaque;
};

inline void IOCallOnce(struct IOCallOnceFlag *flag, IOCallOnceBlock block) {
    if (!flag || !block) {
        return;
    }

    struct OnceWrapper {
        std::once_flag once;
    };

    static std::mutex gMutex;
    static auto &mapRef = *new std::unordered_map<IOCallOnceFlag *, OnceWrapper *>();

    OnceWrapper *wrapper = nullptr;
    {
        std::lock_guard<std::mutex> lg(gMutex);
        auto it = mapRef.find(flag);
        if (it == mapRef.end()) {
            wrapper = new OnceWrapper();
            mapRef.emplace(flag, wrapper);
        } else {
            wrapper = it->second;
        }
    }

    std::call_once(wrapper->once, block);
}

//------------------------------------------------------------------------------
// IOVMPageSize
//------------------------------------------------------------------------------

inline uint64_t IOVMPageSize = 4096; // good enough for host tests
