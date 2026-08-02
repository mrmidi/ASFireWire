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
// The framework auto-creates target 0 and probes it (~3 s) BEFORE the SBP-2
// login completes. Rather than spoof a hardcoded identity, the pre-login probe
// INQUIRY is DEFERRED: its completion is held and replayed with the device's
// real INQUIRY once login is up (generic, no per-device identity). TUR/REQUEST
// SENSE complete GOOD meanwhile; everything else returns BUSY so the initiator
// retries. A held INQUIRY is flushed BUSY on teardown/abort if login never comes.
//

// libc++ <new> must precede DriverKit headers: DriverKit.h forward-declares
// placement new without libc++'s abi_tag, and the reverse order is a hard error.
#include <new>

#include <net.mrmidi.ASFW.ASFWDriver/ASFWSCSIController.h>

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOKitKeys.h>
#include <DriverKit/IOTimerDispatchSource.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>

#include "../Common/TimingUtils.hpp"
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
constexpr uint8_t kOpInquiry       = 0x12;
constexpr uint8_t kOpReserve6      = 0x16;
constexpr uint8_t kOpRelease6      = 0x17;
constexpr uint8_t kOpReserve10     = 0x56;
constexpr uint8_t kOpRelease10     = 0x57;

// One deferred pre-login probe INQUIRY. The framework auto-creates target 0 and
// probes it (~3 s) BEFORE the SBP-2 login completes; INQUIRY must return data
// even when the unit is not ready (SCSI), so instead of a spoof we HOLD the
// probe's completion and replay it against the real device once login is up.
// A single slot suffices — the SAM keeps one probe INQUIRY outstanding; a second
// concurrent one falls back to BUSY.
struct HeldInquiry {
    OSAction* completion;   // owns one retain (taken at hold) until consumed
    uint64_t targetID;
    uint64_t taskID;
    uint64_t dataAddress;   // framework data buffer, valid until the task completes
    uint64_t dataLength;
    uint32_t requestedLength;
    uint32_t timeoutMs;
    uint8_t cdb[16];
    uint8_t cdbLen;
    bool inUse;
};

struct PendingState {
    IOLock* lock;
    HeldInquiry inquiry;
    // Set when a held INQUIRY expired without login. Later pre-login INQUIRYs
    // then answer BUSY immediately instead of re-holding: the SAM retries the
    // probe a few times, and each re-hold would stack another full hold window
    // onto a nub that has been busy since boot — back into the 60 s panic.
    // Cleared on the next login-up notification.
    bool loginWindowExpired;
};

// Must match UserGetDMASpecification's maxTransferSize. Sized as a permissive
// ceiling for any single-LUN SBP-2 scanner, not a per-model value: the LS-9000's
// largest observed READ(10) (VueScan reads whole line groups, ~510 KB) sits well
// under 1 MB, and no scanner in this class is expected to exceed it.
constexpr uint64_t kMaxTransferPerTask = 1u * 1024u * 1024u;
// SAM timeout 0 = infinite; the SBP-2 ORB timer needs a real bound. Sized for
// scanner mechanics in general (SCAN, autofocus run tens of seconds) — not tuned
// to a specific model.
constexpr uint32_t kDefaultTaskTimeoutMs = 60'000;

// Upper bound on how long a pre-login probe INQUIRY may be held. The kernel
// probe path has no timeout of its own and blocks the nub's IOConfigThread;
// the registry busy-timeout panics at 60 s of sustained busy (IOService.cpp:
// 5986). 20 s spans a normal login (~2-5 s) plus bus-reset retries while
// leaving ample headroom before the panic.
constexpr uint64_t kHeldInquiryMaxHoldNs = 20'000'000'000ull;

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

// Store a held INQUIRY into the single slot. Caller has already taken the
// completion + controller retains. Returns false if the slot is occupied (the
// caller then drops the retains and answers BUSY).
bool TryHoldInquiry(PendingState* ps, const HeldInquiry& held) {
    if (ps == nullptr || ps->lock == nullptr) {
        return false;
    }
    IOLockLock(ps->lock);
    const bool slotFree = !ps->inquiry.inUse && !ps->loginWindowExpired;
    if (slotFree) {
        ps->inquiry = held;
    }
    IOLockUnlock(ps->lock);
    return slotFree;
}

