//
// FCPTransport.cpp
// ASFWDriver - AV/C Protocol Layer
//
// FCP (Function Control Protocol) transport layer implementation
//

#include "FCPTransport.hpp"
#include "../../Logging/Logging.hpp"

#include <algorithm>
#include <vector>

using namespace ASFW::Protocols::AVC;

//==============================================================================
// Init / Destruction
//==============================================================================

bool FCPTransport::init(Protocols::Ports::FireWireBusOps* busOps,
                        Protocols::Ports::FireWireBusInfo* busInfo,
                        Discovery::FWDevice* device,
                        Scheduling::ITimerScheduler& timerScheduler,
                        const FCPTransportConfig& config) {
    busOps_ = busOps;
    busInfo_ = busInfo;
    device_ = device;
    timerScheduler_ = &timerScheduler;
    config_ = config;
    shuttingDown_ = false;

    if (!busOps_ || !busInfo_ || !device_) {
        ASFW_LOG_V1(FCP, "FCPTransport: Missing bus port or target device");
        return false;
    }

    // Allocate lock (DriverKit IOLock)
    lock_ = IOLockAlloc();
    if (!lock_) {
        ASFW_LOG_V1(FCP, "FCPTransport: Failed to allocate lock");
        return false;
    }

    ASFW_LOG_V1(FCP,
                "FCPTransport: Initialized for device nodeID=%u, "
                "cmdAddr=0x%llx, rspAddr=0x%llx",
                device_->GetNodeID(), config_.commandAddress,
                config_.responseAddress);
    
    return true;
}

FCPTransport::~FCPTransport() {
    Shutdown();

    // Free lock (cannot throw in DriverKit)
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }

    ASFW_LOG_V1(FCP, "FCPTransport: Destroyed");
}

//==============================================================================
// Command Submission
//==============================================================================

FCPHandle FCPTransport::SubmitCommand(const FCPFrame& command,
                                      FCPCompletion completion) {
    return SubmitCommand(command, std::move(completion), {});
}

FCPHandle FCPTransport::SubmitCommand(const FCPFrame& command,
                                      FCPCompletion completion,
                                      FCPCommandPolicy policy) {
    if (!command.IsValid()) {
        ASFW_LOG_V1(FCP,
                     "FCPTransport: Invalid command size %zu (must be 3-512)",
                     command.length);
        completion(FCPStatus::kInvalidPayload, {});
        return {};
    }

    if (!lock_) {
        completion(FCPStatus::kTransportError, {});
        return {};
    }

    auto cmd = std::make_unique<OutstandingCommand>();
    cmd->command = command;
    cmd->completion = std::move(completion);
    cmd->policy = std::move(policy);

    IOLockLock(lock_);

    if (shuttingDown_) {
        IOLockUnlock(lock_);
        if (cmd->completion) {
            cmd->completion(FCPStatus::kTransportError, {});
        }
        return {};
    }

    // Reserve a non-zero ID before admission. Queued commands are fully
    // cancellable and must never share the active command's old fixed handle.
    cmd->transactionID = ++nextTransactionID_;
    if (cmd->transactionID == 0) {
        cmd->transactionID = ++nextTransactionID_;
    }
    const bool isIdempotent = cmd->policy.retryClass == FCPRetryClass::kIdempotent;
    cmd->retriesLeft = isIdempotent ? config_.maxRetries : 0;
    cmd->allowBusResetRetry = isIdempotent && config_.allowBusResetRetry;
    cmd->gotInterim = false;

    if (pending_ || !queued_.empty()) {
        if (config_.queuePolicy == FCPQueuePolicy::kFifo) {
            const FCPHandle handle{cmd->transactionID};
            queued_.push_back(std::move(cmd));
            const size_t queueDepth = queued_.size();
            IOLockUnlock(lock_);
            ASFW_LOG_V2(FCP, "FCPTransport: Queued command id=%u depth=%zu",
                        handle.transactionID, queueDepth);
            return handle;
        }
        IOLockUnlock(lock_);

        ASFW_LOG_V1(FCP,
                     "FCPTransport: Command already pending");
        if (cmd->completion) {
            cmd->completion(FCPStatus::kBusy, {});
        }
        return {};
    }

    {
        char hexbuf[64] = {0};
        size_t hexlen = std::min(command.length, static_cast<size_t>(16));
        for (size_t i = 0; i < hexlen; i++) {
            snprintf(hexbuf + i*3, 4, "%02x ", command.data[i]);
        }
        ASFW_LOG_HEX(FCP,
                    "FCPTransport: Submitting command: opcode=0x%02x, length=%zu, "
                    "retries=%u, data=[%{public}s]",
                    command.data[2], command.length,
                    cmd->retriesLeft, hexbuf);
    }

    pending_ = std::move(cmd);
    const FCPHandle handle{pending_->transactionID};

    IOLockUnlock(lock_);

    if (!StartPendingWrite()) {
        return {};
    }
    return handle;
}

