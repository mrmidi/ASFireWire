//
// FCPTransport.cpp
// ASFWDriver - AV/C Protocol Layer
//
// FCP (Function Control Protocol) transport layer implementation
//

#include "FCPTransport.hpp"
#include "../../Logging/Logging.hpp"
#include <DriverKit/OSCollections.h>

using namespace ASFW::Protocols::AVC;

//==============================================================================
// Constructor / Destructor
//==============================================================================

FCPTransport::FCPTransport(Async::AsyncSubsystem& async,
                           Discovery::FWDevice& device,
                           const FCPTransportConfig& config)
    : async_(async), device_(device), config_(config) {

    // Allocate lock (DriverKit IOLock)
    lock_ = IOLockAlloc();
    if (!lock_) {
        ASFW_LOG_ERROR(Async,
                     "FCPTransport: Failed to allocate lock");
    }

    // Create dedicated DriverKit dispatch queue for timeout handling
    IODispatchQueue* queue = nullptr;
    IODispatchQueueName queueName = "com.asfw.fcp.timeout";
    auto kr = IODispatchQueue::Create(queueName, 0, 0, &queue);
    if (kr != kIOReturnSuccess || !queue) {
        ASFW_LOG_ERROR(Async,
                     "FCPTransport: Failed to create timeout queue (kr=0x%08x)",
                     kr);
    } else {
        timeoutQueue_ = OSSharedPtr(queue, OSNoRetain);
    }

    // Create timer source
    IOTimerDispatchSource* timer = nullptr;
    kr = IOTimerDispatchSource::Create(timeoutQueue_.get(), &timer);
    if (kr != kIOReturnSuccess || !timer) {
        ASFW_LOG_ERROR(Async,
                     "FCPTransport: Failed to create timer source (kr=0x%08x)",
                     kr);
    } else {
        timer_ = OSSharedPtr(timer, OSNoRetain);
        // Timer will be scheduled with WakeAtTime when needed
    }

    ASFW_LOG_INFO(Async,
                "FCPTransport: Initialized for device nodeID=%u, "
                "cmdAddr=0x%llx, rspAddr=0x%llx",
                device_.GetNodeID(), config_.commandAddress,
                config_.responseAddress);
}

FCPTransport::~FCPTransport() {
    shuttingDown_ = true;

    // Cancel any pending command
    if (pending_) {
        CompleteCommand(FCPStatus::kTransportError, {});
    }

    // Drain and release timeout queue
    if (timer_) {
        timer_->Cancel(^(void) {
            // Cancellation complete
        });
        timer_.reset();
    }

    if (timeoutQueue_) {
        if (!timeoutQueue_->OnQueue()) {
            timeoutQueue_->DispatchSync(^{
                // Ensure all pending timeout work completes before destruction
            });
        }
        timeoutQueue_.reset();
    }

    // Free lock
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }

    ASFW_LOG_INFO(Async, "FCPTransport: Destroyed");
}

//==============================================================================
// Command Submission
//==============================================================================

FCPHandle FCPTransport::SubmitCommand(const FCPFrame& command,
                                      FCPCompletion completion) {
    // Validate payload size
    if (!command.IsValid()) {
        ASFW_LOG_ERROR(Async,
                     "FCPTransport: Invalid command size %zu (must be 3-512)",
                     command.length);
        completion(FCPStatus::kInvalidPayload, {});
        return {};
    }

    IOLockLock(lock_);

    // v1: Reject if command already pending (single outstanding only)
    if (pending_) {
        IOLockUnlock(lock_);

        ASFW_LOG_ERROR(Async,
                     "FCPTransport: Command already pending (v1 concurrency limit)");
        completion(FCPStatus::kBusy, {});
        return {};
    }

    // Create outstanding command state
    auto cmd = std::make_unique<OutstandingCommand>();
    cmd->command = command;
    cmd->completion = std::move(completion);
    cmd->generation = device_.GetGeneration();
    cmd->retriesLeft = config_.maxRetries;
    cmd->allowBusResetRetry = config_.allowBusResetRetry;
    cmd->gotInterim = false;

    ASFW_LOG_INFO(Async,
                "FCPTransport: Submitting command: opcode=0x%02x, length=%zu, "
                "generation=%u, retries=%u",
                command.data[2], command.length,
                cmd->generation, cmd->retriesLeft);

    // Store as pending before issuing asynchronous write
    pending_ = std::move(cmd);

    auto handle = SubmitWriteCommand(pending_->command);
    if (!handle.value) {
        auto completion = std::move(pending_->completion);
        pending_.reset();

        IOLockUnlock(lock_);
        ASFW_LOG_ERROR(Async,
                     "FCPTransport: Failed to submit async write");
        completion(FCPStatus::kTransportError, {});
        return {};
    }

    pending_->asyncHandle = handle;

    // Schedule timeout
    ScheduleTimeout(config_.timeoutMs);

    IOLockUnlock(lock_);

    return FCPHandle{kTransactionID};
}

