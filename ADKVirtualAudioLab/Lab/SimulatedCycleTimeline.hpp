#pragma once

#include "../Ports/ICycleTimeline.hpp"

#include <cstdint>

namespace ASFW::Lab {

// Synthesizes bus time from audio sample position: at 48 kHz, 6 frames per
// 125 us cycle = 512 ticks per frame (see README, "Time is an explicit
// port"). The lab has no 8 kHz cycle timer and does not pretend to — the
// epoch offset positions runs anywhere in the bus second (e.g. just before a
// 7999->0 wrap), and AdvanceTicks injects adversarial jitter as a test input
// instead of something observed on hardware.
class SimulatedCycleTimeline final : public Ports::ICycleTimeline {
public:
    static constexpr int64_t kTicksPerFrame48k = 512;

    explicit SimulatedCycleTimeline(int64_t epochTicks = 0) noexcept
        : epochTicks_(epochTicks), nowTicks_(epochTicks) {}

    // Bus position tracks the audio frame cursor; monotonic by construction
    // (a smaller frame never rewinds the timeline).
    void AdvanceToFrame(uint64_t absoluteFrame) noexcept {
        const int64_t candidate =
            epochTicks_ + static_cast<int64_t>(absoluteFrame) * kTicksPerFrame48k;
        if (candidate > nowTicks_) {
            nowTicks_ = candidate;
        }
    }

    // Adversarial knob: jitter/skew injected by tests (positive only keeps
    // the monotonic contract; pass small deltas).
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

} // namespace ASFW::Lab
