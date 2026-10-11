// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuSphHardResyncGate.hpp - mute latch and hard-resync outlet for the MOTU
// SPH regulator.
//
// Not a second controller and not a second threshold: MotuSphClockServo owns
// the lock state, and this class only turns it into payload muting and into
// the one-shot phase repair. Muted means exactly "the loop is not locked",
// which covers two windows: acquisition at stream start, and a discontinuity,
// where the servo drops the lock itself. Nothing else mutes -- in particular
// not the seed re-reference, which leaves the lock standing.

#pragma once

#include "MotuSphClockServo.hpp"

#include <cstdint>

namespace ASFW::Encoding::Motu {

struct MotuSphHardResyncAction final {
    bool muteBeforeRepair{false};
    bool phaseRepairRequired{false};
    bool unmute{false};
    bool muted{false};
};

class MotuSphHardResyncGate final {
public:
    // `startMuted` arms the acquisition window. Arm it only where a configured
    // servo will deliver decisions: a latch nobody clears would be permanent
    // silence rather than a short acquisition window.
    // assumed: no audio is published at an unverified presentation phase.
    // linux: FFADO likewise sends silent packets at start until its DLL has
    // synchronised (libstreaming/StreamProcessorManager.cpp, syncStartAll) --
    // there a timer, here the loop's first lock.
    // measured: on an 828mk3 at 48 kHz the acquisition mute lasted 6.07 ms.
    void Reset(bool startMuted = false) noexcept {
        muted_ = startMuted;
        hardResyncLatched_ = false;
    }

    // assumed: the hard-resync action -- mute, then step the transmit phase by
    // the whole error once per threshold crossing while the step returns to
    // the measured rate. Never seen firing on hardware.
    [[nodiscard]] MotuSphHardResyncAction Update(bool hardResyncRequired, bool locked) noexcept {
        MotuSphHardResyncAction action{};

        const bool wasMuted = muted_;
        muted_ = !locked;
        action.unmute = wasMuted && !muted_;

        // The repair is armed by the hard-resync edge, not by the mute edge:
        // the loop also starts muted, so a mute edge would make the valve
        // unreachable for the whole acquisition window. The caller reads
        // `muted` before executing the repair, so muting is still ordered
        // ahead of the phase step.
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

} // namespace ASFW::Encoding::Motu
