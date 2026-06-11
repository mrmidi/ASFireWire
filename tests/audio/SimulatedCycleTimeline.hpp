#pragma once

#include "../../ASFWDriver/Audio/Ports/ICycleTimeline.hpp"
#include <cstdint>

namespace ASFW::Testing {

class SimulatedCycleTimeline final : public Ports::ICycleTimeline {
public:
    static constexpr int64_t kTicksPerFrame48k = 512;

    explicit SimulatedCycleTimeline(int64_t epochTicks = 0) noexcept
        : epochTicks_(epochTicks), nowTicks_(epochTicks) {}

    void AdvanceToFrame(uint64_t absoluteFrame) noexcept {
        const int64_t candidate =
            epochTicks_ + static_cast<int64_t>(absoluteFrame) * kTicksPerFrame48k;
        if (candidate > nowTicks_) {
            nowTicks_ = candidate;
        }
    }

    void AdvanceTicks(int64_t deltaTicks) noexcept {
        if (deltaTicks > 0) {
            nowTicks_ += deltaTicks;
        }
    }

    void Reset(int64_t epochTicks = 0) noexcept {
        epochTicks_ = epochTicks;
        nowTicks_ = epochTicks;
    }

    int64_t NowTicks() const noexcept override { return nowTicks_; }

private:
    int64_t epochTicks_{0};
    int64_t nowTicks_{0};
};

} // namespace ASFW::Testing
