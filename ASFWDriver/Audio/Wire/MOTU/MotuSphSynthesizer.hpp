// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once
#include "MotuSph.hpp"
#include <algorithm>
#include <cstdint>

namespace ASFW::Encoding::Motu {
// TX-owned controller. Observation is the first captured SPH, rebased onto
// the actual TX cycle by the existing duplex replay seam. Cadence remains RX
// driven; sample presentation stamps are synthesized with a Q32 accumulator.
// Algorithm informed by PR #172 (858f0758), MotuSphClockServo.hpp:325-483:
// elapsed-frame feed-forward + phase error / elapsed frames (acquisition),
// then quarter gain after lock; strict >4-cycle discontinuity rejection.
// This is a fresh implementation, not a bit-exact vendor controller. The
// +/-2000 ppm clamp is an explicit ASFW policy, not a measured device limit.
class SphSynthesizer final {
public:
    struct State { bool ready{false}; bool locked{false}; bool discontinuity{false}; int64_t stepQ32{0}; int64_t errorTicks{0}; };
    void Configure(uint32_t sampleRateHz) noexcept {
        rate_ = sampleRateHz;
        nominal_ = rate_ ? static_cast<int64_t>((uint64_t{kTicksPerSecond} << 32) / rate_) : 0;
        Reset();
    }
    void Reset() noexcept {
        state_ = {}; state_.stepQ32 = nominal_;
        frames_ = referenceFrames_ = 0;
        phaseQ32_ = referenceTick_ = observedTick_ = 0;
        lastWrappedTick_ = 0;
    }
    [[nodiscard]] State Current() const noexcept { return state_; }
    // Called once for each DATA packet, never for NO-DATA. The first timestamp
    // is a per-frame presentation time, not host arrival time.
    [[nodiscard]] bool Observe(uint32_t wrappedTick) noexcept {
        if (!nominal_ || wrappedTick >= kTicksPerSecond || state_.discontinuity) return false;
        if (!state_.ready) {
            observedTick_ = referenceTick_ = wrappedTick;
            lastWrappedTick_ = wrappedTick;
            phaseQ32_ = uint64_t{wrappedTick} << 32;
            referenceFrames_ = frames_;
            state_.ready = true;
            return true;
        }
        const uint32_t delta = (wrappedTick + kTicksPerSecond - lastWrappedTick_) % kTicksPerSecond;
        // A backward stamp would otherwise masquerade as nearly a second.
        if (delta >= kTicksPerSecond / 2) return Reject();
        observedTick_ += delta;
        lastWrappedTick_ = wrappedTick;
        int64_t errorQ32 = (static_cast<int64_t>(wrappedTick) << 32) - static_cast<int64_t>(phaseQ32_);
        const int64_t periodQ32 = int64_t{kTicksPerSecond} << 32;
        if (errorQ32 > periodQ32 / 2) errorQ32 -= periodQ32;
        if (errorQ32 < -periodQ32 / 2) errorQ32 += periodQ32;
        state_.errorTicks = errorQ32 / (int64_t{1} << 32);
        if (errorQ32 > (int64_t{4 * kTicksPerCycle} << 32) ||
            errorQ32 < -(int64_t{4 * kTicksPerCycle} << 32)) return Reject();
        const uint64_t elapsed = frames_ - referenceFrames_;
        if (elapsed < 512) return true;
        const uint64_t ticks = observedTick_ - referenceTick_;
        const int64_t measured = static_cast<int64_t>((ticks << 32) / elapsed);
        const int64_t tolerance = nominal_ / 500; // 2000 ppm
        if (measured < nominal_ - tolerance || measured > nominal_ + tolerance) return Reject();
        const int64_t gain = state_.locked ? 4 : 1;
        state_.stepQ32 = std::clamp(measured + errorQ32 / static_cast<int64_t>(elapsed) / gain,
            nominal_ - tolerance, nominal_ + tolerance);
        // 2.5us lock threshold, matching PR #172; evaluate full precision.
        state_.locked = errorQ32 < (int64_t{6144} << 32) / 100 &&
                        errorQ32 > -(int64_t{6144} << 32) / 100;
        referenceFrames_ = frames_;
        referenceTick_ = observedTick_;
        return true;
    }
    [[nodiscard]] uint32_t NextSph() noexcept {
        const auto result = SphFromTick(static_cast<uint32_t>(phaseQ32_ >> 32));
        phaseQ32_ = (phaseQ32_ + static_cast<uint64_t>(state_.stepQ32)) % (uint64_t{kTicksPerSecond} << 32);
        ++frames_;
        return result;
    }
private:
    [[nodiscard]] bool Reject() noexcept { state_.ready = false; state_.discontinuity = true; return false; }
    uint32_t rate_{0};
    int64_t nominal_{0};
    State state_{};
    uint64_t frames_{0}, referenceFrames_{0}, phaseQ32_{0}, referenceTick_{0}, observedTick_{0};
    uint32_t lastWrappedTick_{0};
};
} // namespace ASFW::Encoding::Motu
