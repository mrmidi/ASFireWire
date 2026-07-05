#pragma once

// ISessionScheduler — injected one-shot timer abstraction for the SBP-2 session
// components (login/reconnect/logout/busy timeouts, command timeouts).
//
// The session components are plain C++ (POCO) so they can be host-tested under
// ASFW_HOST_TEST; they cannot host an OSAction directly (an OSAction target must
// be an IIG TYPE()-declared method on a DriverKit class). They therefore depend
// on this interface instead of scheduling timers themselves.
//
//   - Production: backed by a single driver-level IOTimerDispatchSource + OSAction
//     owner (the WatchdogCoordinator precedent), wired in FW-58. This avoids the
//     old IOSleep-on-queue pattern, which blocked the dext's single
//     Default queue thread for the whole delay.
//   - Host tests: a deterministic virtual-clock fake (FakeSessionScheduler).
//
// Threading: callbacks fire on the owning dispatch queue (the single Default
// queue), so components need no extra locking beyond their existing generation +
// weak-ownership guards.

#include <cstdint>
#include <functional>

namespace ASFW::Protocols::SBP2 {

// Cancellation token for a scheduled callback. kInvalidSchedulerToken (0) is
// never returned by ScheduleAfter and is always a safe no-op for Cancel().
using SchedulerToken = uint64_t;
inline constexpr SchedulerToken kInvalidSchedulerToken = 0;

class ISessionScheduler {
public:
    virtual ~ISessionScheduler() = default;

    // Schedule `fn` to run once, `delayNs` from now. Returns a token usable with
    // Cancel(). A delay of 0 still defers (the callback never runs inline).
    [[nodiscard]] virtual SchedulerToken ScheduleAfter(uint64_t delayNs,
                                                       std::function<void()> fn) = 0;

    // Cancel a pending callback. No-op if it already fired, was already canceled,
    // or the token is invalid. After Cancel() returns, the callback will not run.
    virtual void Cancel(SchedulerToken token) = 0;
};

} // namespace ASFW::Protocols::SBP2
