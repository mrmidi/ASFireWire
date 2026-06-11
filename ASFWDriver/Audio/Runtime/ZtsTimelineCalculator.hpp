#pragma once

#include "../../Common/TimingUtils.hpp"

#include <cstdint>

namespace ASFW::Audio::Runtime {

class ZtsTimelineCalculator final {
public:
    [[nodiscard]] static constexpr uint64_t AlignZtsStart(uint64_t eventSampleFrame, uint32_t period) noexcept {
        return ((eventSampleFrame + period - 1) / period) * period;
    }

    [[nodiscard]] static uint64_t CalculateTargetHostTicks(uint64_t targetFrame,
                                                           uint64_t eventFrame,
                                                           uint64_t eventHostTicks,
                                                           uint32_t eventHostNanosPerSampleQ8) noexcept {
        const int64_t deltaSamples = static_cast<int64_t>(targetFrame) - static_cast<int64_t>(eventFrame);
        const int64_t deltaNanos = (deltaSamples * static_cast<int64_t>(eventHostNanosPerSampleQ8)) >> 8;
        const uint64_t absNanos = static_cast<uint64_t>(deltaNanos < 0 ? -deltaNanos : deltaNanos);
        const uint64_t ticks = ASFW::Timing::nanosToHostTicks(absNanos);
        const int64_t signedTicks = (deltaNanos < 0) ? -static_cast<int64_t>(ticks) : static_cast<int64_t>(ticks);
        return eventHostTicks + signedTicks;
    }
};

} // namespace ASFW::Audio::Runtime