void MarkLoginWindowExpired(PendingState* ps) {
    if (ps == nullptr || ps->lock == nullptr) {
        return;
    }
    IOLockLock(ps->lock);
    ps->loginWindowExpired = true;
    IOLockUnlock(ps->lock);
}

void ClearLoginWindowExpired(PendingState* ps) {
    if (ps == nullptr || ps->lock == nullptr) {
        return;
    }
    IOLockLock(ps->lock);
    ps->loginWindowExpired = false;
    IOLockUnlock(ps->lock);
}

// Take ownership of the held INQUIRY (if any). Exactly one caller wins; the
// retains transfer to it.
bool ExtractHeldInquiry(PendingState* ps, HeldInquiry* out) {
    if (ps == nullptr || ps->lock == nullptr) {
        return false;
    }
    IOLockLock(ps->lock);
    const bool had = ps->inquiry.inUse;
    if (had) {
        *out = ps->inquiry;
        ps->inquiry = HeldInquiry{};
    }
    IOLockUnlock(ps->lock);
    return had;
}

// Complete a held INQUIRY with BUSY (login never arrived / teardown / abort) and
// consume the held completion retain. `self` is always live at every call site
// (drain block retain, Stop frame, or abort frame), so it is NOT released here.
void CompleteHeldInquiryBusy(ASFWSCSIController* self, const HeldInquiry& held) {
    SCSIUserParallelResponse resp{};
    resp.version = kScsiUserParallelTaskResponseCurrentVersion1;
    resp.fTargetID = held.targetID;
    resp.fControllerTaskIdentifier = held.taskID;
    resp.fServiceResponse = kSCSIServiceResponse_TASK_COMPLETE;
    resp.fCompletionStatus = kSCSITaskStatus_BUSY;
    self->ParallelTaskCompletion(held.completion, resp);
    held.completion->release();
}

// Replay a held INQUIRY against the real device now that login is up. Consumes
// the held retains via the submit completion (or inline on the not-ready guard).
void SubmitHeldInquiry(ASFWSCSIController* self, const HeldInquiry& held) {
    auto bridge = SBP2::SBP2BridgeHub::Get();
    if (!bridge || !bridge->IsReady()) {
        CompleteHeldInquiryBusy(self, held);
        return;
    }

    SBP2::SCSI::CommandRequest request{};
    request.cdb.assign(held.cdb, held.cdb + held.cdbLen);
    request.direction = SBP2::SCSI::DataDirection::FromTarget;
    request.transferLength = held.requestedLength;
    request.timeoutMs = held.timeoutMs;

    OSAction* completion = held.completion;   // held completion retain, released in the lambda
    const uint64_t targetID = held.targetID;
    const uint64_t taskID = held.taskID;
    const uint64_t dataAddress = held.dataAddress;
    const uint64_t dataLength = held.dataLength;
    const uint32_t requestedLength = held.requestedLength;

    // Keep the controller alive across the async submit gap (local retain paired
    // with the lambda's release).
    self->retain();
    bridge->SubmitTask(std::move(request),
        [self, completion, targetID, taskID, dataAddress, dataLength, requestedLength](
            const SBP2::SCSI::CommandResult& result) {
            SCSIUserParallelResponse resp{};
            resp.version = kScsiUserParallelTaskResponseCurrentVersion1;
            resp.fTargetID = targetID;
            resp.fControllerTaskIdentifier = taskID;
            FillResponseFromResult(resp, result);
            if (resp.fServiceResponse == kSCSIServiceResponse_TASK_COMPLETE &&
                !result.payload.empty() && dataAddress != 0) {
                uint64_t n = std::min<uint64_t>(result.payload.size(), requestedLength);
                n = std::min<uint64_t>(n, dataLength);
                memcpy(reinterpret_cast<void*>(dataAddress), result.payload.data(), n);
                resp.fBytesTransferred = n;
            }
            self->ParallelTaskCompletion(completion, resp);
            completion->release();
            self->release();
        });
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
    ivars->targetCreated = false;
    ivars->targetID = 0;

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
    if (ivars != nullptr && ivars->pendingState != nullptr) {
        // Stop flushes any held INQUIRY before free; if one somehow survived, a
        // live controller retain would have kept us out of free — so only the
        // lock + struct need releasing here.
        auto* ps = static_cast<PendingState*>(ivars->pendingState);
        if (ps->lock != nullptr) {
            IOLockFree(ps->lock);
        }
        IOSafeDeleteNULL(ps, PendingState, 1);
        ivars->pendingState = nullptr;
    }
    if (ivars != nullptr) {
        // Backstop for a Start that failed before Stop could run.
        OSSafeReleaseNULL(ivars->holdTimerAction);
        OSSafeReleaseNULL(ivars->holdTimer);
    }
    IOSafeDeleteNULL(ivars, ASFWSCSIController_IVars, 1);
    super::free();
}

