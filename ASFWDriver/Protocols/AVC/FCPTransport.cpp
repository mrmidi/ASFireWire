//
// FCPTransport.cpp
// ASFWDriver - AV/C Protocol Layer
//
// FCP (Function Control Protocol) transport layer implementation
//

#include "FCPTransport.hpp"
#include "../../Logging/Logging.hpp"
#ifndef ASFW_HOST_TEST
#include <DriverKit/OSCollections.h>
#include <DriverKit/IOLib.h>
#endif
#include <time.h>

using namespace ASFW::Protocols::AVC;

//==============================================================================
// Constructor / Destructor
//==============================================================================

// OSDefineMetaClassAndStructors(FCPTransport, OSObject);

//==============================================================================
// Init / Free
//==============================================================================

bool FCPTransport::init(Async::AsyncSubsystem* async,
                        Discovery::FWDevice* device,
                        const FCPTransportConfig& config) {
    if (!OSObject::init()) {
        return false;
    }

    async_ = async;
    device_ = device;
    config_ = config;

    // Allocate lock (DriverKit IOLock)
    lock_ = IOLockAlloc();
    if (!lock_) {
        ASFW_LOG_V1(FCP, "FCPTransport: Failed to allocate lock");
        return false;
    }

    // Create dedicated DriverKit dispatch queue for timeout handling
    IODispatchQueue* queue = nullptr;
    IODispatchQueueName queueName = "com.asfw.fcp.timeout";
    auto kr = IODispatchQueue::Create(queueName, 0, 0, &queue);
    if (kr != kIOReturnSuccess || !queue) {
        ASFW_LOG_V1(FCP, "FCPTransport: Failed to create timeout queue (kr=0x%08x)", kr);
    } else {
        timeoutQueue_ = OSSharedPtr(queue, OSNoRetain);
    }

    ASFW_LOG_V1(FCP,
                "FCPTransport: Initialized for device nodeID=%u, "
                "cmdAddr=0x%llx, rspAddr=0x%llx",
                device_->GetNodeID(), config_.commandAddress,
                config_.responseAddress);
    
    return true;
}

void FCPTransport::free() {
    shuttingDown_ = true;

    // Cancel any pending command
    if (pending_) {
        CompleteCommand(FCPStatus::kTransportError, {});
    }

    // Release queue
    timeoutQueue_.reset();

    // Free lock (cannot throw in DriverKit)
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }

    ASFW_LOG_V1(FCP, "FCPTransport: Destroyed");
    
    OSObject::free();
}

//==============================================================================
// Command Submission
//==============================================================================

FCPHandle FCPTransport::SubmitCommand(const FCPFrame& command,
                                      FCPCompletion completion) {
    if (!command.IsValid()) {
        ASFW_LOG_V1(FCP,
                     "FCPTransport: Invalid command size %zu (must be 3-512)",
                     command.length);
        completion(FCPStatus::kInvalidPayload, {});
        return {};
    }

    if (shuttingDown_) {
        completion(FCPStatus::kTransportError, {});
        return {};
    }

    IOLockLock(lock_);

    if (pending_) {
        IOLockUnlock(lock_);

        ASFW_LOG_V1(FCP,
                     "FCPTransport: Command already pending");
        completion(FCPStatus::kBusy, {});
        return {};
    }

    auto cmd = std::make_unique<OutstandingCommand>();
    cmd->command = command;
    cmd->completion = std::move(completion);
    // Keep generation source consistent with RX routing: both should use Async's
    // generation tracker instead of mixing Discovery device generation.
    cmd->generation = static_cast<uint32_t>(
        async_->GetGenerationTracker().GetCurrentState().generation16);
    cmd->retriesLeft = config_.maxRetries;
    cmd->allowBusResetRetry = config_.allowBusResetRetry;
    cmd->gotInterim = false;

    {
        char hexbuf[64] = {0};
        size_t hexlen = std::min(command.length, static_cast<size_t>(16));
        for (size_t i = 0; i < hexlen; i++) {
            snprintf(hexbuf + i*3, 4, "%02x ", command.data[i]);
        }
        ASFW_LOG_HEX(FCP,
                    "FCPTransport: Submitting command: opcode=0x%02x, length=%zu, "
                    "generation=%u, retries=%u, data=[%{public}s]",
                    command.data[2], command.length,
                    cmd->generation, cmd->retriesLeft, hexbuf);
    }

    pending_ = std::move(cmd);

    FCPFrame commandCopy = pending_->command;

    IOLockUnlock(lock_);

    auto handle = SubmitWriteCommand(commandCopy);
    if (!handle.value) {
        IOLockLock(lock_);
        if (!pending_) {
            IOLockUnlock(lock_);
            return {};
        }

        auto completionCallback = std::move(pending_->completion);
        pending_.reset();

        IOLockUnlock(lock_);
        ASFW_LOG_V1(FCP,
                     "FCPTransport: Failed to submit async write");
        completionCallback(FCPStatus::kTransportError, {});
        return {};
    }

    IOLockLock(lock_);
    if (!pending_) {
        IOLockUnlock(lock_);
        async_->Cancel(handle);
        return {};
    }

    pending_->asyncHandle = handle;

    ScheduleTimeout(config_.timeoutMs);

    IOLockUnlock(lock_);

    return FCPHandle{kTransactionID};
}