ASFW::Async::AsyncHandle FCPTransport::SubmitWriteCommand(const FCPFrame& frame) {
    ASFW::Async::WriteParams params{};
    params.destinationID = device_.GetNodeID();
    params.addressHigh = static_cast<uint32_t>((config_.commandAddress >> 32) & 0xFFFFFFFFULL);
    params.addressLow = static_cast<uint32_t>(config_.commandAddress & 0xFFFFFFFFULL);
    params.payload = frame.Payload().data();
    params.length = static_cast<uint32_t>(frame.length);

    return async_.Write(params,
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

    ASFW_LOG_INFO(Async,
                "FCPTransport: Cancelling command");

    // Cancel async operation
    async_.Cancel(pending_->asyncHandle);

    // Complete with error
    auto completion = std::move(pending_->completion);
    pending_.reset();

    IOLockUnlock(lock_);

    // Invoke completion OUTSIDE lock
    completion(FCPStatus::kTransportError, {});

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
        ASFW_LOG_DEBUG(Async,
                     "FCPTransport: Spurious response (no pending command)");
        return;
    }

    // Validate source node matches target
    if (srcNodeID != device_.GetNodeID()) {
        IOLockUnlock(lock_);
        ASFW_LOG_ERROR(Async,
                     "FCPTransport: Response from wrong node: %u (expected %u)",
                     srcNodeID, device_.GetNodeID());
        return;
    }

    // Validate generation (if not allowing cross-generation)
    if (!pending_->allowBusResetRetry &&
        generation != pending_->generation) {
        IOLockUnlock(lock_);
        ASFW_LOG_ERROR(Async,
                     "FCPTransport: Response generation mismatch: %u (expected %u)",
                     generation, pending_->generation);
        return;
    }

    // Validate response payload
    if (!ValidateResponse(payload)) {
        IOLockUnlock(lock_);
        ASFW_LOG_ERROR(Async,
                     "FCPTransport: Response validation failed");
        return;
    }

    // Copy response payload
    FCPFrame response;
    response.length = std::min(payload.size(), response.data.size());
    std::copy_n(payload.begin(), response.length, response.data.begin());

    ASFW_LOG_INFO(Async,
                "FCPTransport: Received response: ctype=0x%02x, length=%zu",
                response.data[0], response.length);

    // Check for interim response (ctype == 0x0F)
    if (response.data[0] == static_cast<uint8_t>(AVCResponseType::kInterim)) {
        // Extend timeout for final response
        pending_->gotInterim = true;

        ASFW_LOG_INFO(Async,
                    "FCPTransport: Got INTERIM response, extending timeout to %u ms",
                    config_.interimTimeoutMs);

        ScheduleTimeout(config_.interimTimeoutMs);

        IOLockUnlock(lock_);
        return;
    }

    // Final response received
    auto completion = std::move(pending_->completion);
    pending_.reset();

    IOLockUnlock(lock_);

    // Invoke completion OUTSIDE lock
    completion(FCPStatus::kOk, response);
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
        return;  // Already completed/cancelled
    }

    if (status != Async::AsyncStatus::kSuccess) {
        ASFW_LOG_ERROR(Async,
                     "FCPTransport: Async write failed: %d",
                     static_cast<int>(status));

        // Retry or fail
        if (pending_->retriesLeft > 0) {
            pending_->retriesLeft--;
            ASFW_LOG_INFO(Async,
                        "FCPTransport: Retrying command (%u retries left)",
                        pending_->retriesLeft);

            IOLockUnlock(lock_);
            RetryCommand();
            return;
        } else {
            auto completion = std::move(pending_->completion);
            pending_.reset();

            IOLockUnlock(lock_);

            completion(FCPStatus::kTransportError, {});
            return;
        }
    }

    // Async write succeeded, waiting for FCP response
    // (response will arrive via OnFCPResponse)

    IOLockUnlock(lock_);
}

//==============================================================================
// Timeout Handling
//==============================================================================