ASFW::Async::AsyncHandle FCPTransport::SubmitWriteCommand(const FCPFrame& frame,
                                                           FCPWriteAttempt writeAttempt) {
    IOLockLock(lock_);
    if (shuttingDown_ || !busOps_ || !device_) {
        IOLockUnlock(lock_);
        return Async::AsyncHandle{0};
    }

    const FW::Generation gen{writeAttempt.generation};
    const FW::NodeId node{static_cast<uint8_t>(writeAttempt.targetNodeID & 0x3Fu)};
    const Async::FWAddress addr{Async::FWAddress::AddressParts{
        .addressHi = static_cast<uint16_t>((config_.commandAddress >> 32U) & 0xFFFFU),
        .addressLo = static_cast<uint32_t>(config_.commandAddress & 0xFFFFFFFFU),
    }};

    IOLockUnlock(lock_);

    // The async transaction owns this transport until its callback leaves.
    // FCPTransport is ordinary C++ state, not a DriverKit OSObject: using
    // OSObject::retain() on a `new`-allocated instance is invalid.
    const auto self = weak_from_this().lock();
    if (!self) {
        return Async::AsyncHandle{0};
    }
    const auto handle = busOps_->WriteBlock(
        gen, node, addr, frame.Payload(), FW::FwSpeed::S100,
        [self, writeAttempt](Async::AsyncStatus status, std::span<const uint8_t> response) {
            self->OnAsyncWriteComplete(writeAttempt, status, response);
        });
    return handle;
}

bool FCPTransport::StartPendingWrite() {
    if (!lock_) {
        return false;
    }

    IOLockLock(lock_);
    if (shuttingDown_ || !pending_ || !busInfo_ || !device_) {
        IOLockUnlock(lock_);
        return false;
    }

    pending_->asyncHandle = {};
    const FCPWriteAttempt writeAttempt{
        .id = ++nextWriteAttempt_,
        .targetNodeID = device_->GetNodeID(),
        .generation = static_cast<uint32_t>(busInfo_->GetGeneration().value),
    };
    pending_->activeWriteAttempt = writeAttempt;
    pending_->successfulWriteAttempt.reset();
    const uint32_t transactionID = pending_->transactionID;
    const FCPFrame commandCopy = pending_->command;
    IOLockUnlock(lock_);

    ASFW_LOG_V2(FCP,
                "FCPTransport: Issuing write attempt=%llu node=0x%04x generation=%u",
                writeAttempt.id, writeAttempt.targetNodeID, writeAttempt.generation);
    const auto handle = SubmitWriteCommand(commandCopy, writeAttempt);
    if (!handle.value) {
        ASFW_LOG_V1(FCP, "FCPTransport: Failed to submit async write");
        CompleteCommand(FCPStatus::kTransportError, {});
        return false;
    }

    IOLockLock(lock_);
    const bool stillCurrent = !shuttingDown_ && pending_ &&
                              pending_->transactionID == transactionID &&
                              pending_->activeWriteAttempt.has_value() &&
                              pending_->activeWriteAttempt->id == writeAttempt.id;
    if (stillCurrent) {
        pending_->asyncHandle = handle;
    }
    IOLockUnlock(lock_);

    if (!stillCurrent && busOps_) {
        busOps_->Cancel(handle);
    }
    return stillCurrent;
}

void FCPTransport::StartNextQueuedCommand() {
    if (!lock_) {
        return;
    }

    IOLockLock(lock_);
    if (shuttingDown_ || pending_ || queued_.empty()) {
        IOLockUnlock(lock_);
        return;
    }
    pending_ = std::move(queued_.front());
    queued_.pop_front();
    IOLockUnlock(lock_);

    (void)StartPendingWrite();
}

