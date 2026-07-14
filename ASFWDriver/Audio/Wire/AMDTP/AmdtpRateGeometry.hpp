#pragma once

#include <cstdint>
#include <optional>

namespace ASFW::Encoding {

/// Maximum AM824 slots per isochronous data block (CIP DBS).
/// Wire-level container size: PCM audio + MIDI + control slots combined.
inline constexpr uint32_t kMaxAmdtpDbs = 32;

/// Maximum host-facing PCM channel count the driver can handle.
/// Must be <= kMaxAmdtpDbs since PCM channels occupy a subset of DBS slots.
inline constexpr uint32_t kMaxPcmChannels = kMaxAmdtpDbs;

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

struct AmdtpRateGeometry final {
    uint32_t sampleRateHz{0};
    uint32_t nominalFramesPerCycle{0};
    uint32_t sytIntervalFrames{0};
    // AM824 FDF: the IEC 61883-6 SFC sample-rate code (== SampleRate enum value).
    // FFADO AmdtpTransmitStreamProcessor::getFDF() maps the same rates to these.
    uint8_t fdf{0};
};

[[nodiscard]] constexpr std::optional<AmdtpRateGeometry>
AmdtpRateGeometryForSampleRate(uint32_t sampleRateHz) noexcept {
    switch (sampleRateHz) {
        case 32000:  return AmdtpRateGeometry{32000, 4, 8, 0};
        case 44100:  return AmdtpRateGeometry{44100, 6, 8, 1};
        case 48000:  return AmdtpRateGeometry{48000, 6, 8, 2};
        case 88200:  return AmdtpRateGeometry{88200, 12, 16, 3};
        case 96000:  return AmdtpRateGeometry{96000, 12, 16, 4};
        case 176400: return AmdtpRateGeometry{176400, 24, 32, 5};
        case 192000: return AmdtpRateGeometry{192000, 24, 32, 6};
        default:     return std::nullopt;
    }
}

} // namespace ASFW::Encoding