ASFW::Async::AsyncHandle FCPTransport::SubmitWriteCommand(const FCPFrame& frame) {
    ASFW::Async::WriteParams params{};
    params.destinationID = device_->GetNodeID();
    params.addressHigh = static_cast<uint32_t>((config_.commandAddress >> 32) & 0xFFFFFFFFULL);
    params.addressLow = static_cast<uint32_t>(config_.commandAddress & 0xFFFFFFFFULL);
    params.payload = frame.Payload().data();
    params.length = static_cast<uint32_t>(frame.length);

    return async_->Write(params,
        [this](ASFW::Async::AsyncHandle handle,
               ASFW::Async::AsyncStatus status,
               std::span<const uint8_t> response) {
            OnAsyncWriteComplete(handle, status, response);
        });
}

//==============================================================================
// Command Cancellation
//==============================================================================

bool FCPTransport::CancelCommand(FCPHandle handle) {
    if (!handle.IsValid() || handle.transactionID != kTransactionID) {
        return false;
    }

    IOLockLock(lock_);

    if (!pending_) {
        IOLockUnlock(lock_);
        return false;
    }

    ASFW_LOG_V2(FCP,
                "FCPTransport: Cancelling command");

    // Cancel async operation
    async_->Cancel(pending_->asyncHandle);

    IOLockUnlock(lock_);

    CompleteCommand(FCPStatus::kTransportError, {});

    return true;
}

//==============================================================================
// Response Reception
//==============================================================================