void FCPTransport::Shutdown() {
    if (!lock_) {
        shuttingDown_ = true;
        return;
    }

    Async::AsyncHandle handle{};
    std::vector<FCPCompletion> completions;

    IOLockLock(lock_);
    if (shuttingDown_) {
        IOLockUnlock(lock_);
        return;
    }

    shuttingDown_ = true;
    if (pending_) {
        handle = pending_->asyncHandle;
        completions.push_back(std::move(pending_->completion));
        CancelTimeout();
        pending_.reset();
    }
    while (!queued_.empty()) {
        completions.push_back(std::move(queued_.front()->completion));
        queued_.pop_front();
    }
    IOLockUnlock(lock_);

    if (handle.value && busOps_) {
        busOps_->Cancel(handle);
    }
    for (auto& completion : completions) {
        if (completion) {
            completion(FCPStatus::kTransportError, {});
        }
    }
}

//==============================================================================
// Command Cancellation
//==============================================================================

bool FCPTransport::CancelCommand(FCPHandle handle) {
    if (!handle.IsValid() || !lock_) {
        return false;
    }

    IOLockLock(lock_);

    if (pending_ && pending_->transactionID == handle.transactionID) {
        ASFW_LOG_V2(FCP, "FCPTransport: Cancelling active command id=%u", handle.transactionID);
        const Async::AsyncHandle asyncHandle = pending_->asyncHandle;
        // Prevent a synchronous cancel completion from retrying this command
        // while the caller is explicitly terminating it.
        pending_->activeWriteAttempt.reset();
        pending_->successfulWriteAttempt.reset();
        pending_->asyncHandle = {};
        IOLockUnlock(lock_);

        // Completion can be delivered synchronously by a host implementation, so
        // never call into the async layer while holding the FCP state lock.
        if (asyncHandle.value && busOps_) {
            busOps_->Cancel(asyncHandle);
        }

        CompleteCommand(FCPStatus::kTransportError, {});
        return true;
    }

    const auto queuedIt = std::find_if(
        queued_.begin(), queued_.end(), [handle](const std::unique_ptr<OutstandingCommand>& cmd) {
            return cmd && cmd->transactionID == handle.transactionID;
        });
    if (queuedIt == queued_.end()) {
        IOLockUnlock(lock_);
        return false;
    }

    FCPCompletion completion = std::move((*queuedIt)->completion);
    queued_.erase(queuedIt);
    IOLockUnlock(lock_);
    ASFW_LOG_V2(FCP, "FCPTransport: Cancelled queued command id=%u", handle.transactionID);
    if (completion) {
        completion(FCPStatus::kTransportError, {});
    }
    return true;
}

//==============================================================================
// Response Reception
//==============================================================================

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void FCPTransport::OnFCPResponse(uint16_t srcNodeID,
                                 uint32_t generation,
                                 std::span<const uint8_t> payload) {
    IOLockLock(lock_);

    if (shuttingDown_ || !pending_) {
        IOLockUnlock(lock_);
        ASFW_LOG_V3(FCP,
                     "FCPTransport: Spurious response (no pending command)");
        return;
    }

    if (!pending_->successfulWriteAttempt.has_value()) {
        IOLockUnlock(lock_);
        ASFW_LOG_V3(FCP,
                     "FCPTransport: Ignoring response before write completion");
        return;
    }

    const FCPWriteAttempt successfulAttempt = *pending_->successfulWriteAttempt;
    const uint16_t expectedNodeID = successfulAttempt.targetNodeID;
    const bool exactMatch = srcNodeID == expectedNodeID;
    const bool nodeNumberMatch = (srcNodeID & 0x3F) == (expectedNodeID & 0x3F);
    if (!exactMatch && !nodeNumberMatch) {
        IOLockUnlock(lock_);
        ASFW_LOG_V1(FCP,
                     "FCPTransport: Response from wrong node: 0x%04x (expected node 0x%02x)",
                     srcNodeID, expectedNodeID & 0x3F);
        return;
    }
    if (!exactMatch && nodeNumberMatch) {
        ASFW_LOG_V3(FCP,
                     "FCPTransport: Accepting response with matching node number but different bus ID "
                     "(src=0x%04x expected=0x%04x)",
                     srcNodeID, expectedNodeID);
    }

    // Match the generation captured for this write attempt exactly.  A retry
    // after a bus reset starts a new write attempt with its new generation;
    // it must not make an old-generation response eligible for completion.
    if (generation != successfulAttempt.generation) {
        IOLockUnlock(lock_);
        ASFW_LOG_V1(FCP,
                     "FCPTransport: Response generation mismatch: %u (expected %u)",
                     generation, successfulAttempt.generation);
        return;
    }

    if (!ValidateResponse(payload)) {
        IOLockUnlock(lock_);
        ASFW_LOG_V3(FCP,
                     "FCPTransport: Response validation failed (likely stale/duplicate response)");
        return;
    }

    FCPFrame response;
    response.length = std::min(payload.size(), response.data.size());
    std::copy_n(payload.begin(), response.length, response.data.begin());

    ASFW_LOG_V2(FCP,
                "FCPTransport: Received response: ctype=0x%02x, length=%zu",
                response.data[0], response.length);

    if (response.data[0] == static_cast<uint8_t>(AVCResponseType::kInterim)) {
        pending_->gotInterim = true;

        ASFW_LOG_V2(FCP,
                    "FCPTransport: Got INTERIM response, extending timeout to %u ms",
                    config_.interimTimeoutMs);

        ScheduleTimeout(config_.interimTimeoutMs);

        IOLockUnlock(lock_);
        return;
    }

    IOLockUnlock(lock_);

    CompleteCommand(FCPStatus::kOk, response);
}

