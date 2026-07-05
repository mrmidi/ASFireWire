//
// ASFWSCSIController.cpp
// ASFWDriver
//
// PHASE 1: synthetic single-target SCSI HBA bridged to the real SBP-2 target.
// SCSI tasks from the SAM (SCSITaskUserClient → VueScan) are handed to
// SBP2TargetBridge (fetched via the process-global SBP2BridgeHub), which runs
// them through the SessionRegistry/CommandExecutor command plane — ORB per
// command, real SCSI status + autosense back in the response.
//
// Until the SBP-2 login is up (or when the FireWire side is down), a phantom
// fallback keeps the SAM's probe alive: hardcoded INQUIRY (spoofing the real
// scanner identity — VueScan matches on the INQUIRY strings), GOOD for
// TUR/REQUEST SENSE, BUSY for everything else so initiators retry.
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

// Standard SCSI opcodes we special-case in Phase 0.
namespace {
constexpr uint8_t kOpTestUnitReady = 0x00;
constexpr uint8_t kOpRequestSense  = 0x03;
constexpr uint8_t kOpInquiry       = 0x12;
constexpr uint8_t kOpReserve6      = 0x16;
constexpr uint8_t kOpRelease6      = 0x17;
constexpr uint8_t kOpReserve10     = 0x56;
constexpr uint8_t kOpRelease10     = 0x57;

// Minimal standard INQUIRY data (36 bytes) for the phantom device.
// Peripheral device type 0x06 = scanner (no built-in kernel driver → the SAM
// should offer a SCSITaskUserClient). Identity spoofs the real CoolScan 9000:
// VueScan (like SANE's coolscan3) matches scanners on the exact vendor/product
// INQUIRY strings, so a neutral "ASFW PHANTOM" identity is invisible to it.
// Strings from the go/no-go probe INQUIRY against the real scanner.
constexpr uint8_t kPhantomInquiry[36] = {
    0x06,             // [0] qualifier(0) | device type 0x06 (scanner)
    0x00,             // [1] RMB=0
    0x02,             // [2] SCSI-2
    0x02,             // [3] response data format
    0x1F,             // [4] additional length = 31
    0x00, 0x00, 0x00, // [5..7]
    'N','i','k','o','n',' ',' ',' ',                         // [8..15]  vendor
    'L','S','-','9','0','0','0',' ','E','D',' ',' ',' ',' ',' ',' ', // [16..31] product
    '1','.','0','2'   // [32..35] revision
};

// Must match UserGetDMASpecification's maxTransferSize: VueScan reads whole
// line groups per READ(10) (~510 KB observed), well under 1 MB.
constexpr uint64_t kMaxTransferPerTask = 1u * 1024u * 1024u;
// SAM timeout 0 = infinite; the SBP-2 ORB timer needs a real bound. Scanner
// mechanics (SCAN, autofocus) run tens of seconds.
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
    ASFW_LOG(Controller, "[SCSIHBA] Start (SBP-2 bridge + phantom fallback)");
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
    ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        ASFW_LOG(Controller, "[SCSIHBA] super::Start failed: 0x%x", ret);
        return ret;
    }

    // Reverse channel from the FireWire side (a separate IOService, unreachable
    // via the provider chain): fires on SBP-2 login up/down. PHASE 1 IS INERT —
    // it only logs; the static phantom target still serves the SCSI probe. Phase
    // 2 will hop onto auxQueue here and call
    // UserCreateTargetForID/UserDestroyTargetForID (login-gated hot-plug).
    SBP2::SBP2BridgeHub::SetTargetObserver([this](uint64_t guid, bool loggedIn) {
        ASFW_LOG(Controller, "[SCSIHBA] SBP-2 target %s guid=0x%016llx (phase-1 inert)",
                 loggedIn ? "up" : "down", guid);
    });
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWSCSIController, Stop)
{
    ASFW_LOG(Controller, "[SCSIHBA] Stop");
    // Drop the reverse-channel observer before teardown so a login event cannot
    // fire into a half-stopped HBA (the observer captures raw `this`).
    SBP2::SBP2BridgeHub::ClearTargetObserver();
    if (ivars != nullptr && ivars->targetCreated) {
        UserDestroyTargetForID(static_cast<SCSITargetIdentifier>(ivars->targetID));
        ivars->targetCreated = false;
    }
    if (ivars != nullptr) {
        OSSafeReleaseNULL(ivars->auxQueue);
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
    // instantly while everything ≤197 B went through. The dext never touches
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
    // No explicit UserCreateTargetForID here: the kernel shim scans target IDs
    // 0..UserReportHighestSupportedDeviceID at bring-up and creates a device for
    // every ID where UserTargetPresentForID returns true. Calling it ourselves
    // produced a DUPLICATE IOSCSIParallelInterfaceDevice for target 0 (two
    // UserInitializeTargetForID + two probe TURs in the same ms), and the orphan
    // wedged teardown permanently (uninterruptible, reboot-only recovery).
    // Explicit creation is for hot-plug targets — Phase 1 uses it from SBP-2
    // login, and must then suppress the presence-scan answer for that ID.
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

    // RESERVE/RELEASE never reach the wire: the working Sequoia stack
    // (VueScan via IOFireWireSBP2Lib) never sent them, and the LS-9000
    // firmware wedges on a RESERVE(6) retry after UNIT ATTENTION — no status
    // block, and the target stays dead until power cycle. The HBA owns the
    // only initiator on this bus, so reservations are trivially GOOD.
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
        // Pre-login / FireWire-down fallback: keep the SAM probe path alive.
        // INQUIRY answers the spoofed identity, TUR/REQUEST SENSE complete GOOD,
        // everything else returns BUSY so the initiator retries after login.
        if (opcode == kOpInquiry) {
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
