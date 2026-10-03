// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "MotuSphClockServo.hpp"

#include <cstdint>

namespace ASFW::Audio::MOTU {

struct MotuSphHardResyncAction final {
    bool muteBeforeRepair{false};
    bool phaseRepairRequired{false};
    bool unmute{false};
    bool muted{false};
};

// Mute latch and hard-resync outlet for MotuSphClockServo. This is not a second
// controller and not a second threshold either: the servo owns the
// lock state and this class only turns it into payload muting and into the
// one-shot phase repair.
//
// Muted means exactly "the loop is not locked", which covers two windows:
// acquisition at stream start (bounded by two update intervals = 22 ms) and a
// discontinuity, where the servo drops the lock itself. Nothing else mutes --
// in particular the seed re-reference that arrives about 12 s into every stream
// does not, because it leaves the lock standing.
class MotuSphHardResyncGate final {
public:
    // Kept as a name because callers and tests refer to it; it is the servo's
    // lock threshold, and there is only one of them.
    static constexpr int64_t kUnmuteThresholdQ32 = MotuSphClockServo::kLockThresholdQ32;

    // `startMuted` is the acquisition window. The caller arms it only for a
    // MOTU V3 stream with a configured servo: a gate that never receives a
    // decision must not be the reason a DICE stream is silent.
    void Reset(bool startMuted = false) noexcept {
        muted_ = startMuted;
        hardResyncLatched_ = false;
    }

    [[nodiscard]] MotuSphHardResyncAction Update(bool hardResyncRequired,
                                                 bool locked) noexcept {
        MotuSphHardResyncAction action{};

        const bool wasMuted = muted_;
        muted_ = !locked;
        action.unmute = wasMuted && !muted_;

        // The repair is armed by the hard-resync edge rather than by the mute
        // edge. The loop also starts muted, so hanging the repair off the mute
        // edge would make the valve
        // unreachable for the whole acquisition window. The caller observes
        // `muted` before executing the repair, so payload muting is still
        // ordered ahead of the phase jump.
        if (hardResyncRequired && !hardResyncLatched_) {
            action.phaseRepairRequired = true;
            action.muteBeforeRepair = muted_ && !wasMuted;
        }
        hardResyncLatched_ = hardResyncRequired;

        action.muted = muted_;
        return action;
    }

    [[nodiscard]] bool IsMuted() const noexcept { return muted_; }

private:
    bool muted_{false};
    bool hardResyncLatched_{false};
};

static_assert(MotuSphHardResyncGate::kUnmuteThresholdQ32 >
              61 * MotuSphClockServo::kOneQ32);
static_assert(MotuSphHardResyncGate::kUnmuteThresholdQ32 <
              62 * MotuSphClockServo::kOneQ32);

} // namespace ASFW::Audio::MOTU
