// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace ASFW::Audio::Model {
// TA 1999008 10.3.2: wire values are signed 1/256 dB, not linear amplitude.
// The resolution grid is anchored at the reported minimum. Never invent limits.
struct AvcVolumeRange {
    int16_t minimum{}, maximum{}, resolution{};
    [[nodiscard]] bool Valid() const noexcept {
        return minimum != INT16_MIN && maximum != INT16_MAX && minimum < maximum &&
               resolution > 0 && resolution != INT16_MAX &&
               resolution <= static_cast<int32_t>(maximum) - minimum;
    }
    [[nodiscard]] std::optional<int16_t> Quantize(float db) const noexcept {
        if (!Valid() || !std::isfinite(db)) return std::nullopt;
        const double raw = std::clamp(static_cast<double>(db) * 256.0,
                                      static_cast<double>(minimum), static_cast<double>(maximum));
        const auto steps = std::llround((raw - minimum) / resolution);
        const int32_t highestStep = (static_cast<int32_t>(maximum) - minimum) / resolution;
        return static_cast<int16_t>(minimum + std::clamp<int64_t>(steps, 0, highestStep) * resolution);
    }
    [[nodiscard]] static constexpr float Decibels(int16_t raw) noexcept {
        return static_cast<float>(raw) / 256.0f;
    }
};
struct AvcPublishedControl {
    uint32_t token{}, scope{}, element{};
    char name[96]{};
    bool hasMute{}, muted{}, hasVolume{};
    int16_t current{};
    AvcVolumeRange range{};
};
inline constexpr const char* kAvcControlsProperty = "ASFWAvcControls";
inline constexpr uint32_t kMaxAvcControls = 128;
[[nodiscard]] constexpr uint32_t AvcControlToken(uint8_t subunit, uint8_t block, uint8_t channel) noexcept {
    return (uint32_t{subunit} << 16) | (uint32_t{block} << 8) | channel;
}
} // namespace ASFW::Audio::Model
