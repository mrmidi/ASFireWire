//
// ASFWSCSIController.cpp
// ASFWDriver
//
// Single-target SCSI HBA bridged to the real SBP-2 target. SCSI tasks from the
// SAM (SCSITaskUserClient → VueScan) are handed to SBP2TargetBridge (fetched via
// the process-global SBP2BridgeHub), which runs them through the
// SessionRegistry/CommandExecutor command plane — ORB per command, real SCSI
// status + autosense back in the response.
//
// Target lifecycle follows the SBP-2 session (pure framework hotplug model):
// UserTargetPresentForID answers false UNCONDITIONALLY, so the kernel shim's
// bring-up presence scan never creates a target — a machine booting with no
// SBP-2 device has no target whose probe could strand the registry (the
// pre-fix unconditional true held the boot probe INQUIRY forever: 60 s
// registry busy-timeout panic, IOService.cpp:5986, issue #54). Constant false
// also makes the scan's timing irrelevant: a login-driven create can never
// race the scan into a duplicate target 0 (the HW-observed v49 wedge). All
// creation is explicit: UserCreateTargetForID(0) on the SBP-2 login-up edge,
// UserDestroyTargetForID(0) on the terminal logout/login-failure edge, both on
// a dedicated lifecycle queue. Transient bus-reset suspension emits no edge
// (reconnect re-asserts login), so the target survives a bus reset mid-scan.
//
// The SAM probes the target right after creation, while the session is logged
// in — the probe INQUIRY is forwarded to the device and returns its real
// identity. In the suspended window (bus reset dropped the login; reconnect
// pending) INQUIRY answers BUSY like everything else, so the initiator's
// bounded retries either land after reconnect or fail cleanly — nothing is
// ever held without a deadline. (The previous design deferred pre-login
// INQUIRYs indefinitely; that was the #54 strand and is gone. TUR/REQUEST
// SENSE still answer GOOD to keep probes moving.)
//

// libc++ <new> must precede DriverKit headers: DriverKit.h forward-declares
// placement new without libc++'s abi_tag, and the reverse order is a hard error.
#include <new>

#include <net.mrmidi.ASFW.ASFWDriver/ASFWSCSIController.h>

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOKitKeys.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>

#include "../Logging/Logging.hpp"
#include "../Protocols/SBP2/SCSICommandSet.hpp"
#include "SBP2BridgeHub.hpp"
#include "SBP2TargetBridge.hpp"

#include <algorithm>
#include <string.h>

namespace SBP2 = ASFW::Protocols::SBP2;

