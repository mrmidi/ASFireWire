#pragma once

#include <cstdint>

namespace ASFW::Protocols::Audio::IEC61883 {

struct SytTimestamp final {
    uint16_t value{0xFFFF};
    bool valid{false};
};

// SYT field layout (IEC 61883-1): [15:12] cycle count (mod 16),
// [11:0] cycle offset in 24.576 MHz ticks (0..3071).
class SytFormatter final {
public:
    static constexpr uint16_t kNoInfo = 0xFFFF;

    [[nodiscard]] static bool IsNoInfo(uint16_t syt) noexcept;
    [[nodiscard]] static SytTimestamp NoInfo() noexcept;

    [[nodiscard]] static uint16_t EncodeCycleOffset(uint32_t cycle,
                                                    uint32_t offsetTicks) noexcept;
};

} // namespace ASFW::Protocols::Audio::IEC61883
