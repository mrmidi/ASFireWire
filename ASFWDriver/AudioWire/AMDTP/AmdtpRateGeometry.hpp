#pragma once

#include <cstdint>
#include <optional>

namespace ASFW::Encoding {

struct AmdtpRateGeometry final {
    uint32_t sampleRateHz{0};
    uint32_t nominalFramesPerCycle{0};
    uint32_t sytIntervalFrames{0};
};

[[nodiscard]] constexpr std::optional<AmdtpRateGeometry>
AmdtpRateGeometryForSampleRate(uint32_t sampleRateHz) noexcept {
    switch (sampleRateHz) {
        case 32000:  return AmdtpRateGeometry{32000, 4, 8};
        case 44100:  return AmdtpRateGeometry{44100, 6, 8};
        case 48000:  return AmdtpRateGeometry{48000, 6, 8};
        case 88200:  return AmdtpRateGeometry{88200, 12, 16};
        case 96000:  return AmdtpRateGeometry{96000, 12, 16};
        case 176400: return AmdtpRateGeometry{176400, 24, 32};
        case 192000: return AmdtpRateGeometry{192000, 24, 32};
        default:     return std::nullopt;
    }
}

} // namespace ASFW::Encoding