// Standard SCSI opcodes we special-case.
namespace {
constexpr uint8_t kOpTestUnitReady = 0x00;
constexpr uint8_t kOpRequestSense  = 0x03;
constexpr uint8_t kOpReserve6      = 0x16;
constexpr uint8_t kOpRelease6      = 0x17;
constexpr uint8_t kOpReserve10     = 0x56;
constexpr uint8_t kOpRelease10     = 0x57;

struct PendingState {
    IOLock* lock;
    // Target 0 exists kernel-side (created at SBP-2 login, destroyed at logout).
    // Create/destroy idempotence guard; written on lifecycleQueue only.
    bool targetAttached;
    // Set (and never cleared) at the top of Stop: lifecycle blocks still queued
    // skip create/destroy so they cannot race the framework's own child-target
    // termination. Stop deliberately does NOT wait for in-flight blocks — a
    // synchronous wait from the Default queue can deadlock against an in-flight
    // UserCreateTargetForID (its target-init upcall is serviced on Default).
    bool stopping;
};

bool IsTargetAttached(PendingState* ps) {
    if (ps == nullptr || ps->lock == nullptr) {
        return false;
    }
    IOLockLock(ps->lock);
    const bool attached = ps->targetAttached;
    IOLockUnlock(ps->lock);
    return attached;
}

void SetTargetAttached(PendingState* ps, bool attached) {
    if (ps == nullptr || ps->lock == nullptr) {
        return;
    }
    IOLockLock(ps->lock);
    ps->targetAttached = attached;
    IOLockUnlock(ps->lock);
}

bool IsStopping(PendingState* ps) {
    if (ps == nullptr || ps->lock == nullptr) {
        return true; // no state → treat as tearing down, do nothing
    }
    IOLockLock(ps->lock);
    const bool stopping = ps->stopping;
    IOLockUnlock(ps->lock);
    return stopping;
}

void SetStopping(PendingState* ps) {
    if (ps == nullptr || ps->lock == nullptr) {
        return;
    }
    IOLockLock(ps->lock);
    ps->stopping = true;
    IOLockUnlock(ps->lock);
}

// Must match UserGetDMASpecification's maxTransferSize. Sized as a permissive
// ceiling for any single-LUN SBP-2 scanner, not a per-model value: the LS-9000's
// largest observed READ(10) (VueScan reads whole line groups, ~510 KB) sits well
// under 1 MB, and no scanner in this class is expected to exceed it.
constexpr uint64_t kMaxTransferPerTask = 1u * 1024u * 1024u;
// SAM timeout 0 = infinite; the SBP-2 ORB timer needs a real bound. Sized for
// scanner mechanics in general (SCAN, autofocus run tens of seconds) — not tuned
// to a specific model.
constexpr uint32_t kDefaultTaskTimeoutMs = 60'000;

// Map an SBP-2 command result onto the SAM response. Synthetic
// kIOReturnNotReady (bridge accepted the task but the session dropped out
// before submission) maps to BUSY so the initiator retries; other transport
// errors are delivery failures.
void FillResponseFromResult(SCSIUserParallelResponse& resp,
                            const ASFW::Protocols::SBP2::SCSI::CommandResult& result)
{
    resp.fServiceResponse = kSCSIServiceResponse_TASK_COMPLETE;
    resp.fCompletionStatus = kSCSITaskStatus_GOOD;
    resp.fBytesTransferred = 0;
    resp.fSenseLength = 0;

    if (result.transportStatus == kIOReturnNotReady) {
        resp.fCompletionStatus = kSCSITaskStatus_BUSY;
        return;
    }
    if (result.transportStatus != 0) {
        resp.fServiceResponse = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
        return;
    }

    if (result.scsiStatusValid) {
        resp.fCompletionStatus = static_cast<SCSITaskStatus>(result.scsiStatus);
        if (result.scsiStatus == kSCSITaskStatus_CHECK_CONDITION &&
            !result.senseData.empty()) {
            const size_t n = std::min<size_t>(result.senseData.size(),
                                              sizeof(resp.fSenseBuffer));
            memcpy(resp.fSenseBuffer, result.senseData.data(), n);
            resp.fSenseLength = static_cast<uint8_t>(
                std::min<size_t>(n, UINT8_MAX));
        }
        return;
    }

    // No command-set status in the status block: SBP-2 sbpStatus 0 (no
    // additional info) on a completed ORB means GOOD; anything else is a
    // transport-level failure.
    if (result.sbpStatus !=
        ASFW::Protocols::SBP2::Wire::SBPStatus::kNoAdditionalInfo) {
        resp.fServiceResponse = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    }
}

// Runs on lifecycleQueue (never auxQueue: UserCreateTargetForID is routed
// through AuxiliaryQueue by the framework, and never the Default queue: it
// services the framework's target-init upcalls). Handles one SBP-2 login edge:
// create target 0 on login-up, destroy it on terminal logout. Caller holds a
// self retain across the call.
//
// Known race, accepted: a block that passed the stopping check can still be
// inside a create/destroy kernel call when the framework begins terminating
// the controller (Stop cannot wait for it — a synchronous wait from the
// Default queue deadlocks against the create's target-init upcall). The
// framework must tolerate hotplug create/destroy racing termination; the call
// then fails and is logged. HW validation covers the unplug paths.
void HandleLoginEdge(ASFWSCSIController* self, PendingState* ps,
                     uint64_t guid, bool loggedIn)
{
    if (IsStopping(ps)) {
        return;
    }

    if (loggedIn) {
        if (IsTargetAttached(ps)) {
            return; // reconnect re-assert or duplicate catch-up — already attached
        }
        // Re-check the session NOW (this block may run long after the edge was
        // queued — a stale Start catch-up must not create a target for a
        // session that has since logged out; the later down-edge found nothing
        // to destroy).
        auto bridge = SBP2::SBP2BridgeHub::Get();
        if (!bridge || !bridge->IsReady()) {
            ASFW_LOG(Controller, "[SCSIHBA] login edge stale (session not ready) — create skipped");
            return;
        }
        OSDictionary* dict = OSDictionary::withCapacity(1);
        if (dict == nullptr) {
            // No create attempt without a properties dict; the next login edge
            // (reconnect re-fires the observer) retries.
            ASFW_LOG(Controller, "[SCSIHBA] target dict alloc failed — create skipped");
            return;
        }
        const kern_return_t kr = self->UserCreateTargetForID(0, dict);
        OSSafeReleaseNULL(dict);
        if (kr == kIOReturnSuccess) {
            SetTargetAttached(ps, true);
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
    if (IsTargetAttached(ps)) {
        const kern_return_t kr = self->UserDestroyTargetForID(0);
        // Clear the flag even on failure: the kernel target is terminating (or
        // already gone) either way, and the next login edge recreates it.
        SetTargetAttached(ps, false);
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
} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool ASFWSCSIController::init()
{
    if (!super::init()) {
        return false;
    }
    ivars = IONewZero(ASFWSCSIController_IVars, 1);
    if (ivars == nullptr) {
        return false;
    }

    // IONewZero → targetAttached=false, stopping=false.
    PendingState* ps = IONewZero(PendingState, 1);
    if (ps == nullptr) {
        return false;
    }
    ps->lock = IOLockAlloc();
    if (ps->lock == nullptr) {
        IOSafeDeleteNULL(ps, PendingState, 1);
        return false;
    }
    ivars->pendingState = ps;
    return true;
}

void ASFWSCSIController::free()
{
    if (ivars != nullptr) {
        // Queues are released here, not in Stop: lifecycle blocks retain the
        // controller, so free() only runs once every queued block has finished
        // — the queues are idle by now. Releasing in Stop instead would race
        // in-flight blocks, and a failed Start (which never gets a Stop) would
        // leak them.
        OSSafeReleaseNULL(ivars->lifecycleQueue);
        OSSafeReleaseNULL(ivars->auxQueue);
        if (ivars->pendingState != nullptr) {
            auto* ps = static_cast<PendingState*>(ivars->pendingState);
            if (ps->lock != nullptr) {
                IOLockFree(ps->lock);
            }
            IOSafeDeleteNULL(ps, PendingState, 1);
            ivars->pendingState = nullptr;
        }
    }
    IOSafeDeleteNULL(ivars, ASFWSCSIController_IVars, 1);
    super::free();
}

kern_return_t IMPL(ASFWSCSIController, Start)
{
    ASFW_LOG(Controller, "[SCSIHBA] Start (SBP-2 bridge, login-driven target)");
    // UserCreateTargetForID is declared QUEUENAME(AuxiliaryQueue) in the SDK .iig
    // ("this call to the framework runs on the Auxiliary queue"), but the framework
    // does not create that queue — the dext must. Without it the call never
    // dispatches: target device gets created kernel-side but the call never
    // returns, controller start wedges (!registered, busy climbing, teardown stuck).
    kern_return_t ret = IODispatchQueue::Create("AuxiliaryQueue", 0, 0, &ivars->auxQueue);
    if (ret != kIOReturnSuccess || ivars->auxQueue == nullptr) {
        ASFW_LOG(Controller, "[SCSIHBA] AuxiliaryQueue create failed: 0x%x", ret);
        return ret;
    }
    ret = SetDispatchQueue("AuxiliaryQueue", ivars->auxQueue);
    if (ret != kIOReturnSuccess) {
        ASFW_LOG(Controller, "[SCSIHBA] SetDispatchQueue(AuxiliaryQueue) failed: 0x%x", ret);
        return ret;
    }
    // Login-edge work (UserCreateTargetForID/UserDestroyTargetForID) runs on its
    // own serial queue: not auxQueue (the create call is routed through it — a
    // call FROM it never dispatches, same wedge as above) and not the Default
    // queue (it services the framework's target-init upcalls during the create).
    ret = IODispatchQueue::Create("ASFWSCSIController-TargetLifecycle", 0, 0,
                                  &ivars->lifecycleQueue);
    if (ret != kIOReturnSuccess || ivars->lifecycleQueue == nullptr) {
        ASFW_LOG(Controller, "[SCSIHBA] lifecycle queue create failed: 0x%x", ret);
        return ret;
    }
    ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        ASFW_LOG(Controller, "[SCSIHBA] super::Start failed: 0x%x", ret);
        return ret;
    }

    // Reverse channel from the FireWire side (a separate IOService, unreachable
    // via the provider chain): fires on SBP-2 login up/down and drives the
    // target lifecycle (create on login, destroy on terminal logout) — see
    // HandleLoginEdge.
    //
    // Runs UNDER the hub lock (see SBP2BridgeHub::NotifyTargetState), so it only
    // schedules work: retain self, hop onto lifecycleQueue, handle there, release.
    PendingState* ps = static_cast<PendingState*>(ivars->pendingState);
    SBP2::SBP2BridgeHub::SetTargetObserver([this, ps](uint64_t guid, bool loggedIn) {
        if (ivars == nullptr || ivars->lifecycleQueue == nullptr) {
            return;
        }
        this->retain();
        ivars->lifecycleQueue->DispatchAsync(^{
            HandleLoginEdge(this, ps, guid, loggedIn);
            this->release();
        });
    });

    // Catch-up: the FireWire side may already be logged in when the HBA starts
    // (HBA service restart while the driver is running) — no further login
    // event will fire, so synthesize the up-edge. Safe against a racing real
    // edge: the lifecycle queue serializes, targetAttached dedupes a double
    // create, and HandleLoginEdge re-checks IsReady() at execution time so a
    // catch-up that lands after a terminal logout creates nothing.
    auto bridge = SBP2::SBP2BridgeHub::Get();
    if (bridge && bridge->IsReady()) {
        this->retain();
        ivars->lifecycleQueue->DispatchAsync(^{
            HandleLoginEdge(this, ps, /*guid*/ 0, /*loggedIn*/ true);
            this->release();
        });
    }
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWSCSIController, Stop)
{
    ASFW_LOG(Controller, "[SCSIHBA] Stop");
    // Gate lifecycle blocks first: anything still queued sees stopping and
    // skips create/destroy, so it cannot race the framework's own child-target
    // termination (see the no-destroy comment below).
    SetStopping(static_cast<PendingState*>(ivars != nullptr ? ivars->pendingState : nullptr));
    // Drop the observer. ClearTargetObserver is synchronous with respect to
    // an in-flight login notification (runs under the hub lock), so no new
    // lifecycle blocks are scheduled after it returns.
    //
    // Deliberately NO synchronous wait on lifecycleQueue here: Stop runs on the
    // Default queue, and an in-flight UserCreateTargetForID cannot return until
    // its target-init upcall is serviced on that same Default queue — a sync
    // wait closes a three-way deadlock (Stop → lifecycleQueue → kernel create →
    // Default) that wedges termination into the 60 s registry busy-timeout
    // panic. Queued blocks are gated by the stopping flag instead; queue
    // objects are released in free(), which cannot run until every block (each
    // holds a controller retain) has finished.
    SBP2::SBP2BridgeHub::ClearTargetObserver();
    // No UserDestroyTargetForID here — not even for a target this HBA created
    // at login: on HBA teardown the framework terminates the target as a child
    // of the stopping controller, and an explicit destroy re-enters that
    // in-flight termination on the aux queue, target 0 never quiesces, and the
    // registry busy-times out at 60s → panic (IOSCSITargetDevice (1,1) +
    // IOThunderboltPort, IOService.cpp:5986). Logout-driven destroys run on the
    // lifecycle queue BEFORE Stop and are gated off by the stopping flag above.
    return Stop(provider, SUPERDISPATCH);
}

// ---------------------------------------------------------------------------
// HBA lifecycle — framework calls these once during bring-up
// ---------------------------------------------------------------------------

kern_return_t IMPL(ASFWSCSIController, UserInitializeController)
{
    ASFW_LOG(Controller, "[SCSIHBA] UserInitializeController");
    // Tag the HBA as a FireWire, external interconnect (mirrors the removed
    // IOFireWireSerialBusProtocolTransport bridge's Protocol Characteristics).
    OSDictionary* props = OSDictionary::withCapacity(2);
    if (props != nullptr) {
        OSString* interconnect = OSString::withCString("FireWire");
        OSString* location = OSString::withCString("External");
        if (interconnect != nullptr) { props->setObject("Physical Interconnect", interconnect); interconnect->release(); }
        if (location != nullptr)     { props->setObject("Physical Interconnect Location", location); location->release(); }
        UserSetHBAProperties(props);
        props->release();
    }

    // All seven keys are required and must be reported before this method
    // returns (IOUserSCSIParallelInterfaceController.h contract). Without this
    // call the kernel shim publishes no segment count/byte-count constraints
    // and rejects any task larger than a small threshold before it reaches the
    // dext: VueScan's SEND LUT (32 KB out) and image READ (~510 KB in) failed
    // instantly while everything ≤197 B went through (LS-9000 traffic, but the
    // constraint applies to any scanner in this class). The dext never touches
    // fBufferIOVMAddr segments (data is copied via UserGetDataBuffer), so the
    // values only need to be permissive and consistent with
    // UserGetDMASpecification (1 MB max transfer, 64-bit, 4-byte alignment).
    OSDictionary* constraints = OSDictionary::withCapacity(7);
    if (constraints == nullptr) {
        return kIOReturnNoMemory;
    }
    struct { const char* key; uint64_t value; } entries[] = {
        { kIOMaximumSegmentCountReadKey,           256 },
        { kIOMaximumSegmentCountWriteKey,          256 },
        { kIOMaximumSegmentByteCountReadKey,       kMaxTransferPerTask },
        { kIOMaximumSegmentByteCountWriteKey,      kMaxTransferPerTask },
        { kIOMinimumSegmentAlignmentByteCountKey,  4 },
        { kIOMaximumSegmentAddressableBitCountKey, 64 },
        // 64-bit addressable, 4-byte minimum alignment (header's mask encoding).
        { kIOMinimumHBADataAlignmentMaskKey,       0xFFFFFFFFFFFFFFFCULL },
    };
    for (const auto& e : entries) {
        OSNumber* num = OSNumber::withNumber(e.value, 64);
        if (num == nullptr) {
            constraints->release();
            return kIOReturnNoMemory;
        }
        constraints->setObject(e.key, num);
        num->release();
    }
    kern_return_t ret = UserReportHBAConstraints(constraints);
    constraints->release();
    if (ret != kIOReturnSuccess) {
        ASFW_LOG(Controller, "[SCSIHBA] UserReportHBAConstraints failed: 0x%x", ret);
        return ret;
    }
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWSCSIController, UserStartController)
{
    // No target yet: the kernel shim scans target IDs
    // 0..UserReportHighestSupportedDeviceID at bring-up and creates a device for
    // every ID where UserTargetPresentForID returns true — which is now false
    // until an SBP-2 login is up, so the scan creates nothing and controller
    // registration completes immediately. A machine booting with no SBP-2
    // device on the bus therefore has no target whose probe could strand the
    // registry (issue #54). Target 0 is created from the login observer
    // (HandleLoginEdge); creating it here with a true presence answer spawned a
    // DUPLICATE device that wedged teardown (HW-observed, v49).
    ASFW_LOG(Controller, "[SCSIHBA] UserStartController — no target until SBP-2 login");
    return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// Capability / identity queries
// ---------------------------------------------------------------------------

kern_return_t IMPL(ASFWSCSIController, UserReportInitiatorIdentifier)
{
    *id = 7; // conventional initiator ID
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWSCSIController, UserReportHighestSupportedDeviceID)
{
    *id = 0; // single target
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWSCSIController, UserReportMaximumTaskCount)
{
    // 8, not 1: with a single-slot pool a task slot that fails to recycle
    // starves every later command silently (observed: TUR completes, INQUIRY
    // never dispatched). SBP-2's one-in-flight constraint is enforced at the
    // session layer in Phase 1, not here.
    *count = 8;
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWSCSIController, UserReportHBAHighestLogicalUnitNumber)
{
    *value = 0;
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWSCSIController, UserDoesHBAPerformAutoSense)
{
    *result = true; // we fill fSenseBuffer on CHECK CONDITION (Phase 1)
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWSCSIController, UserDoesHBASupportMultiPathing)
{
    *result = false;
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWSCSIController, UserDoesHBAPerformDeviceManagement)
{
    // False despite the driver managing targets itself (login-driven
    // create/destroy). The value only gates family-initiated scanning, never
    // the driver's own UserCreateTargetForID/UserDestroyTargetForID — the
    // whole HW-validated lifecycle ran with false. In the legacy kernel
    // family, false is what triggers the auto-create scan; the DriverKit
    // shim's presence scan (neutralized by UserTargetPresentForID == false
    // below) appears to be its analog, but the shim source is not published,
    // so true's exact effect is unverified. Flipping to true might suppress
    // that scan at the root — untested on HW; see the validation plan.
    *result = false;
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWSCSIController, UserDoesHBASupportSCSIParallelFeature)
{
    // We are a synthetic HBA over a serial (FireWire) transport — no real
    // parallel-SCSI negotiation features.
    *result = false;
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWSCSIController, UserTargetPresentForID)
{
    // Constant false — the presence scan must NEVER create a target. Answering
    // true unconditionally made the bring-up scan auto-create target 0 on a
    // device-less boot and strand its probe INQUIRY forever (the 60 s registry
    // busy-timeout boot panic of issue #54); answering true while attached
    // would let a login-driven create that lands BEFORE the scan runs be
    // duplicated BY the scan (the HW-observed v49 duplicate-target wedge — the
    // scan's timing relative to login edges is not ordered). All target
    // creation goes through UserCreateTargetForID on the login edge instead.
    (void)targetID;
    *result = false;
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWSCSIController, UserInitializeTargetForID)
{
    ASFW_LOG(Controller, "[SCSIHBA] UserInitializeTargetForID %llu", (uint64_t)targetID);
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWSCSIController, UserGetDMASpecification)
{
    // This spec drives the kernel shim's IODMACommand for the task buffer it
    // maps before UserProcessParallelTask (fBufferIOVMAddr). Big-endian segment
    // output byte-swaps those segment addresses — with kDMAOutputSegmentBig32
    // the first data-carrying command (INQUIRY) died in kernel DMA prep and
    // never reached the dext ("InitializeDeviceSupport error"). Wire endianness
    // is irrelevant here; SBP-2 data movement uses our own DMA path anyway.
    *maxTransferSize = 1u * 1024u * 1024u; // 1 MB per task (generous for scans)
    *alignment = 4;
    *numAddressBits = 64;
    *segmentType = kDMAOutputSegmentHost64;
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWSCSIController, UserMapHBAData)
{
    // No per-task HBA scratch region in Phase 0, but the unique task ID is
    // mandatory: the kernel uses it to identify the SCSIParallelTask, and an
    // unset ID corrupts task accounting (first command completes, its slot is
    // never freed, and no further commands ever reach the dext).
    *uniqueTaskID = ++ivars->nextUniqueTaskID;
    ASFW_LOG(Controller, "[SCSIHBA] UserMapHBAData → taskID %u", *uniqueTaskID);
    return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// Bundled-task path: opt OUT so the framework uses single-task UserProcessParallelTask.
// ---------------------------------------------------------------------------

kern_return_t IMPL(ASFWSCSIController, UserMapBundledParallelTaskCommandAndResponseBuffers)
{
    (void)parallelCommandIOMemoryDescriptor;
    (void)parallelResponseIOMemoryDescriptor;
    return kIOReturnUnsupported; // → framework uses UserProcessParallelTask
}

void IMPL(ASFWSCSIController, UserProcessBundledParallelTasks)
{
    // Never called (bundled mode declined above).
    (void)parallelRequestSlotIndices;
    (void)parallelRequestSlotIndicesCount;
    (void)completion;
}

// ---------------------------------------------------------------------------
// The core: execute one SCSI task
// ---------------------------------------------------------------------------

kern_return_t IMPL(ASFWSCSIController, UserProcessParallelTask)
{
    const uint8_t opcode = parallelRequest.fCommandDescriptorBlock[0];

    SCSIUserParallelResponse resp{};
    resp.version = kScsiUserParallelTaskResponseCurrentVersion1;
    resp.fTargetID = parallelRequest.fTargetID;
    resp.fControllerTaskIdentifier = parallelRequest.fControllerTaskIdentifier;
    resp.fServiceResponse = kSCSIServiceResponse_TASK_COMPLETE;
    resp.fCompletionStatus = kSCSITaskStatus_GOOD;
    resp.fBytesTransferred = 0;
    resp.fSenseLength = 0;

    // RESERVE/RELEASE never reach the wire. The justification is generic: this
    // HBA owns the only initiator on the bus, so a reservation is uncontended
    // and trivially GOOD for any single-initiator SBP-2 target. (Motivating
    // observation: the working Sequoia stack — VueScan via IOFireWireSBP2Lib —
    // never sent them, and LS-9000 firmware wedges on a RESERVE(6) retry after
    // UNIT ATTENTION: no status block, target dead until power cycle.)
    if (opcode == kOpReserve6 || opcode == kOpRelease6 ||
        opcode == kOpReserve10 || opcode == kOpRelease10) {
        ASFW_LOG(Controller, "[SCSIHBA] opcode 0x%02x (RESERVE/RELEASE) → synthetic GOOD",
                 opcode);
        ParallelTaskCompletion(completion, resp);
        if (response != nullptr) {
            *response = kIOReturnSuccess;
        }
        return kIOReturnSuccess;
    }

    auto bridge = SBP2::SBP2BridgeHub::Get();
    const bool ready = bridge && bridge->IsReady();

    if (!ready) {
        // Suspended window: the target exists (it was created at login) but the
        // session dropped after a bus reset and reconnect is pending. Every
        // data-carrying command — INQUIRY included — answers BUSY so the
        // initiator's bounded retries either land after the reconnect or fail
        // cleanly; nothing is held without a deadline (an indefinitely held
        // INQUIRY was the issue-#54 strand: a device that vanishes while
        // suspended emits no terminal edge, so a held completion would never
        // fire and the task would pin the registry). TUR/REQUEST SENSE complete
        // GOOD to keep probes moving.
        if (opcode == kOpTestUnitReady || opcode == kOpRequestSense) {
            // GOOD, no data.
        } else {
            ASFW_LOG(Controller, "[SCSIHBA] opcode 0x%02x → BUSY (SBP-2 session not ready)",
                     opcode);
            resp.fCompletionStatus = kSCSITaskStatus_BUSY;
        }
        ParallelTaskCompletion(completion, resp);
        if (response != nullptr) {
            *response = kIOReturnSuccess;
        }
        return kIOReturnSuccess;
    }

    // ---- Bridged path: forward the task to the SBP-2 command plane ----

    SBP2::SCSI::CommandRequest request{};
    const uint8_t cdbLength = parallelRequest.fCommandSize;
    if (cdbLength == 0 || cdbLength > sizeof(parallelRequest.fCommandDescriptorBlock)) {
        resp.fServiceResponse = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
        ParallelTaskCompletion(completion, resp);
        if (response != nullptr) {
            *response = kIOReturnSuccess;
        }
        return kIOReturnSuccess;
    }
    request.cdb.assign(parallelRequest.fCommandDescriptorBlock,
                       parallelRequest.fCommandDescriptorBlock + cdbLength);

    const uint32_t transferLength = static_cast<uint32_t>(
        std::min<uint64_t>(parallelRequest.fRequestedTransferCount, kMaxTransferPerTask));
    switch (parallelRequest.fTransferDirection) {
    case kSCSIDataTransfer_FromInitiatorToTarget:
        request.direction = SBP2::SCSI::DataDirection::ToTarget;
        request.transferLength = transferLength;
        break;
    case kSCSIDataTransfer_FromTargetToInitiator:
        request.direction = SBP2::SCSI::DataDirection::FromTarget;
        request.transferLength = transferLength;
        break;
    default:
        request.direction = SBP2::SCSI::DataDirection::None;
        request.transferLength = 0;
        break;
    }
    // SAM task timeout; 0 means "infinite" — clamp to a generous SBP-2 ORB timer.
    request.timeoutMs = parallelRequest.fTimeoutInMilliSec != 0
                            ? parallelRequest.fTimeoutInMilliSec
                            : kDefaultTaskTimeoutMs;

    // Resolve the task's data buffer HERE — the SDK contract is that
    // UserGetDataBuffer is called inside UserProcessParallelTask. The mapping
    // stays valid until the task completes, so the completion lambda may write
    // through the captured address for data-IN.
    IOAddressSegment dataSeg{};
    if (request.transferLength > 0) {
        IOBufferMemoryDescriptor* buffer = nullptr;
        kern_return_t kr = UserGetDataBuffer(parallelRequest.fTargetID,
                                             parallelRequest.fControllerTaskIdentifier,
                                             &buffer);
        if (kr != kIOReturnSuccess || buffer == nullptr ||
            buffer->GetAddressRange(&dataSeg) != kIOReturnSuccess || dataSeg.address == 0 ||
            dataSeg.length < request.transferLength) {
            ASFW_LOG(Controller, "[SCSIHBA] task data buffer unavailable (kr=0x%x len=%llu/%u)",
                     kr, dataSeg.length, request.transferLength);
            resp.fServiceResponse = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
            ParallelTaskCompletion(completion, resp);
            if (response != nullptr) {
                *response = kIOReturnSuccess;
            }
            return kIOReturnSuccess;
        }
        // UserGetDataBuffer returns a borrowed reference — do not release.
        if (request.direction == SBP2::SCSI::DataDirection::ToTarget) {
            const auto* src = reinterpret_cast<const uint8_t*>(dataSeg.address);
            request.outgoingPayload.assign(src, src + request.transferLength);
        }
    }

    // The completion fires later, from the FireWire driver's work queue — keep
    // the OSAction and this service alive across the async gap.
    completion->retain();
    this->retain();

    const SCSITargetIdentifier targetID = parallelRequest.fTargetID;
    const uint64_t taskID = parallelRequest.fControllerTaskIdentifier;
    const bool dataIn = (request.direction == SBP2::SCSI::DataDirection::FromTarget);
    const uint32_t requestedLength = request.transferLength;
    const uint64_t dataAddress = dataSeg.address;
    const uint64_t dataLength = dataSeg.length;

    bridge->SubmitTask(std::move(request),
        [this, completion, targetID, taskID, dataIn, requestedLength, dataAddress,
         dataLength, opcode](const SBP2::SCSI::CommandResult& result) {
            SCSIUserParallelResponse asyncResp{};
            asyncResp.version = kScsiUserParallelTaskResponseCurrentVersion1;
            asyncResp.fTargetID = targetID;
            asyncResp.fControllerTaskIdentifier = taskID;
            FillResponseFromResult(asyncResp, result);

            if (dataIn && asyncResp.fServiceResponse == kSCSIServiceResponse_TASK_COMPLETE &&
                !result.payload.empty() && dataAddress != 0) {
                uint64_t n = std::min<uint64_t>(result.payload.size(), requestedLength);
                n = std::min<uint64_t>(n, dataLength);
                memcpy(reinterpret_cast<void*>(dataAddress), result.payload.data(), n);
                asyncResp.fBytesTransferred = n;
            } else if (!dataIn &&
                       asyncResp.fServiceResponse == kSCSIServiceResponse_TASK_COMPLETE &&
                       asyncResp.fCompletionStatus == kSCSITaskStatus_GOOD) {
                // SBP-2 status carries no residual for data-OUT — report the full
                // transfer on GOOD.
                asyncResp.fBytesTransferred = requestedLength;
            }

            if (asyncResp.fServiceResponse != kSCSIServiceResponse_TASK_COMPLETE ||
                asyncResp.fCompletionStatus != kSCSITaskStatus_GOOD) {
                const auto& sense = result.senseData;
                const uint8_t key = sense.size() > 2 ? (sense[2] & 0x0F) : 0xFF;
                const uint8_t asc = sense.size() > 12 ? sense[12] : 0xFF;
                const uint8_t ascq = sense.size() > 13 ? sense[13] : 0xFF;
                ASFW_LOG(Controller,
                         "[SCSIHBA] task result: opcode=0x%02x svc=%u status=0x%02x "
                         "transport=0x%x sbp=0x%02x sense=%u (key=%x asc=%02x ascq=%02x) "
                         "bytes=%llu",
                         opcode, asyncResp.fServiceResponse, asyncResp.fCompletionStatus,
                         result.transportStatus, result.sbpStatus, asyncResp.fSenseLength,
                         key, asc, ascq, (uint64_t)asyncResp.fBytesTransferred);
            }

            ParallelTaskCompletion(completion, asyncResp);
            completion->release();
            this->release();
        });

    if (response != nullptr) {
        *response = kIOReturnSuccess;
    }
    return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// Task-management functions — Phase 1/2 map these to SBP-2 task management.
// ---------------------------------------------------------------------------

kern_return_t IMPL(ASFWSCSIController, UserAbortTaskRequest)
{
    (void)theT; (void)theL; (void)theQ;
    // Nothing to abort HBA-side: no task is ever held (not-ready answers BUSY
    // synchronously), and bridged tasks complete through the registry's own
    // abort path on teardown.
    if (response != nullptr) { *response = 0; }
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWSCSIController, UserAbortTaskSetRequest)
{
    (void)theT; (void)theL;
    if (response != nullptr) { *response = 0; }
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWSCSIController, UserClearACARequest)
{
    (void)theT; (void)theL;
    if (response != nullptr) { *response = 0; }
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWSCSIController, UserClearTaskSetRequest)
{
    (void)theT; (void)theL;
    if (response != nullptr) { *response = 0; }
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWSCSIController, UserLogicalUnitResetRequest)
{
    (void)theT; (void)theL;
    if (response != nullptr) { *response = 0; }
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWSCSIController, UserTargetResetRequest)
{
    (void)theT;
    if (response != nullptr) { *response = 0; }
    return kIOReturnSuccess;
}
