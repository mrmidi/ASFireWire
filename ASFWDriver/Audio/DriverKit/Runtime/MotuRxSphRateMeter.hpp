// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once
#include "../../Wire/MOTU/MotuSph.hpp"
#include <cstdint>
namespace ASFW::Audio::Runtime {
struct MotuRxSphCumulativeMeasurement final {
    bool valid{false};
    uint64_t frames{0};
    int64_t ticks{0};
};
// RX queue only. Raw SPH differences are measured against RX frame counts.
class MotuRxSphRateMeter final {
  public:
    void Reset() noexcept {
        frames_ = 0;
        ticks_ = 0;
        rejected_ = 0;
        Reanchor();
    }
    void Reanchor() noexcept { havePrevious_ = false; }
    void Observe(uint32_t sph, uint32_t frames, uint32_t rate) noexcept {
        if (!frames || !rate)
            return; // NO-DATA does not advance either clock.
        if (!::ASFW::Encoding::Motu::IsWellFormedSph(sph)) {
            ++rejected_;
            Reanchor();
            return;
        }
        const int64_t ticks = ::ASFW::Encoding::Motu::TickFromSph(sph);
        if (havePrevious_) {
            const int64_t delta = ::ASFW::Encoding::Motu::TickDifference(ticks, previousTicks_);
            const int64_t nominal =
                int64_t{previousFrames_} * ::ASFW::Encoding::Motu::kTicksPerSecond;
            const int64_t measured = delta * rate;
            const int64_t error = measured > nominal ? measured - nominal : nominal - measured;
            // PR #191's 25% plausibility policy distinguishes gaps/restarts
            // from rate drift. Always re-anchor the next measurement to this packet.
            if (delta > 0 && error * 100 <= nominal * 25) {
                frames_ += previousFrames_;
                ticks_ += delta;
            } else
                ++rejected_;
        }
        previousTicks_ = ticks;
        previousFrames_ = frames;
        havePrevious_ = true;
    }
    [[nodiscard]] MotuRxSphCumulativeMeasurement CumulativeSnapshot() const noexcept {
        return {.valid = frames_ != 0, .frames = frames_, .ticks = ticks_};
    }
    [[nodiscard]] uint64_t Rejected() const noexcept { return rejected_; }

  private:
    bool havePrevious_{false};
    uint32_t previousFrames_{0};
    int64_t previousTicks_{0};
    uint64_t frames_{0}, rejected_{0};
    int64_t ticks_{0};
};
} // namespace ASFW::Audio::Runtime
