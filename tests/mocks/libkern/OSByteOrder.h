// libkern/OSByteOrder.h stub for host testing on Linux
#pragma once

#include <cstdint>

// Linux/POSIX byte order functions
#ifdef __linux__
#include <endian.h>

#ifndef OSSwapBigToHostInt16
#define OSSwapBigToHostInt16(x)    be16toh(x)
#endif
#ifndef OSSwapBigToHostInt32
#define OSSwapBigToHostInt32(x)    be32toh(x)
#endif
#ifndef OSSwapBigToHostInt64
#define OSSwapBigToHostInt64(x)    be64toh(x)
#endif

#ifndef OSSwapHostToBigInt16
#define OSSwapHostToBigInt16(x)    htobe16(x)
#endif
#ifndef OSSwapHostToBigInt32
#define OSSwapHostToBigInt32(x)    htobe32(x)
#endif
#ifndef OSSwapHostToBigInt64
#define OSSwapHostToBigInt64(x)    htobe64(x)
#endif

#ifndef OSSwapLittleToHostInt16
#define OSSwapLittleToHostInt16(x) le16toh(x)
#endif
#ifndef OSSwapLittleToHostInt32
#define OSSwapLittleToHostInt32(x) le32toh(x)
#endif
#ifndef OSSwapLittleToHostInt64
#define OSSwapLittleToHostInt64(x) le64toh(x)
#endif

#ifndef OSSwapHostToLittleInt16
#define OSSwapHostToLittleInt16(x) htole16(x)
#endif
#ifndef OSSwapHostToLittleInt32
#define OSSwapHostToLittleInt32(x) htole32(x)
#endif
#ifndef OSSwapHostToLittleInt64
#define OSSwapHostToLittleInt64(x) htole64(x)
#endif

#else  // macOS fallback (shouldn't be used in Linux builds, but for completeness)

// Manual byte swapping for non-Linux systems
inline uint16_t OSSwapInt16(uint16_t x) {
    return (x << 8) | (x >> 8);
}

inline uint32_t OSSwapInt32(uint32_t x) {
    return ((x & 0xFF000000u) >> 24) |
           ((x & 0x00FF0000u) >> 8) |
           ((x & 0x0000FF00u) << 8) |
           ((x & 0x000000FFu) << 24);
}

inline uint64_t OSSwapInt64(uint64_t x) {
    return ((x & 0xFF00000000000000ull) >> 56) |
           ((x & 0x00FF000000000000ull) >> 40) |
           ((x & 0x0000FF0000000000ull) >> 24) |
           ((x & 0x000000FF00000000ull) >> 8) |
           ((x & 0x00000000FF000000ull) << 8) |
           ((x & 0x0000000000FF0000ull) << 24) |
           ((x & 0x000000000000FF00ull) << 40) |
           ((x & 0x00000000000000FFull) << 56);
}

// Assume little-endian host for non-Linux builds
#ifndef OSSwapBigToHostInt16
#define OSSwapBigToHostInt16(x)    OSSwapInt16(x)
#endif
#ifndef OSSwapBigToHostInt32
#define OSSwapBigToHostInt32(x)    OSSwapInt32(x)
#endif
#ifndef OSSwapBigToHostInt64
#define OSSwapBigToHostInt64(x)    OSSwapInt64(x)
#endif

#ifndef OSSwapHostToBigInt16
#define OSSwapHostToBigInt16(x)    OSSwapInt16(x)
#endif
#ifndef OSSwapHostToBigInt32
#define OSSwapHostToBigInt32(x)    OSSwapInt32(x)
#endif
#ifndef OSSwapHostToBigInt64
#define OSSwapHostToBigInt64(x)    OSSwapInt64(x)
#endif

#ifndef OSSwapLittleToHostInt16
#define OSSwapLittleToHostInt16(x) (x)
#endif
#ifndef OSSwapLittleToHostInt32
#define OSSwapLittleToHostInt32(x) (x)
#endif
#ifndef OSSwapLittleToHostInt64
#define OSSwapLittleToHostInt64(x) (x)
#endif

#ifndef OSSwapHostToLittleInt16
#define OSSwapHostToLittleInt16(x) (x)
#endif
#ifndef OSSwapHostToLittleInt32
#define OSSwapHostToLittleInt32(x) (x)
#endif
#ifndef OSSwapHostToLittleInt64
#define OSSwapHostToLittleInt64(x) (x)
#endif

#endif