//==============================================================================
// Async Write Completion
//==============================================================================

void FCPTransport::OnAsyncWriteComplete(FCPWriteAttempt writeAttempt,
                                        Async::AsyncStatus status,
                                        std::span<const uint8_t> response) {
    (void)response;
    IOLockLock(lock_);

    if (shuttingDown_ || !pending_) {
        IOLockUnlock(lock_);
        return;
    }

    // A reset/retry can have issued another FCP write while this asynchronous
    // completion was in flight. It must not arm or fail the newer attempt.
    if (!pending_->activeWriteAttempt.has_value() ||
        pending_->activeWriteAttempt->id != writeAttempt.id) {
        IOLockUnlock(lock_);
        ASFW_LOG_V3(FCP, "FCPTransport: Ignoring stale write completion");
        return;
    }

    if (status != Async::AsyncStatus::kSuccess) {
        ASFW_LOG_V1(FCP,
                     "FCPTransport: Async write failed: %d",
                     static_cast<int>(status));

        if (pending_->retriesLeft > 0) {
            pending_->retriesLeft--;
            ASFW_LOG_V2(FCP,
                        "FCPTransport: Retrying command (%u retries left)",
                        pending_->retriesLeft);

            IOLockUnlock(lock_);
            RetryCommand();
            return;
        }

        IOLockUnlock(lock_);

        CompleteCommand(FCPStatus::kTransportError, {});
        return;
    }

    // The response interval begins only when the command write reached the
    // remote node. Apple IOFireWireAVC records the route and arms its timer
    // from writeDone; submission latency is not device response time.
    pending_->successfulWriteAttempt = writeAttempt;
    ScheduleTimeout(config_.timeoutMs);

    IOLockUnlock(lock_);
}

//==============================================================================
// Timeout Handling
//==============================================================================

void FCPTransport::OnCommandTimeout() {
    IOLockLock(lock_);

    if (shuttingDown_ || !pending_) {
        IOLockUnlock(lock_);
        return;
    }

    ASFW_LOG_V1(FCP,
                 "FCPTransport: Command timeout (interim=%d, retries=%u)",
                 pending_->gotInterim, pending_->retriesLeft);

    if (pending_->retriesLeft > 0) {
        pending_->retriesLeft--;
        ASFW_LOG_V2(FCP,
                    "FCPTransport: Retrying command after timeout (%u retries left)",
                    pending_->retriesLeft);

        IOLockUnlock(lock_);
        RetryCommand();
        return;
    } else {
        IOLockUnlock(lock_);
        CompleteCommand(FCPStatus::kTimeout, {});
        return;
    }
}

