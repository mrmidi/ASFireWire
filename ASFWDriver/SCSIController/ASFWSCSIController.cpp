//
// ASFWSCSIController.cpp
// ASFWDriver
//
// PHASE 0 implementation: a synthetic single-target SCSI HBA with NO FireWire /
// SBP-2 wiring. It answers a hardcoded INQUIRY (so the SCSI Architecture Model
// can build a peripheral nub) and completes everything else GOOD. Purpose: prove
// on a live Tahoe machine that the SAM publishes a SCSITaskUserClient for a
// DriverKit HBA (verify with `ioreg | grep "SCSITaskUserClient GUID"` and by
// VueScan enumerating the phantom device). No hardware required.
//
// Phase 1 replaces the dummy INQUIRY/command handling with real SBP-2 command
// submission via main's Protocols/SBP2/Session (CommandExecutor) and drives
// UserCreateTargetForID from SBP-2 login success. That requires the v14
// SCSI-status/autosense surfacing (see coolscan transport-state notes) so
// fCompletionStatus / fSenseBuffer carry real values.
//

#include <net.mrmidi.ASFW.ASFWDriver/ASFWSCSIController.h>

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/OSDictionary.h>

#include "../Logging/Logging.hpp"

#include <string.h>

// Standard SCSI opcodes we special-case in Phase 0.
namespace {
constexpr uint8_t kOpTestUnitReady = 0x00;
constexpr uint8_t kOpRequestSense  = 0x03;
constexpr uint8_t kOpInquiry       = 0x12;

// Minimal standard INQUIRY data (36 bytes) for the phantom device.
// Peripheral device type 0x06 = scanner (no built-in kernel driver → the SAM
// should offer a SCSITaskUserClient). Clearly labelled as the ASFW phantom so it
// is never mistaken for the real scanner during Phase 0.
constexpr uint8_t kPhantomInquiry[36] = {
    0x06,             // [0] qualifier(0) | device type 0x06 (scanner)
    0x00,             // [1] RMB=0
    0x02,             // [2] SCSI-2
    0x02,             // [3] response data format
    0x1F,             // [4] additional length = 31
    0x00, 0x00, 0x00, // [5..7]
    'A','S','F','W',' ',' ',' ',' ',                         // [8..15]  vendor
    'S','B','P','2',' ','P','H','A','N','T','O','M',' ',' ',' ',' ', // [16..31] product
    '0','.','1',' '   // [32..35] revision
};
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
    return true;
}

void ASFWSCSIController::free()
{
    IOSafeDeleteNULL(ivars, ASFWSCSIController_IVars, 1);
    super::free();
}

kern_return_t IMPL(ASFWSCSIController, Start)
{
    ASFW_LOG(Controller, "[SCSIHBA] Start (phase-0 phantom HBA)");
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        ASFW_LOG(Controller, "[SCSIHBA] super::Start failed: 0x%x", ret);
        return ret;
    }
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWSCSIController, Stop)
{
    ASFW_LOG(Controller, "[SCSIHBA] Stop");
    if (ivars != nullptr && ivars->targetCreated) {
        UserDestroyTargetForID(static_cast<SCSITargetIdentifier>(ivars->targetID));
        ivars->targetCreated = false;
    }
    if (ivars != nullptr) {
        OSSafeReleaseNULL(ivars->targetCreateQueue);
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
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWSCSIController, UserStartController)
{
    // UserCreateTargetForID must NOT be called synchronously from the default
    // dispatch queue: the kernel's follow-up calls (UserInitializeTargetForID, …)
    // are dispatched onto that same queue, so a synchronous call deadlocks
    // controller start (Apple docs, UserCreateTargetForID discussion). Observed
    // on Tahoe as: target device created but never registered, HBA !registered,
    // busy count climbing forever, teardown wedged. Defer to a private queue.
    ASFW_LOG(Controller, "[SCSIHBA] UserStartController — deferring phantom target creation");
    kern_return_t ret = IODispatchQueue::Create("ASFWSCSIController-TargetCreate", 0, 0,
                                                &ivars->targetCreateQueue);
    if (ret != kIOReturnSuccess || ivars->targetCreateQueue == nullptr) {
        ASFW_LOG(Controller, "[SCSIHBA] target-create queue failed: 0x%x", ret);
        return kIOReturnSuccess; // controller still starts; no phantom target
    }
    retain();
    ivars->targetCreateQueue->DispatchAsync(^{
        kern_return_t kr = UserCreateTargetForID(static_cast<SCSIDeviceIdentifier>(0), nullptr);
        if (kr == kIOReturnSuccess) {
            ivars->targetCreated = true;
            ivars->targetID = 0;
        } else {
            ASFW_LOG(Controller, "[SCSIHBA] UserCreateTargetForID(0) failed: 0x%x", kr);
        }
        release();
    });
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
    *count = 1; // SBP-2 probe model: one command in flight
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
    *maxTransferSize = 1u * 1024u * 1024u; // 1 MB per task (generous for scans)
    *alignment = 4;                        // quadlet alignment (1394)
    *numAddressBits = 32;                  // 32-bit DMA
    *segmentType = kDMAOutputSegmentBig32; // IEEE 1394 is big-endian on the wire
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWSCSIController, UserMapHBAData)
{
    // No per-task HBA scratch region in Phase 0.
    (void)uniqueTaskID;
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

    if (opcode == kOpInquiry) {
        // Write the phantom INQUIRY into the task's data buffer.
        IOBufferMemoryDescriptor* buffer = nullptr;
        kern_return_t kr = UserGetDataBuffer(parallelRequest.fTargetID,
                                             parallelRequest.fControllerTaskIdentifier,
                                             &buffer);
        if (kr == kIOReturnSuccess && buffer != nullptr) {
            IOAddressSegment seg{};
            if (buffer->GetAddressRange(&seg) == kIOReturnSuccess && seg.address != 0) {
                uint64_t want = parallelRequest.fRequestedTransferCount;
                uint64_t n = sizeof(kPhantomInquiry);
                if (want != 0 && want < n) { n = want; }
                if (n > seg.length) { n = seg.length; }
                memcpy(reinterpret_cast<void*>(seg.address), kPhantomInquiry, n);
                resp.fBytesTransferred = n;
            }
            // UserGetDataBuffer returns a borrowed reference (framework-owned) —
            // do NOT release it (static analyzer / Get* ownership convention).
        } else {
            ASFW_LOG(Controller, "[SCSIHBA] INQUIRY: UserGetDataBuffer failed 0x%x", kr);
        }
    } else if (opcode == kOpTestUnitReady || opcode == kOpRequestSense) {
        // GOOD, no data (phantom is always "ready", no pending sense).
    } else {
        // Any other command: succeed with no data in Phase 0. (Phase 1 forwards
        // to SBP-2 and reports real status/sense.)
        ASFW_LOG(Controller, "[SCSIHBA] opcode 0x%02x → GOOD (phase-0 stub)", opcode);
    }

    // Fire the completion OSAction (triggers the framework's kernel-side
    // UserCompleteParallelTask). ParallelTaskCompletion is the dext-callable entry.
    ParallelTaskCompletion(completion, resp);

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
