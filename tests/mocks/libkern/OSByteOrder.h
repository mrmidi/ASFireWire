// libkern/OSByteOrder.h stub for host testing on Linux
#pragma once

#include <cstdint>

// Linux/POSIX byte order functions
#ifdef __linux__
#include <endian.h>

#define OSSwapBigToHostInt16(x)    be16toh(x)
#define OSSwapBigToHostInt32(x)    be32toh(x)
#define OSSwapBigToHostInt64(x)    be64toh(x)

#define OSSwapHostToBigInt16(x)    htobe16(x)
#define OSSwapHostToBigInt32(x)    htobe32(x)
#define OSSwapHostToBigInt64(x)    htobe64(x)

#define OSSwapLittleToHostInt16(x) le16toh(x)
#define OSSwapLittleToHostInt32(x) le32toh(x)
#define OSSwapLittleToHostInt64(x) le64toh(x)

#define OSSwapHostToLittleInt16(x) htole16(x)
#define OSSwapHostToLittleInt32(x) htole32(x)
#define OSSwapHostToLittleInt64(x) htole64(x)

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
#define OSSwapBigToHostInt16(x)    OSSwapInt16(x)
#define OSSwapBigToHostInt32(x)    OSSwapInt32(x)
#define OSSwapBigToHostInt64(x)    OSSwapInt64(x)

#define OSSwapHostToBigInt16(x)    OSSwapInt16(x)
#define OSSwapHostToBigInt32(x)    OSSwapInt32(x)
#define OSSwapHostToBigInt64(x)    OSSwapInt64(x)

#define OSSwapLittleToHostInt16(x) (x)
#define OSSwapLittleToHostInt32(x) (x)
#define OSSwapLittleToHostInt64(x) (x)

#define OSSwapHostToLittleInt16(x) (x)
#define OSSwapHostToLittleInt32(x) (x)
#define OSSwapHostToLittleInt64(x) (x)

#endif