void FCPTransport::ScheduleTimeout(uint32_t timeoutMs) {
    if (shuttingDown_ || !pending_) {
        return;
    }

    // FCP has only one in-flight command, so an interim response replaces the
    // initial response deadline. The injected scheduler is the driver-owned
    // IOTimerDispatchSource, not an IOSleep block occupying a queue thread.
    CancelTimeout();
    const uint64_t epoch = ++nextTimeoutEpoch_;
    pending_->timeoutEpoch = epoch;

    const auto self = weak_from_this().lock();
    if (!self) {
        return;
    }

    const auto token = timerScheduler_->ScheduleAfter(
        static_cast<uint64_t>(timeoutMs) * 1'000'000ULL,
        [self, epoch] {
        IOLockLock(self->lock_);
        bool shouldFire = (!self->shuttingDown_ && self->pending_ &&
                           self->pending_->timeoutEpoch == epoch);
        if (shouldFire) {
            self->pending_->timeoutToken = Scheduling::kInvalidTimerToken;
        }
        IOLockUnlock(self->lock_);

        if (shouldFire) {
            self->OnCommandTimeout();
        }
    });

    if (token == Scheduling::kInvalidTimerToken) {
        ASFW_LOG_V1(FCP, "FCPTransport: Failed to schedule command timeout");
        return;
    }
    pending_->timeoutToken = token;
}

void FCPTransport::CancelTimeout() {
    if (!pending_ || pending_->timeoutToken == Scheduling::kInvalidTimerToken) {
        return;
    }

    const auto token = pending_->timeoutToken;
    pending_->timeoutToken = Scheduling::kInvalidTimerToken;
    timerScheduler_->Cancel(token);
}

//==============================================================================
// Retry Logic
//==============================================================================

void FCPTransport::RetryCommand() {
    IOLockLock(lock_);

    if (shuttingDown_ || !pending_) {
        IOLockUnlock(lock_);
        return;
    }

    CancelTimeout();
    const Async::AsyncHandle priorHandle = pending_->asyncHandle;
    pending_->asyncHandle = {};
    // Cancel() is allowed to complete synchronously. Clear both attempt
    // snapshots before calling out so that a late completion cannot arm or
    // complete the replacement write.
    pending_->activeWriteAttempt.reset();
    pending_->successfulWriteAttempt.reset();
    pending_->gotInterim = false;

    IOLockUnlock(lock_);

    // Cancel a previous in-flight attempt before submitting a retry. Its
    // completion is tagged with the old attempt and will be ignored even if
    // cancellation races delivery.
    if (priorHandle.value && busOps_) {
        busOps_->Cancel(priorHandle);
    }

    (void)StartPendingWrite();
}

//==============================================================================
// Bus Reset Handling
//==============================================================================

void FCPTransport::OnBusReset(uint32_t newGeneration) {
    IOLockLock(lock_);

    if (shuttingDown_ || !pending_) {
        IOLockUnlock(lock_);
        return;
    }

    const uint32_t activeGeneration = pending_->successfulWriteAttempt
                                          ? pending_->successfulWriteAttempt->generation
                                          : (pending_->activeWriteAttempt
                                                 ? pending_->activeWriteAttempt->generation
                                                 : 0U);
    ASFW_LOG_V2(FCP,
                "FCPTransport: Bus reset during command (gen %u → %u, "
                "allowRetry=%d, retriesLeft=%u)",
                activeGeneration, newGeneration,
                pending_->allowBusResetRetry, pending_->retriesLeft);

    const Async::AsyncHandle priorHandle = pending_->asyncHandle;
    pending_->asyncHandle = {};
    pending_->activeWriteAttempt.reset();
    pending_->successfulWriteAttempt.reset();
    pending_->gotInterim = false;
    CancelTimeout();

    if (pending_->allowBusResetRetry && pending_->retriesLeft > 0) {
        // Linux's generic fcp helper retries a pending transaction after its
        // update callback observes a reset (firewire/fcp.c:292-317). ASFW's
        // DeviceManager deliberately invalidates all routes at that point, so
        // we defer our idempotent replay until discovery has bound this GUID to
        // the new generation. Apple likewise keeps the in-generation command
        // variant from retrying directly on a reset
        // (IOFireWireAVCCommand.cpp:430-458). This is a clean-room policy for
        // our asynchronous, rebinding transport.
        pending_->awaitingRouteRevalidation = true;
        pending_->resetGeneration = newGeneration;
        pending_->gotInterim = false;

        ASFW_LOG_V2(FCP,
                    "FCPTransport: Deferring idempotent retry until route revalidation");

        IOLockUnlock(lock_);
        if (priorHandle.value && busOps_) {
            busOps_->Cancel(priorHandle);
        }
        return;
    }

    IOLockUnlock(lock_);
    if (priorHandle.value && busOps_) {
        busOps_->Cancel(priorHandle);
    }
    CompleteCommand(FCPStatus::kBusReset, {});
}

