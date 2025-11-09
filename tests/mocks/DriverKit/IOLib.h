// DriverKit/IOLib.h stub for host testing
#pragma once

#include <cstdint>
// INCLUDE THIS ONLY IF LINUX IS DETECTED
#if defined(__linux__)
    #include <byteswap.h>
#endif

// Byte swap functions
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    #define OSSwapBigToHostInt32(x) __builtin_bswap32(x)
    #define OSSwapHostToBigInt32(x) __builtin_bswap32(x)
    #define OSSwapLittleToHostInt32(x) (x)
    #define OSSwapHostToLittleInt32(x) (x)
#else
    #define OSSwapBigToHostInt32(x) (x)
    #define OSSwapHostToBigInt32(x) (x)
    #define OSSwapLittleToHostInt32(x) __builtin_bswap32(x)
    #define OSSwapHostToLittleInt32(x) __builtin_bswap32(x)
#endif

// Mach time functions (stub - not used in packet tests)
struct mach_timebase_info_data_t {
    uint32_t numer;
    uint32_t denom;
};

inline int mach_timebase_info(mach_timebase_info_data_t* info) {
    if (info) {
        info->numer = 1;
        info->denom = 1;
    }
    return 0;
}

inline uint64_t mach_absolute_time() { return 0; }
inline uint64_t mach_continuous_time() { return 0; }

// IOLog stub
#define IOLog(...) ((void)0)
