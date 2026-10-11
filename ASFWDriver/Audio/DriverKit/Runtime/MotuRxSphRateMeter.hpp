// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once
#include "MotuPhaseTrace.hpp"
#include "../../Wire/MOTU/MotuSph.hpp"
#include <cstdint>
namespace ASFW::Audio::Runtime {
struct MotuRxSphCumulativeMeasurement final { bool valid{false}; uint64_t frames{0}; int64_t ticks{0}; };
struct MotuRxAbsPhaseWindow final {
    bool valid{false}; uint32_t samples{0}, stored{0};
    int64_t minTicks{0}, medianTicks{0}, maxTicks{0};
};
// RX queue only. Raw SPH differences are measured against RX frame counts.
// Keep bounded absolute-phase statistics used by control; no windowed log model.
class MotuRxSphRateMeter final {
public:
    static constexpr uint32_t kAbsPhaseCapacity = 64;
    void Reset() noexcept { frames_ = 0; ticks_ = 0; rejected_ = 0; Reanchor(); }
    void Reanchor() noexcept { havePrevious_ = false; BeginWindow(); }
    void BeginWindow() noexcept { absPhaseSamples_ = absPhaseStored_ = 0; }
    void ObserveAbsolutePhase(uint32_t sph, uint32_t cycleTimer) noexcept {
        if (!::ASFW::Encoding::Motu::IsWellFormedSph(sph)) return;
        ++absPhaseSamples_;
        if (absPhaseStored_ < kAbsPhaseCapacity)
            absPhase_[absPhaseStored_++] = MotuRxSphMinusCycleTimerTicks(sph, cycleTimer);
    }
    void Observe(uint32_t sph, uint32_t frames, uint32_t rate) noexcept {
        if (!frames || !rate) return; // NO-DATA does not advance either clock.
        if (!::ASFW::Encoding::Motu::IsWellFormedSph(sph)) { ++rejected_; Reanchor(); return; }
        const int64_t ticks = ::ASFW::Encoding::Motu::TickFromSph(sph);
        if (havePrevious_) {
            const int64_t delta = ::ASFW::Encoding::Motu::TickDifference(ticks, previousTicks_);
            const int64_t nominal = int64_t{previousFrames_} * ::ASFW::Encoding::Motu::kTicksPerSecond;
            const int64_t measured = delta * rate;
            const int64_t error = measured > nominal ? measured - nominal : nominal - measured;
            // PR #191's 25% plausibility policy distinguishes gaps/restarts
            // from rate drift. Always re-anchor the next measurement to this packet.
            if (delta > 0 && error * 100 <= nominal * 25) { frames_ += previousFrames_; ticks_ += delta; }
            else ++rejected_;
        }
        previousTicks_ = ticks; previousFrames_ = frames; havePrevious_ = true;
    }
    [[nodiscard]] MotuRxSphCumulativeMeasurement CumulativeSnapshot() const noexcept {
        return {.valid = frames_ != 0, .frames = frames_, .ticks = ticks_};
    }
    [[nodiscard]] uint64_t Rejected() const noexcept { return rejected_; }
    [[nodiscard]] MotuRxAbsPhaseWindow AbsolutePhaseSnapshot() const noexcept {
        MotuRxAbsPhaseWindow out{};
        out.samples = absPhaseSamples_;
        out.stored = absPhaseStored_;
        if (absPhaseStored_ == 0) {
            return out;
        }

        // Insertion sort over at most kAbsPhaseCapacity samples: bounded and
        // allocation-free.
        int64_t sorted[kAbsPhaseCapacity];
        for (uint32_t i = 0; i < absPhaseStored_; ++i) {
            const int64_t value = absPhase_[i];
            uint32_t j = i;
            while (j > 0 && sorted[j - 1] > value) {
                sorted[j] = sorted[j - 1];
                --j;
            }
            sorted[j] = value;
        }

        out.minTicks = sorted[0];
        out.maxTicks = sorted[absPhaseStored_ - 1];
        // The upper middle on an even count: an observed sample, not an
        // average with a half tick the device never presented.
        out.medianTicks = sorted[absPhaseStored_ / 2];
        out.valid = true;
        return out;
    }

private:
    bool havePrevious_{false};
    uint32_t previousFrames_{0};
    int64_t previousTicks_{0};
    uint64_t frames_{0}, rejected_{0};
    int64_t ticks_{0};
    int64_t absPhase_[kAbsPhaseCapacity]{};
    uint32_t absPhaseSamples_{0}, absPhaseStored_{0};
};
} // namespace ASFW::Audio::Runtime
