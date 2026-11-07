#pragma once

// TargetLifecycle — the HBA's target-0 create/destroy state, driven by SBP-2
// login edges. Split out of ASFWSCSIController so the edge handling runs on the
// host against fake create/destroy calls; the controller supplies the real
// UserCreateTargetForID / UserDestroyTargetForID through Ops.
//
// Threading: OnEdge runs on the controller's lifecycle queue only (serial), so
// edges never interleave. MarkStopping is called from Stop on the Default
// queue; the flags are atomic because of that one cross-queue writer.

#include <atomic>
#include <cstdint>
#include <functional>
#include <utility>

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOReturn.h>
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

    explicit TargetLifecycle(Ops ops) : ops_(std::move(ops)) {}

    TargetLifecycle(const TargetLifecycle&) = delete;
    TargetLifecycle& operator=(const TargetLifecycle&) = delete;

    // Handle one login edge: create target 0 on up, destroy it on a terminal
    // down. Lifecycle queue only.
    void OnEdge(uint64_t guid, bool loggedIn);

    // Set once at the top of Stop and never cleared: edges still queued skip
    // create/destroy so they cannot race the framework's own child-target
    // termination.
    void MarkStopping() { stopping_ = true; }
    [[nodiscard]] bool IsStopping() const { return stopping_; }
    [[nodiscard]] bool IsTargetAttached() const { return targetAttached_; }

    // True only while createTarget() (UserCreateTargetForID(0)) runs. The SCSI
    // family runs its whole attach probe for the new target inside that call —
    // TEST UNIT READY, INQUIRY, VPD, REPORT LUNS, logical-unit creation — and
    // nothing else can reach target 0 before it returns (no logical unit is
    // published yet). HW 2026-11-06/07 (Leaf Valeo 11, macOS 26.7.1): every
    // probe CDB arrived before the call returned, the last INQUIRY 8 ms before.
    // Read from the HBA task queues.
    [[nodiscard]] bool IsCreatingTarget() const { return creatingTarget_; }

    // The HBA answered one attach-probe TEST UNIT READY locally; counted for
    // the one summary line OnEdge logs after the create returns.
    void NoteAttachProbeTURAnswered() { ++attachProbeTURsAnswered_; }

private:
    Ops ops_;
    // Target 0 exists kernel-side (created at login, destroyed at logout).
    // Written on the lifecycle queue only.
    std::atomic<bool> targetAttached_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> creatingTarget_{false};
    std::atomic<uint32_t> attachProbeTURsAnswered_{0};
};

} // namespace ASFW::Protocols::SBP2
