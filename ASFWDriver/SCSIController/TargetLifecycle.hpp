#pragma once

// TargetLifecycle — the HBA's target-0 create/destroy state, driven by SBP-2
// login edges. Split out of ASFWSCSIController so the edge handling runs on the
// host against fake create/destroy calls; the controller supplies the real
// UserCreateTargetForID / UserDestroyTargetForID through Ops.
//
// Threading: OnEdge runs on the controller's lifecycle queue only (serial), so
// edges never interleave. MarkStopping is called from Stop on the Default
// queue; the flags are lock-guarded because of that one cross-queue writer.

#include <cstdint>
#include <functional>

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOLib.h>
#endif

namespace ASFW::Protocols::SBP2 {

class TargetLifecycle {
public:
    struct Ops {
        // Re-checked when an up edge executes: the edge may have been queued
        // long before, behind a blocked create, and the session gone since.
        std::function<bool()> sessionReady;
        std::function<kern_return_t()> destroyTarget;
        std::function<kern_return_t()> createTarget;
    };

    explicit TargetLifecycle(Ops ops);
    ~TargetLifecycle();

    TargetLifecycle(const TargetLifecycle&) = delete;
    TargetLifecycle& operator=(const TargetLifecycle&) = delete;

    // Handle one login edge: create target 0 on up, destroy it on a terminal
    // down. Lifecycle queue only.
    void OnEdge(uint64_t guid, bool loggedIn);

    // Set once at the top of Stop and never cleared: edges still queued skip
    // create/destroy so they cannot race the framework's own child-target
    // termination.
    void MarkStopping();

    [[nodiscard]] bool IsStopping() const;
    [[nodiscard]] bool IsTargetAttached() const;

private:
    void SetTargetAttached(bool attached);

    Ops ops_;
    IOLock* lock_{nullptr};
    // Target 0 exists kernel-side (created at login, destroyed at logout).
    // Written on the lifecycle queue only.
    bool targetAttached_{false};
    bool stopping_{false};
};

} // namespace ASFW::Protocols::SBP2