kern_return_t IMPL(ASFWSCSIController, Start)
{
    ASFW_LOG(Controller, "[SCSIHBA] Start (SBP-2 bridge + deferred-INQUIRY probe)");
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
    // The hold-bound timer must exist before the framework can probe target 0
    // (a held INQUIRY without it can wedge the nub's IOConfigThread past the
    // 60 s registry busy timeout). Created on the aux queue so the expiry
    // handler serializes with the login drain and the Stop flush barrier.
    ret = IOTimerDispatchSource::Create(ivars->auxQueue, &ivars->holdTimer);
    if (ret != kIOReturnSuccess || ivars->holdTimer == nullptr) {
        ASFW_LOG(Controller, "[SCSIHBA] hold timer create failed: 0x%x", ret);
        return ret != kIOReturnSuccess ? ret : kIOReturnError;
    }
    ret = CreateActionHeldInquiryTimerFired(0, &ivars->holdTimerAction);
    if (ret != kIOReturnSuccess || ivars->holdTimerAction == nullptr) {
        ASFW_LOG(Controller, "[SCSIHBA] hold timer action create failed: 0x%x", ret);
        return ret != kIOReturnSuccess ? ret : kIOReturnError;
    }
    ret = ivars->holdTimer->SetHandler(ivars->holdTimerAction);
    if (ret != kIOReturnSuccess) {
        ASFW_LOG(Controller, "[SCSIHBA] hold timer SetHandler failed: 0x%x", ret);
        return ret;
    }
    (void)ivars->holdTimer->SetEnableWithCompletion(true, nullptr);

    ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        ASFW_LOG(Controller, "[SCSIHBA] super::Start failed: 0x%x", ret);
        return ret;
    }

    // Reverse channel from the FireWire side (a separate IOService, unreachable
    // via the provider chain): fires on SBP-2 login up/down. On login up, replay
    // any probe INQUIRY held during the pre-login window with the device's real
    // INQUIRY (see the deferred-INQUIRY handling in UserProcessParallelTask).
    //
    // Runs UNDER the hub lock (see SBP2BridgeHub::NotifyTargetState), so it only
    // schedules work: retain self, hop onto auxQueue, drain there, release.
    SBP2::SBP2BridgeHub::SetTargetObserver([this](uint64_t guid, bool loggedIn) {
        (void)guid;
        if (!loggedIn || ivars == nullptr || ivars->auxQueue == nullptr) {
            return;
        }
        this->retain();
        ivars->auxQueue->DispatchAsync(^{
            auto* ps = static_cast<PendingState*>(ivars->pendingState);
            ClearLoginWindowExpired(ps);
            HeldInquiry held{};
            if (ExtractHeldInquiry(ps, &held)) {
                ASFW_LOG(Controller, "[SCSIHBA] replaying held INQUIRY after login");
                SubmitHeldInquiry(this, held);
            }
            this->release();
        });
    });
    return kIOReturnSuccess;
}

