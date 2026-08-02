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

#include "../../../Scheduling/ITimerScheduler.hpp"

namespace ASFW::Protocols::SBP2 {

// Compatibility aliases while SBP-2 migrates to the protocol-neutral timer
// contract. New control planes include Scheduling/ITimerScheduler.hpp directly.
using SchedulerToken = ASFW::Scheduling::TimerToken;
inline constexpr SchedulerToken kInvalidSchedulerToken = ASFW::Scheduling::kInvalidTimerToken;
using ISessionScheduler = ASFW::Scheduling::ITimerScheduler;

} // namespace ASFW::Protocols::SBP2