void FCPTransport::OnRouteRevalidated(uint32_t generation) {
    IOLockLock(lock_);

    if (shuttingDown_ || !pending_ || !pending_->awaitingRouteRevalidation) {
        IOLockUnlock(lock_);
        return;
    }

    const bool routeIsCurrent = generation == pending_->resetGeneration &&
                                busInfo_ && device_ && device_->IsReady() &&
                                device_->GetGeneration().value == generation &&
                                busInfo_->GetGeneration().value == generation &&
                                device_->GetNodeID() != 0xFFFFU;
    if (!routeIsCurrent) {
        IOLockUnlock(lock_);
        ASFW_LOG_V3(FCP,
                    "FCPTransport: Ignoring route revalidation for generation %u",
                    generation);
        return;
    }

    pending_->awaitingRouteRevalidation = false;
    pending_->resetGeneration = 0;
    --pending_->retriesLeft;
    IOLockUnlock(lock_);

    ASFW_LOG_V2(FCP,
                "FCPTransport: Retrying idempotent command on revalidated generation %u",
                generation);
    (void)StartPendingWrite();
}


bool FCPTransport::ValidateResponse(std::span<const uint8_t> response) const {
    if (response.size() < kAVCFrameMinSize) {
        ASFW_LOG_V3(FCP,
                     "FCPTransport: Response too small: %zu bytes",
                     response.size());
        return false;
    }

    if (response.size() > kAVCFrameMaxSize) {
        ASFW_LOG_V3(FCP,
                     "FCPTransport: Response too large: %zu bytes",
                     response.size());
        return false;
    }

    uint8_t cmdAddress = pending_->command.data[1];
    uint8_t rspAddress = response[1];

    if (cmdAddress != rspAddress) {
        ASFW_LOG_V3(FCP,
                     "FCPTransport: Response address mismatch: 0x%02x (expected 0x%02x)",
                     rspAddress, cmdAddress);
        return false;
    }

    uint8_t cmdOpcode = pending_->command.data[2];
    uint8_t rspOpcode = response[2];

    bool opcodeMatches = false;

    if (((cmdAddress & 0xF8) == 0x20) && (cmdOpcode == 0xD0)) {
        opcodeMatches = (rspOpcode == 0xD0 || rspOpcode == 0xC1 ||
                        rspOpcode == 0xC2 || rspOpcode == 0xC3 ||
                        rspOpcode == 0xC4);

        if (!opcodeMatches) {
            ASFW_LOG_V3(FCP,
                         "FCPTransport: Tape transport-state response opcode invalid: 0x%02x",
                         rspOpcode);
        }
    } else {
        opcodeMatches = ((rspOpcode & 0x7F) == (cmdOpcode & 0x7F));

        if (!opcodeMatches) {
            ASFW_LOG_V3(FCP,
                         "FCPTransport: Response opcode mismatch: 0x%02x (expected 0x%02x)",
                         rspOpcode, cmdOpcode);
        }
    }

    if (!opcodeMatches) {
        return false;
    }

    if (pending_->policy.responseMatcher &&
        !pending_->policy.responseMatcher(pending_->command.Payload(), response)) {
        ASFW_LOG_V3(FCP, "FCPTransport: Response rejected by command-specific matcher");
        return false;
    }

    return true;
}

//==============================================================================
// Command Completion
//==============================================================================

void FCPTransport::CompleteCommand(FCPStatus status, const FCPFrame& response) {
    // Must NOT be called with lock held

    IOLockLock(lock_);

    if (!pending_) {
        IOLockUnlock(lock_);
        return;
    }

    auto completion = std::move(pending_->completion);

    // Cancel timeout
    CancelTimeout();

    // Clear pending
    pending_.reset();

    IOLockUnlock(lock_);

    // Invoke completion OUTSIDE lock
    if (completion) {
        completion(status, response);
    }

    // A completion may synchronously submit another command. SubmitCommand()
    // sees the non-empty queue and appends it, so the oldest queued command
    // still starts first here.
    StartNextQueuedCommand();
}