void ASFWSCSIController::HeldInquiryTimerFired_Impl(
    ASFWSCSIController_HeldInquiryTimerFired_Args)
{
    (void)action;
    (void)time;
    if (ivars == nullptr) {
        return;
    }
    auto* ps = static_cast<PendingState*>(ivars->pendingState);
    HeldInquiry held{};
    if (ExtractHeldInquiry(ps, &held)) {
        // Login never arrived. Refuse further holds until it does (the SAM's
        // probe retries would otherwise stack fresh hold windows onto a nub
        // that has been busy since boot) and fail the probe with BUSY.
        MarkLoginWindowExpired(ps);
        ASFW_LOG(Controller,
                 "[SCSIHBA] held INQUIRY expired without SBP-2 login → BUSY "
                 "(further pre-login INQUIRYs answer BUSY until login)");
        CompleteHeldInquiryBusy(this, held);
    }
}

kern_return_t IMPL(ASFWSCSIController, Stop)
{
    ASFW_LOG(Controller, "[SCSIHBA] Stop");
    // Drop the observer first. ClearTargetObserver is synchronous with respect to
    // an in-flight login notification (runs under the hub lock), so no new drain
    // blocks are scheduled after it returns.
    SBP2::SBP2BridgeHub::ClearTargetObserver();
    if (ivars != nullptr && ivars->auxQueue != nullptr) {
        // Barrier on auxQueue: runs after any queued drain block, so a held
        // INQUIRY is either already replayed or still ours to flush here (backstop
        // against a leaked OSAction if the SAM tears down without aborting it).
        IODispatchQueue* aux = ivars->auxQueue;
        aux->DispatchSync(^{
            HeldInquiry held{};
            if (ExtractHeldInquiry(static_cast<PendingState*>(ivars->pendingState), &held)) {
                CompleteHeldInquiryBusy(this, held);
            }
        });
        if (ivars->holdTimer != nullptr) {
            // Same ordering rule as InterruptManager::Teardown: the final
            // releases ride in the cancel completion, so the kernel-side free
            // cannot land while a fire is still in flight.
            IOTimerDispatchSource* timer = ivars->holdTimer;
            OSAction* timerAction = ivars->holdTimerAction;
            ivars->holdTimer = nullptr;
            ivars->holdTimerAction = nullptr;
            timer->SetEnableWithCompletion(false, nullptr);
            const kern_return_t ckr = timer->Cancel(^{
                if (timerAction != nullptr) {
                    timerAction->release();
                }
                timer->release();
            });
            if (ckr != kIOReturnSuccess) {
                if (timerAction != nullptr) {
                    timerAction->release();
                }
                timer->release();
            }
        }
        OSSafeReleaseNULL(ivars->auxQueue);
    }
    // No UserDestroyTargetForID: target 0 is framework-auto-created (presence
    // scan, see UserStartController). On HBA teardown the framework terminates it
    // as a child of the stopping controller. An explicit destroy here re-enters
    // that in-flight termination on the aux queue, target 0 never quiesces, and
    // the registry busy-times out at 60s → panic (IOSCSITargetDevice (1,1) +
    // IOThunderboltPort, IOService.cpp:5986). Same auto-target-0 rule as create.
    if (ivars != nullptr) {
        ivars->targetCreated = false;
    }
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
    // No explicit UserCreateTargetForID: the kernel shim scans target IDs
    // 0..UserReportHighestSupportedDeviceID at bring-up and creates a device for
    // every ID where UserTargetPresentForID returns true, so target 0 is auto-
    // created here (HW-confirmed: an explicit create for target 0 fails
    // kIOReturnError because it already exists, and pairing it with a true
    // presence answer spawned a DUPLICATE device that wedged teardown). The
    // pre-login probe of this auto-created target is handled by deferring INQUIRY
    // (see UserProcessParallelTask), not by creating the target on login.
    ASFW_LOG(Controller, "[SCSIHBA] UserStartController — target 0 published via presence scan");
    ivars->targetCreated = true;
    ivars->targetID = 0;
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
    *result = false; // let the SAM manage device objects
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
    *result = (targetID == 0);
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
        // Pre-login window (or Suspend after a bus reset). INQUIRY must return
        // data even when the unit is not ready, so DEFER it: hold the completion
        // and replay it with the device's real INQUIRY once login is up — no
        // spoof. TUR/REQUEST SENSE complete GOOD to keep the probe moving;
        // everything else returns BUSY so the initiator retries.
        if (opcode == kOpInquiry) {
            IOBufferMemoryDescriptor* buffer = nullptr;
            IOAddressSegment seg{};
            const uint8_t cdbLen = parallelRequest.fCommandSize;
            kern_return_t kr = UserGetDataBuffer(parallelRequest.fTargetID,
                                                 parallelRequest.fControllerTaskIdentifier,
                                                 &buffer);
            if (kr == kIOReturnSuccess && buffer != nullptr &&
                buffer->GetAddressRange(&seg) == kIOReturnSuccess && seg.address != 0 &&
                cdbLen > 0 && cdbLen <= sizeof(HeldInquiry{}.cdb)) {
                HeldInquiry held{};
                held.completion = completion;
                held.targetID = parallelRequest.fTargetID;
                held.taskID = parallelRequest.fControllerTaskIdentifier;
                held.dataAddress = seg.address;
                held.dataLength = seg.length;
                held.requestedLength = static_cast<uint32_t>(std::min<uint64_t>(
                    parallelRequest.fRequestedTransferCount, kMaxTransferPerTask));
                held.timeoutMs = parallelRequest.fTimeoutInMilliSec != 0
                                     ? parallelRequest.fTimeoutInMilliSec
                                     : kDefaultTaskTimeoutMs;
                memcpy(held.cdb, parallelRequest.fCommandDescriptorBlock, cdbLen);
                held.cdbLen = cdbLen;
                held.inUse = true;
                // Hold a completion retain for the async gap; consumed on drain
                // (SubmitHeldInquiry) or flush (CompleteHeldInquiryBusy). The
                // borrowed buffer ref stays valid until the task completes — do
                // NOT release it.
                completion->retain();
                if (TryHoldInquiry(static_cast<PendingState*>(ivars->pendingState), held)) {
                    ASFW_LOG(Controller, "[SCSIHBA] INQUIRY deferred until SBP-2 login");
                    // Bound the hold: the kernel probe blocks the nub's
                    // IOConfigThread with no timeout of its own, and the
                    // registry panics at 60 s of sustained busy.
                    if (ivars->holdTimer != nullptr) {
                        (void)ASFW::Timing::initializeHostTimebase();
                        const uint64_t deadline =
                            mach_absolute_time() +
                            ASFW::Timing::nanosToHostTicks(kHeldInquiryMaxHoldNs);
                        (void)ivars->holdTimer->WakeAtTime(kIOTimerClockMachAbsoluteTime,
                                                           deadline, 0);
                    }
                    if (response != nullptr) {
                        *response = kIOReturnSuccess;
                    }
                    return kIOReturnSuccess;  // completion fires on drain/flush/expiry
                }
                // Slot already occupied — undo the retain, fall through to BUSY.
                completion->release();
            } else {
                ASFW_LOG(Controller, "[SCSIHBA] INQUIRY: UserGetDataBuffer failed 0x%x", kr);
            }
            resp.fCompletionStatus = kSCSITaskStatus_BUSY;
        } else if (opcode == kOpTestUnitReady || opcode == kOpRequestSense) {
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
    // If the framework times out / aborts the deferred probe INQUIRY, complete it
    // so the OSAction is not leaked (single target, so match coarsely).
    HeldInquiry held{};
    if (ivars != nullptr &&
        ExtractHeldInquiry(static_cast<PendingState*>(ivars->pendingState), &held)) {
        CompleteHeldInquiryBusy(this, held);
    }
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
