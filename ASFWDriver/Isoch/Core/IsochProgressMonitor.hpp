// IsochProgressMonitor.hpp
// Payload-agnostic bounded progress policy for OHCI isoch contexts.

#pragma once

#include <cstdint>

namespace ASFW::Isoch::Core {

struct IsochProgressThresholds final {
    uint64_t wakeAfterTicks{0};
    uint64_t snapshotAfterTicks{0};
    uint64_t fatalAfterTicks{0};

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return wakeAfterTicks > 0 &&
               snapshotAfterTicks >= wakeAfterTicks &&
               fatalAfterTicks >= snapshotAfterTicks;
    }
};

struct IsochProgressObservation final {
    uint64_t nowTicks{0};
    uint64_t retiredCursor{0};
    bool run{false};
    bool active{false};
    bool dead{false};
};

struct IsochProgressDecision final {
    bool progressed{false};
    bool snapshot{false};
    bool requestWake{false};
    bool fatal{false};
    uint64_t stalledTicks{0};
};

// This state machine deliberately knows nothing about packet contents, clock
// domains, or device families. Interrupt delivery is not progress: only a
// monotonically changing retired cursor resets the stall epoch.
class IsochProgressMonitor final {
  public:
    void Configure(IsochProgressThresholds thresholds) noexcept {
        thresholds_ = thresholds;
    }

    void Reset(uint64_t nowTicks, uint64_t retiredCursor) noexcept {
        initialized_ = true;
        lastProgressTicks_ = nowTicks;
        lastRetiredCursor_ = retiredCursor;
        snapshotIssued_ = false;
        wakeIssued_ = false;
        fatalIssued_ = false;
    }

    [[nodiscard]] IsochProgressDecision Observe(
        const IsochProgressObservation& observation) noexcept {
        IsochProgressDecision decision{};

        if (!initialized_ || !thresholds_.IsValid() ||
            observation.nowTicks < lastProgressTicks_) {
            Reset(observation.nowTicks, observation.retiredCursor);
            decision.progressed = true;
            return decision;
        }

        if (observation.retiredCursor != lastRetiredCursor_) {
            Reset(observation.nowTicks, observation.retiredCursor);
            decision.progressed = true;
            return decision;
        }

        decision.stalledTicks = observation.nowTicks - lastProgressTicks_;

        // Linux explicitly wakes a running context after queue publication.
        // ASFW permits one equivalent recovery attempt only when OHCI says the
        // context is idle; WAKE is neither useful nor safe as a repeated loop.
        if (!wakeIssued_ && observation.run && !observation.dead &&
            !observation.active &&
            decision.stalledTicks >= thresholds_.wakeAfterTicks) {
            wakeIssued_ = true;
            decision.requestWake = true;
            if (!snapshotIssued_) {
                snapshotIssued_ = true;
                decision.snapshot = true;
            }
        }

        if (!snapshotIssued_ &&
            decision.stalledTicks >= thresholds_.snapshotAfterTicks) {
            snapshotIssued_ = true;
            decision.snapshot = true;
        }

        if (!fatalIssued_ &&
            decision.stalledTicks >= thresholds_.fatalAfterTicks) {
            fatalIssued_ = true;
            decision.fatal = true;
            if (!snapshotIssued_) {
                snapshotIssued_ = true;
                decision.snapshot = true;
            }
        }

        return decision;
    }

    [[nodiscard]] uint64_t LastRetiredCursor() const noexcept {
        return lastRetiredCursor_;
    }

    [[nodiscard]] uint64_t LastProgressTicks() const noexcept {
        return lastProgressTicks_;
    }

  private:
    IsochProgressThresholds thresholds_{};
    uint64_t lastProgressTicks_{0};
    uint64_t lastRetiredCursor_{0};
    bool initialized_{false};
    bool snapshotIssued_{false};
    bool wakeIssued_{false};
    bool fatalIssued_{false};
};

} // namespace ASFW::Isoch::Core