void FCPTransport::OnFCPResponse(uint16_t srcNodeID,
                                 uint32_t generation,
                                 std::span<const uint8_t> payload) {
    IOLockLock(lock_);

    if (!pending_) {
        IOLockUnlock(lock_);
        ASFW_LOG_V3(FCP,
                     "FCPTransport: Spurious response (no pending command)");
        return;
    }

    const uint16_t expectedNodeID = device_->GetNodeID();
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

    // Generation value can be unknown (0) in some receive paths while the bus is still
    // converging; accept unknown generation as wildcard to avoid dropping valid responses.
    if (!pending_->allowBusResetRetry &&
        generation != 0 &&
        pending_->generation != 0 &&
        generation != pending_->generation) {
        IOLockUnlock(lock_);
        ASFW_LOG_V1(FCP,
                     "FCPTransport: Response generation mismatch: %u (expected %u)",
                     generation, pending_->generation);
        return;
    }
    if (!pending_->allowBusResetRetry &&
        (generation == 0 || pending_->generation == 0)) {
        ASFW_LOG_V3(FCP,
                    "FCPTransport: Accepting response with unknown generation "
                    "(rx=%u pending=%u)",
                    generation,
                    pending_->generation);
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

void FCPTransport::OnAsyncWriteComplete(Async::AsyncHandle handle,
                                        Async::AsyncStatus status,
                                        std::span<const uint8_t> response) {
    (void)handle;
    (void)response;
    IOLockLock(lock_);

    if (!pending_) {
        IOLockUnlock(lock_);
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

    IOLockUnlock(lock_);
}

//==============================================================================
// Timeout Handling
//==============================================================================

void FCPTransport::OnCommandTimeout() {
    IOLockLock(lock_);

    if (!pending_) {
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
    if (!pending_) {
        return;
    }

    pending_->timeoutToken = ++nextTimeoutToken_;

    auto queue = timeoutQueue_;
    if (!queue) {
        ASFW_LOG_V1(FCP,
                     "FCPTransport: Timeout queue unavailable (timeoutMs=%u)",
                     timeoutMs);
        return;
    }

    const uint64_t token = pending_->timeoutToken;

    queue->DispatchAsync(^{
        IOLockLock(lock_);
        bool stillPending = (pending_ && pending_->timeoutToken == token);
        IOLockUnlock(lock_);

        if (!stillPending) {
            return;
        }

        IOSleep(static_cast<uint64_t>(timeoutMs));

        IOLockLock(lock_);
        bool shouldFire = (pending_ && pending_->timeoutToken == token);
        if (shouldFire) {
            pending_->timeoutToken = 0;
        }
        IOLockUnlock(lock_);

        if (shouldFire) {
            OnCommandTimeout();
        }
    });
}

void FCPTransport::CancelTimeout() {
    if (pending_) {
        pending_->timeoutToken = 0;
    }
}

//==============================================================================
// Retry Logic
//==============================================================================

void FCPTransport::RetryCommand() {
    IOLockLock(lock_);

    if (!pending_) {
        IOLockUnlock(lock_);
        return;
    }

    // Keep retries aligned with Async/RX generation source.
    pending_->generation = static_cast<uint32_t>(
        async_->GetGenerationTracker().GetCurrentState().generation16);
    pending_->gotInterim = false;

    ASFW_LOG_V2(FCP,
                "FCPTransport: Retrying command with generation=%u",
                pending_->generation);

    FCPFrame commandCopy = pending_->command;
    IOLockUnlock(lock_);

    auto handle = SubmitWriteCommand(commandCopy);
    if (!handle.value) {
        ASFW_LOG_V1(FCP,
                     "FCPTransport: Async write submission failed during retry");
        CompleteCommand(FCPStatus::kTransportError, {});
        return;
    }

    IOLockLock(lock_);
    if (!pending_) {
        IOLockUnlock(lock_);
        async_->Cancel(handle);
        return;
    }

    pending_->asyncHandle = handle;

    ScheduleTimeout(config_.timeoutMs);

    IOLockUnlock(lock_);
}

//==============================================================================
// Bus Reset Handling
//==============================================================================

void FCPTransport::OnBusReset(uint32_t newGeneration) {
    IOLockLock(lock_);

    if (!pending_) {
        IOLockUnlock(lock_);
        return;
    }

    ASFW_LOG_V2(FCP,
                "FCPTransport: Bus reset during command (gen %u → %u, "
                "allowRetry=%d, retriesLeft=%u)",
                pending_->generation, newGeneration,
                pending_->allowBusResetRetry, pending_->retriesLeft);

    if (pending_->allowBusResetRetry && pending_->retriesLeft > 0) {
        pending_->retriesLeft--;
        pending_->generation = newGeneration;
        pending_->gotInterim = false;

        ASFW_LOG_V2(FCP,
                    "FCPTransport: Retrying command after bus reset");

        IOLockUnlock(lock_);
        RetryCommand();
        return;
    } else {
        IOLockUnlock(lock_);

        CompleteCommand(FCPStatus::kBusReset, {});
        return;
    }
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

    return opcodeMatches;
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
    completion(status, response);
}
