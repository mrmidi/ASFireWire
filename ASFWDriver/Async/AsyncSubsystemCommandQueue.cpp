#include "AsyncSubsystem.hpp"

#include "../Logging/Logging.hpp"
#include "Core/TransactionManager.hpp"

namespace ASFW::Async {

AsyncHandle AsyncSubsystem::ReadWithRetry(const ReadParams& params,
                                         const RetryPolicy& retryPolicy,
                                         CompletionCallback callback) {
    if (!commandQueue_ || !commandQueueLock_) {
        ASFW_LOG_ERROR(Async, "ReadWithRetry: Queue not initialized");
        return AsyncHandle{0};
    }

    static std::atomic<uint32_t> sNextQueuedHandle{0x80000000};
    AsyncHandle placeholderHandle{sNextQueuedHandle.fetch_add(1, std::memory_order_relaxed)};

    ::IOLockLock(commandQueueLock_);

    commandQueue_->emplace_back(std::make_shared<PendingCommandState>(
        params,
        retryPolicy,
        std::move(callback),
        placeholderHandle,
        this));
    const size_t queueDepth = commandQueue_->size();
    const bool wasIdle = !commandInFlight_.load(std::memory_order_acquire);

    ::IOLockUnlock(commandQueueLock_);

    ASFW_LOG(Async, "📥 Queued read request: dest=%04x addr=%08x:%08x len=%u handle=0x%x (queue depth=%zu)",
             params.destinationID, params.addressHigh, params.addressLow,
             params.length, placeholderHandle.value, queueDepth);

    if (wasIdle) {
        ASFW_LOG(Async, "🚀 Queue was idle, starting execution");
        ExecuteNextCommand();
    }

    return placeholderHandle;
}

bool AsyncSubsystem::Cancel(AsyncHandle handle) {
    if (!handle) {
        return false;
    }

    // ---------------------------------------------------------------------
    // 1) ReadWithRetry placeholder handles (queued command queue path)
    // ---------------------------------------------------------------------
    if (handle.value >= 0x80000000u) {
        if (!commandQueue_ || !commandQueueLock_) {
            return false;
        }

        CompletionCallback cancelledCallback{nullptr};
        PendingCommandPtr inFlight{};

        ::IOLockLock(commandQueueLock_);

        // Search pending queue first.
        for (auto it = commandQueue_->begin(); it != commandQueue_->end(); ++it) {
            const auto& cmd = *it;
            if (!cmd) {
                continue;
            }
            if (cmd->publicHandle.value == handle.value) {
                cancelledCallback = cmd->userCallback;
                commandQueue_->erase(it);
                ::IOLockUnlock(commandQueueLock_);

                // Must not invoke callback inline on the cancel path.
                PostToWorkloop(^{
                    if (cancelledCallback) {
                        cancelledCallback(handle, AsyncStatus::kAborted, 0xFF, std::span<const uint8_t>{});
                    }
                });
                return true;
            }
        }

        // Not queued: check the single in-flight queued command (queue is serial).
        inFlight = inFlightCommand_;
        ::IOLockUnlock(commandQueueLock_);

        if (!inFlight || inFlight->publicHandle.value != handle.value) {
            return false;
        }

        inFlight->cancelRequested.store(true, std::memory_order_release);

        // If the underlying transaction has been submitted, cancel it too.
        const AsyncHandle underlying = inFlight->currentHandle;
        if (underlying) {
            (void)Cancel(underlying);
        }

        return true;
    }

    // ---------------------------------------------------------------------
    // 2) Normal async transaction handles (tLabel+1 => 1..64)
    // ---------------------------------------------------------------------
    if (handle.value >= 1 && handle.value <= 64) {
        if (!txnMgr_ || !tracking_) {
            return false;
        }

        const uint8_t label = static_cast<uint8_t>(handle.value - 1);
        auto txnPtr = txnMgr_->Extract(TLabel{label});
        if (!txnPtr) {
            return false;
        }

        if (auto* alloc = tracking_->GetLabelAllocator()) {
            alloc->Free(label);
        }

        // Must not invoke callback inline on the cancel path.
        Transaction* raw = txnPtr.release();
        PostToWorkloop(^{
            if (!raw) {
                return;
            }

            if (!IsTerminalState(raw->state())) {
                raw->TransitionTo(TransactionState::Cancelled, "AsyncSubsystem::Cancel");
                raw->InvokeResponseHandler(kIOReturnAborted, 0xFF, {});
            }
            delete raw;
        });

        return true;
    }

    return false;
}

// ============================================================================
// Command Queue Implementation (Apple IOFWCmdQ pattern)
// ============================================================================

void AsyncSubsystem::ExecuteNextCommand() {
    if (!commandQueueLock_ || !commandQueue_) {
        return;
    }

    ::IOLockLock(commandQueueLock_);

    if (commandQueue_->empty()) {
        commandInFlight_.store(false, std::memory_order_release);
        inFlightCommand_.reset();
        ::IOLockUnlock(commandQueueLock_);
        ASFW_LOG(Async, "📭 Command queue empty - going idle");
        return;
    }

    // Dequeue next command (Apple IOFWCmdQ pattern: remove from queue before execution)
    PendingCommandPtr cmd = std::move(commandQueue_->front());
    commandQueue_->pop_front();
    const size_t remainingCommands = commandQueue_->size();
    commandInFlight_.store(true, std::memory_order_release);
    inFlightCommand_ = cmd;

    ::IOLockUnlock(commandQueueLock_);

    if (!cmd) {
        ASFW_LOG_ERROR(Async, "ExecuteNextCommand: dequeued null command state");
        ExecuteNextCommand();
        return;
    }

    ASFW_LOG(Async,
             "📤 Executing queued command to %04x addr=%08x:%08x len=%u retries=%u (queue depth=%zu)",
             cmd->params.destinationID,
             cmd->params.addressHigh,
             cmd->params.addressLow,
             cmd->params.length,
             cmd->retriesRemaining,
             remainingCommands);

    // If cancellation raced with the dequeue window, complete as aborted without submitting.
    if (cmd->cancelRequested.load(std::memory_order_acquire)) {
        PostToWorkloop(^{
            if (cmd->userCallback) {
                cmd->userCallback(cmd->publicHandle, AsyncStatus::kAborted, 0xFF, std::span<const uint8_t>{});
            }
        });

        ::IOLockLock(commandQueueLock_);
        if (inFlightCommand_ == cmd) {
            inFlightCommand_.reset();
        }
        ::IOLockUnlock(commandQueueLock_);

        ExecuteNextCommand();
        return;
    }

    // Static wrapper for internal callback (handles retry + queue advancement).
    struct InternalCallbackContext {
        static void FinishAndAdvance(AsyncSubsystem& subsystem,
                                     const PendingCommandPtr& cmd) {
            if (subsystem.commandQueueLock_) {
                ::IOLockLock(subsystem.commandQueueLock_);
                if (subsystem.inFlightCommand_ == cmd) {
                    subsystem.inFlightCommand_.reset();
                }
                ::IOLockUnlock(subsystem.commandQueueLock_);
            }
            subsystem.ExecuteNextCommand();
        }

        static void HandleCompletion(const PendingCommandPtr& cmdPtr,
                                     AsyncHandle /*handle*/,
                                     AsyncStatus status,
                                     uint8_t responseCode,
                                     std::span<const uint8_t> responsePayload) {
            AsyncSubsystem* subsystem = cmdPtr ? cmdPtr->subsystem : nullptr;
            if (!subsystem || !cmdPtr) {
                return;
            }

            // Cancellation is best-effort and must never fail the node/session.
            // If cancellation was requested at any point, override the completion result.
            if (cmdPtr->cancelRequested.load(std::memory_order_acquire)) {
                status = AsyncStatus::kAborted;
                responseCode = 0xFF;
                responsePayload = std::span<const uint8_t>{};
            }

            if (status == AsyncStatus::kSuccess) {
                ASFW_LOG(Async, "✅ Command completed successfully: handle=0x%x", cmdPtr->publicHandle.value);
                if (cmdPtr->userCallback) {
                    cmdPtr->userCallback(cmdPtr->publicHandle, status, responseCode, responsePayload);
                }
                FinishAndAdvance(*subsystem, cmdPtr);
                return;
            }

            if (status == AsyncStatus::kAborted) {
                ASFW_LOG(Async, "🛑 Command cancelled: handle=0x%x", cmdPtr->publicHandle.value);
                if (cmdPtr->userCallback) {
                    cmdPtr->userCallback(cmdPtr->publicHandle,
                                         AsyncStatus::kAborted,
                                         0xFF,
                                         std::span<const uint8_t>{});
                }
                FinishAndAdvance(*subsystem, cmdPtr);
                return;
            }

            if (cmdPtr->retriesRemaining > 0) {
                bool shouldRetry = false;
                if (status == AsyncStatus::kTimeout && cmdPtr->retryPolicy.retryOnTimeout) {
                    shouldRetry = true;
                } else if (status == AsyncStatus::kBusyRetryExhausted && cmdPtr->retryPolicy.retryOnBusy) {
                    shouldRetry = true;
                }

                if (shouldRetry && !cmdPtr->cancelRequested.load(std::memory_order_acquire)) {
                    cmdPtr->retriesRemaining--;
                    ASFW_LOG(Async,
                             "🔄 Command failed (status=%u), retrying (%u attempts left) handle=0x%x",
                             static_cast<unsigned>(status),
                             cmdPtr->retriesRemaining,
                             cmdPtr->publicHandle.value);

                    // Re-submit immediately (already dequeued, so no queue push).
                    AsyncHandle retryHandle = subsystem->Read(
                        cmdPtr->params,
                        [cmdPtr](AsyncHandle h, AsyncStatus s, uint8_t rc, std::span<const uint8_t> payload) {
                            HandleCompletion(cmdPtr, h, s, rc, payload);
                        });

                    cmdPtr->currentHandle = retryHandle;
                    return;  // Keep command in-flight until it completes or retries exhaust.
                }
            }

            // No retry or retries exhausted - final failure.
            ASFW_LOG(Async,
                     "❌ Command failed permanently: handle=0x%x status=%u",
                     cmdPtr->publicHandle.value,
                     static_cast<unsigned>(status));

            if (cmdPtr->userCallback) {
                cmdPtr->userCallback(cmdPtr->publicHandle, status, responseCode, responsePayload);
            }

            FinishAndAdvance(*subsystem, cmdPtr);
        }
    };

    // Submit command to hardware layer.
    const AsyncHandle handle = Read(cmd->params, [cmd](AsyncHandle h,
                                                      AsyncStatus s,
                                                      uint8_t rc,
                                                      std::span<const uint8_t> payload) {
        InternalCallbackContext::HandleCompletion(cmd, h, s, rc, payload);
    });
    cmd->currentHandle = handle;

    ASFW_LOG(Async, "📮 Command submitted: public=0x%x current=0x%x", cmd->publicHandle.value, handle.value);
}

} // namespace ASFW::Async

