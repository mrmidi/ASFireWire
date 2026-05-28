#include "CycleObserver.hpp"

#include "../../Hardware/RegisterMap.hpp" // IntEventBits

namespace ASFW::Driver::Role {

bool CycleObserver::OnInterrupt(uint32_t generation, uint32_t intEventBits) noexcept {
    // A new bus generation resets the accumulated evidence — stale cycle
    // evidence must never carry across a reset.
    if (generation != generation_) {
        generation_ = generation;
        obs_ = CycleObservation{};
        cycleInconsistentSeen_ = false;
    }

    bool changed = false;

    // cycleSynch ⇒ cycles are starting on the bus (positive evidence).
    if (((intEventBits & IntEventBits::kCycleSynch) != 0U) && !obs_.cycleStartObserved) {
        obs_.cycleStartObserved = true;
        changed = true;
    }

    // cycleLost ⇒ expected cycle starts are missing.
    if (((intEventBits & IntEventBits::kCycleLost) != 0U) && !obs_.cycleLostObserved) {
        obs_.cycleLostObserved = true;
        changed = true;
    }

    // Recorded for diagnostics only; not part of start/lost evidence and never
    // drives the edge return (the audio-recovery semantics live elsewhere).
    if ((intEventBits & IntEventBits::kCycleInconsistent) != 0U) {
        cycleInconsistentSeen_ = true;
    }

    return changed;
}

} // namespace ASFW::Driver::Role
