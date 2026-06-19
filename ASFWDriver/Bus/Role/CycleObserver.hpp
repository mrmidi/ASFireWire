#pragma once

// CycleObserver.hpp — cycle-start/lost evidence for RoleCoordinator (FW-7).
//
// Maps OHCI cycleLost IntEvent bits and the FW-8 bounded no-loss window into a
// per-generation CycleObservation. Pure and host-testable; touches no hardware.
// It does NOT alter the existing cycleInconsistent → DICE audio-recovery path
// (that stays in ControllerCoreInterrupts) — it only records evidence.
//
// cycleSynch is intentionally ignored for role evidence: it fires from the local
// cycle timer and is not proof that a remote root generated cycle-start packets.

#include <cstdint>

#include "RolePolicy.hpp" // CycleObservation

namespace ASFW::Driver::Role {

class CycleObserver {
  public:
    // Record one interrupt's IntEvent bits for the given bus generation. A new
    // generation resets the accumulated evidence. Returns true iff the
    // CycleObservation changed as a result of this call (lost newly set).
    bool OnInterrupt(uint32_t generation, uint32_t intEventBits) noexcept;
    bool MarkCycleContinuityObserved(uint32_t generation) noexcept;

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
