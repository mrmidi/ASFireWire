// TargetLifecycle — see TargetLifecycle.hpp.

#include "TargetLifecycle.hpp"

#include "../Logging/Logging.hpp"

namespace ASFW::Protocols::SBP2 {

// Known race, accepted: an edge that passed the stopping check can still be
// inside a create/destroy kernel call when the framework begins terminating
// the controller (Stop cannot wait for it — a synchronous wait from the
// Default queue deadlocks against the create's target-init upcall). The
// framework must tolerate hotplug create/destroy racing termination; the call
// then fails and is logged. HW validation covers the unplug paths.
void TargetLifecycle::OnEdge(uint64_t guid, bool loggedIn) {
    if (IsStopping()) {
        return;
    }

    if (loggedIn) {
        if (IsTargetAttached()) {
            // Reconnect re-assert or duplicate catch-up — the login is
            // continuous, keep the target. (A lost session re-login is NOT
            // this case: LoginSession::NotifySessionLost emits a terminal
            // down-edge first, so the destroy leg below has already cleared
            // the flag before the fresh login's up-edge arrives.)
            return;
        }
        // Re-check the session NOW (this edge may run long after it was
        // queued — a stale Start catch-up must not create a target for a
        // session that has since logged out; the later down-edge found nothing
        // to destroy).
        if (!ops_.sessionReady || !ops_.sessionReady()) {
            ASFW_LOG(Controller, "[SCSIHBA] login edge stale (session not ready) — create skipped");
            return;
        }
        // With UserDoesHBAPerformDeviceManagement=false the family runs its own
        // bring-up scan and creates a target-0 node at HBA start, and
        // CreateTargetForID hard-fails on an existing ID
        // (IOSCSIParallelFamily, IOSCSIParallelInterfaceController.cpp:
        // `GetTargetForID != NULL` reject in CreateTargetForID) while
        // DestroyTargetForID on a missing ID is a silent no-op. Clear any
        // pre-existing node — the family's boot node, or a stray from a lost
        // session — so the create below is deterministic and the SAM probe runs
        // against the live login, not the family's boot-time INQUIRY retry
        // (whose window closes ~1 s after start; HW finding 1, 2026-07-29).
        const kern_return_t destroyKr = ops_.destroyTarget();
        if (destroyKr != kIOReturnSuccess) {
            // Expected when no node pre-exists; also the breadcrumb if the
            // DriverKit shim refuses to destroy a family-created node.
            ASFW_LOG(Controller, "[SCSIHBA] pre-create destroy: 0x%x", destroyKr);
        }
        const kern_return_t kr = ops_.createTarget();
        if (kr == kIOReturnSuccess) {
            targetAttached_ = true;
            ASFW_LOG(Controller,
                     "[SCSIHBA] target 0 created (SBP-2 login, guid=0x%016llx)", guid);
        } else {
            // Not retried here: a reconnect or replug re-fires the up edge.
            ASFW_LOG(Controller, "[SCSIHBA] UserCreateTargetForID(0) failed: 0x%x", kr);
        }
        return;
    }

    // Terminal logout or login failure (a transient bus-reset suspension emits
    // no event — reconnect re-asserts login instead). Outstanding bridged tasks
    // complete through the registry's abort path with synthetic failures; the
    // framework handles completions racing a destroyed target (standard
    // hotplug).
    if (IsTargetAttached()) {
        const kern_return_t kr = ops_.destroyTarget();
        // Clear the flag even on failure: the kernel target is terminating (or
        // already gone) either way, and the next login edge recreates it.
        targetAttached_ = false;
        if (kr == kIOReturnSuccess) {
            ASFW_LOG(Controller,
                     "[SCSIHBA] target 0 destroyed (SBP-2 logout, guid=0x%016llx)", guid);
        } else {
            ASFW_LOG(Controller,
                     "[SCSIHBA] UserDestroyTargetForID(0) failed: 0x%x (guid=0x%016llx)",
                     kr, guid);
        }
    }
}

} // namespace ASFW::Protocols::SBP2
