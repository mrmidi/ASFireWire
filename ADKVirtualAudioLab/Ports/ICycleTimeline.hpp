#pragma once

#include <cstdint>

namespace ASFW::Ports {

// Time as an explicit port (see README, Architecture / Ports).
//
// Production binds this to the recovered bus timeline (ASFW already maintains
// an unwrapped output phase in the 24.576 MHz tick domain); the lab binds
// Lab::SimulatedCycleTimeline, which synthesizes bus time from audio sample
// position (6 frames per 125 us cycle at 48 kHz). Unwrapped by contract —
// consumers do wrap arithmetic themselves (the SYT domain wraps at 16 cycles,
// the bus second at 8000).
class ICycleTimeline {
public:
    virtual ~ICycleTimeline() = default;

    // Monotonically increasing, unwrapped bus time in 24.576 MHz ticks.
    virtual int64_t NowTicks() const noexcept = 0;
};

} // namespace ASFW::Ports