void FCPTransport::OnCommandTimeout() {
    IOLockLock(lock_);

    if (!pending_) {
        IOLockUnlock(lock_);
        return;  // Already completed
    }

    ASFW_LOG_ERROR(Async,
                 "FCPTransport: Command timeout (interim=%d, retries=%u)",
                 pending_->gotInterim, pending_->retriesLeft);

    // Retry or fail
    if (pending_->retriesLeft > 0) {
        pending_->retriesLeft--;
        ASFW_LOG_INFO(Async,
                    "FCPTransport: Retrying command after timeout (%u retries left)",
                    pending_->retriesLeft);

        IOLockUnlock(lock_);
        RetryCommand();
        return;
    } else {
        auto completion = std::move(pending_->completion);
        pending_.reset();
        completion(FCPStatus::kTimeout, {});
        return;
    }
}


void FCPTransport::ScheduleTimeout(uint32_t timeoutMs) {
    // Must be called with lock held

    if (!pending_) {
        return;
    }

    pending_->timeoutToken = ++nextTimeoutToken_;

    if (!timeoutQueue_) {
        ASFW_LOG_ERROR(Async,
                     "FCPTransport: Timeout queue unavailable");
        return;
    }

    const uint64_t token = pending_->timeoutToken;
    
    // Schedule timeout on dispatch queue
    timeoutQueue_->DispatchAsync(^{
        // Calculate deadline
        uint64_t start = mach_continuous_time();
        uint64_t timeoutNs = timeoutMs * 1000000ULL;
        uint64_t deadline = start + timeoutNs;
        
        // Wait until deadline
        while (mach_continuous_time() < deadline) {
            // Busy wait (no sleep available in DriverKit)
            // Check cancellation periodically
            IOLockLock(lock_);
            bool cancelled = (!pending_ || pending_->timeoutToken != token);
            IOLockUnlock(lock_);
            
            if (cancelled) {
                return;
            }
        }
        
        // Timeout expired - check if still valid
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
    // Must be called with lock held
    if (pending_) {
        pending_->timeoutToken = 0;
    }
    
    if (timer_) {
        timer_->Cancel(^(void) {
            // Cancellation complete
        });
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

    // Update generation (may have changed after bus reset)
    pending_->generation = device_.GetGeneration();
    pending_->gotInterim = false;

    ASFW_LOG_INFO(Async,
                "FCPTransport: Retrying command with generation=%u",
                pending_->generation);

    // Resubmit async write
    auto handle = SubmitWriteCommand(pending_->command);
    if (!handle.value) {
        auto completion = std::move(pending_->completion);
        pending_.reset();

        IOLockUnlock(lock_);
        ASFW_LOG_ERROR(Async,
                     "FCPTransport: Async write submission failed during retry");
        completion(FCPStatus::kTransportError, {});
        return;
    }

    pending_->asyncHandle = handle;

    // Schedule new timeout
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

    ASFW_LOG_INFO(Async,
                "FCPTransport: Bus reset during command (gen %u → %u, "
                "allowRetry=%d, retriesLeft=%u)",
                pending_->generation, newGeneration,
                pending_->allowBusResetRetry, pending_->retriesLeft);

    if (pending_->allowBusResetRetry && pending_->retriesLeft > 0) {
        // Safe to retry (e.g., STATUS query)
        pending_->retriesLeft--;
        pending_->generation = newGeneration;
        pending_->gotInterim = false;

        ASFW_LOG_INFO(Async,
                    "FCPTransport: Retrying command after bus reset");

        IOLockUnlock(lock_);
        RetryCommand();
        return;
    } else {
        // Fail command
        auto completion = std::move(pending_->completion);
        pending_.reset();

        IOLockUnlock(lock_);

        completion(FCPStatus::kBusReset, {});
        return;
    }
}

//==============================================================================
// Response Validation
//==============================================================================

bool FCPTransport::ValidateResponse(std::span<const uint8_t> response) const {
    // Must be called with lock held

    // Check size
    if (response.size() < kAVCFrameMinSize) {
        ASFW_LOG_ERROR(Async,
                     "FCPTransport: Response too small: %zu bytes",
                     response.size());
        return false;
    }

    if (response.size() > kAVCFrameMaxSize) {
        ASFW_LOG_ERROR(Async,
                     "FCPTransport: Response too large: %zu bytes",
                     response.size());
        return false;
    }

    // Check that opcode matches (byte[2] in response should match command)
    // Some devices change the subunit byte (byte[1]), but opcode should match
    uint8_t cmdOpcode = pending_->command.data[2];
    uint8_t rspOpcode = response[2];

    if ((rspOpcode & 0x7F) != (cmdOpcode & 0x7F)) {
        ASFW_LOG_ERROR(Async,
                     "FCPTransport: Response opcode mismatch: 0x%02x (expected 0x%02x)",
                     rspOpcode, cmdOpcode);
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
    completion(status, response);
}
