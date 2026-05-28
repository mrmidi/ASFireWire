#pragma once

// CycleObserver.hpp — cycle-start/lost evidence for RoleCoordinator (FW-7).
//
// Maps OHCI cycle IntEvent bits to a per-generation CycleObservation. Pure and
// host-testable; touches no hardware. It does NOT alter the existing
// cycleInconsistent → DICE audio-recovery path (that stays in
// ControllerCoreInterrupts) — it only records evidence.
//
// OnInterrupt returns true only when the observation actually changed, so the
// caller can feed RoleCoordinator on edges and never on the per-cycle cycleSynch
// flood (cycleSynch fires ~8000×/s when enabled).

#include <cstdint>

#include "RolePolicy.hpp" // CycleObservation

namespace ASFW::Driver::Role {

class CycleObserver {
  public:
    // Record one interrupt's IntEvent bits for the given bus generation. A new
    // generation resets the accumulated evidence. Returns true iff the
    // CycleObservation changed as a result of this call (start or lost newly set).
    bool OnInterrupt(uint32_t generation, uint32_t intEventBits) noexcept;

    [[nodiscard]] CycleObservation Observation() const noexcept { return obs_; }
    [[nodiscard]] uint32_t Generation() const noexcept { return generation_; }
    // cycleInconsistent is recorded for diagnostics but is not part of the
    // start/lost evidence and never drives the changed-edge return value.
    [[nodiscard]] bool CycleInconsistentSeen() const noexcept { return cycleInconsistentSeen_; }

  private:
    uint32_t generation_{0};
    CycleObservation obs_{};
    bool cycleInconsistentSeen_{false};
};

} // namespace ASFW::Driver::Role
