//
// IsochTypes.hpp
// ASFWDriver
//
// Core Isochronous Type Definitions
//

#pragma once

#include <cstdint>
#include <TargetConditionals.h> // For __BYTE_ORDER__ checks if needed by system headers? No, compiler defines it.

namespace ASFW::Isoch {

// Endianness helper
inline uint32_t SwapBigToHost(uint32_t x) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap32(x);
#else
    return x;
#endif
}

/// Sample rate family determines timing algorithm
enum class SampleRateFamily : uint8_t {
    k44100,  // 44.1, 88.2, 176.4 kHz - fractional samples/packet
    k48000   // 32, 48, 96, 192 kHz - integer samples/packet
};

/// Sample rate codes per IEC 61883-6
enum class SampleRate : uint8_t {
    k32000   = 0,  // CIP_SFC_32000  (48k family)
    k44100   = 1,  // CIP_SFC_44100  (44.1k family)
    k48000   = 2,  // CIP_SFC_48000  (48k family)
    k88200   = 3,  // CIP_SFC_88200  (44.1k family)
    k96000   = 4,  // CIP_SFC_96000  (48k family)
    k176400  = 5,  // CIP_SFC_176400 (44.1k family)
    k192000  = 6,  // CIP_SFC_192000 (48k family)
    kUnknown = 0xFF
};

/// Get family for a sample rate
[[nodiscard]] constexpr SampleRateFamily GetFamily(SampleRate rate) noexcept {
    switch (rate) {
        case SampleRate::k44100:
        case SampleRate::k88200:
        case SampleRate::k176400:
            return SampleRateFamily::k44100;
        default:
            return SampleRateFamily::k48000;
    }
}

/// SYT intervals per sample rate (from Linux amdtp_syt_intervals)
 constexpr uint8_t kSYTIntervals[] = {
    8,   // 32kHz
    8,   // 44.1kHz
    8,   // 48kHz
    16,  // 88.2kHz
    16,  // 96kHz
    32,  // 176.4kHz
    32,  // 192kHz
};

} // namespace ASFW::Isoch
