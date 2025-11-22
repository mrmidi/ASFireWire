#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <span>
#include <cstring>
#include <optional>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <DriverKit/IOLib.h>

#include "../AsyncTypes.hpp"
#include "../../Shared/Memory/DMAMemoryManager.hpp"
#include "../../Common/FWCommon.hpp"  // For FW::Response, FW::RespName, FW::ResponseFromByte
#include "CompletionQueue.hpp"
#include "TxCompletion.hpp"
#include "LabelAllocator.hpp"
#include "PayloadRegistry.hpp"
#include "../Engine/ContextManager.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Logging/LogConfig.hpp"

// Phase 2.0: Transaction infrastructure (sole source of truth)
#include "../Core/Transaction.hpp"
#include "../Core/TransactionManager.hpp"
#include "TransactionCompletionHandler.hpp"

namespace ASFW::Async {

// Forward declarations
class LabelAllocator;
namespace Engine { class ContextManager; }

// Metadata for registering a new outgoing transaction.
struct TxMetadata {
    uint16_t generation{0};
    uint16_t sourceNodeID{0};
    uint16_t destinationNodeID{0};
    uint8_t  tLabel{0};
    uint8_t  tCode{0};
    uint32_t expectedLength{0};
    CompletionCallback callback{nullptr};
    CompletionStrategy completionStrategy{CompletionStrategy::CompleteOnAT};  // Explicit two-path model
};

// A parsed incoming response packet, ready for matching.
struct RxResponse {
    uint16_t generation{0};
    uint16_t sourceNodeID{0};
    uint16_t destinationNodeID{0};
    uint8_t  tLabel{0};
    uint8_t  tCode{0};
    uint8_t  rCode{0};                // Response code (for response tCodes 0x6, 0x7)
    std::span<const uint8_t> payload{};
    OHCIEventCode eventCode{static_cast<OHCIEventCode>(0)};
    uint16_t hardwareTimeStamp{0};
};

// Tracking actor - templated on completion queue type
template <typename TCompletionQueue>
class Track_Tracking {
public:
    Track_Tracking(LabelAllocator* allocator,
                   TransactionManager* txnMgr,
                   TCompletionQueue& completionQueue,
                   Engine::ContextManager* contextManager = nullptr)
        : labelAllocator_(allocator),
          txnMgr_(txnMgr),
          completionQueue_(completionQueue),
          contextManager_(contextManager),
          lock_(nullptr),
          payloads_(std::make_unique<PayloadRegistry>()),
          // Phase 2.0: Transaction infrastructure (created below after validation)
          txnHandler_(nullptr)
    {
        lock_ = ::IOLockAlloc();
        if (lock_ == nullptr) {
            // Log error but don't crash - caller should check via RegisterTx return value
            ASFW_LOG(Async, "ERROR: Track_Tracking: IOLockAlloc failed!");
        }

        if (!txnMgr_) {
            ASFW_LOG(Async, "ERROR: Track_Tracking: TransactionManager required!");
        } else {
            ASFW_LOG(Async, "✅ Track_Tracking: Transaction-only mode (Phase 2.0)");
        }

        // Create TransactionCompletionHandler with both txnMgr and labelAllocator
        txnHandler_ = std::make_unique<TransactionCompletionHandler>(txnMgr, allocator);
    }

    ~Track_Tracking() {
        if (lock_) {
            ::IOLockFree(lock_);
        }
    }

    [[nodiscard]] AsyncHandle RegisterTx(const TxMetadata& meta) {
        if (!labelAllocator_ || !txnMgr_ || !lock_) {
            return AsyncHandle{0};
        }

        ::IOLockLock(lock_);

        // If no transactions are in flight but the bitmap isn't empty, reset it
        // to avoid stale bits pinning allocation (observed stuck tLabel).
        if (txnMgr_->Count() == 0 && labelAllocator_->IsLabelInUse(0 /*ignored, uses bitmap*/)) {
            ASFW_LOG(Async, "Label bitmap non-empty with zero transactions; resetting allocator");
            labelAllocator_->Reset();
        }

        // Allocate a free label from the bitmap allocator to avoid collisions
        uint8_t label = labelAllocator_->Allocate();
        if (label == LabelAllocator::kInvalidLabel) {
            ::IOLockUnlock(lock_);
            ASFW_LOG(Async, "ERROR: RegisterTx failed - no available tLabels");
            return AsyncHandle{0};
        }

        // Phase 2.0: tLabel is the identifier (matches Apple's pattern)
        // No need for synthetic txid

        // Allocate Transaction (sole source of truth)
        auto result = txnMgr_->Allocate(
            TLabel{label},
            BusGeneration{meta.generation},
            NodeID{meta.destinationNodeID}
        );

        if (!result) {
            labelAllocator_->Free(label);
            ::IOLockUnlock(lock_);
            // Phase 2.1: Log error with rich context (file, line, function, message)
            result.error().Log();
            return AsyncHandle{0};
        }

        Transaction* txn = *result;

        ASFW_LOG_V3(Async, "🔍 [RegisterTx] Allocated Transaction: txn=%p tLabel=%u",
                    txn, label);

        // Set transaction parameters
        txn->SetTimeout(200);  // TODO: Get from config or meta
        txn->SetTCode(meta.tCode);  // Store tCode for IsReadOperation() check
        txn->SetCompletionStrategy(meta.completionStrategy);

        // EXPLICIT: Mark read operations to skip AT completion
        if (meta.completionStrategy == CompletionStrategy::CompleteOnAR) {
            txn->SetSkipATCompletion(true);
            ASFW_LOG_V3(Async, "🔍 [RegisterTx] Read operation: will skip AT completion, strategy=%{public}s",
                        ToString(meta.completionStrategy));
        }

        ASFW_LOG_V3(Async, "🔍 [RegisterTx] meta.callback valid=%d for tLabel=%u",
                    meta.callback ? 1 : 0, label);

        // Set response handler (wraps meta.callback)
        txn->SetResponseHandler([callback = meta.callback, label]
                                (kern_return_t kr, std::span<const uint8_t> data) {
            ASFW_LOG_V3(Async, "🔍 [Wrapper Lambda] ENTRY: tLabel=%u callback=%p valid=%d kr=0x%x",
                        label, &callback, callback ? 1 : 0, kr);
            if (callback) {
                // Convert kern_return_t to AsyncStatus for Phase 2.3 callback
                AsyncStatus status = (kr == kIOReturnSuccess) ? AsyncStatus::kSuccess :
                                    (kr == kIOReturnTimeout) ? AsyncStatus::kTimeout :
                                    AsyncStatus::kHardwareError;
                // Phase 2.3: CompletionCallback now takes (handle, status, span)
                // Encode handle as (label + 1) to ensure handle is never 0
                ASFW_LOG_V3(Async, "🔍 [Wrapper Lambda] About to invoke callback: handle=%u status=%u",
                            static_cast<uint32_t>(label) + 1, static_cast<uint32_t>(status));
                callback(AsyncHandle{static_cast<uint32_t>(label) + 1}, status, data);
                ASFW_LOG_V3(Async, "🔍 [Wrapper Lambda] Callback returned");
            } else {
                ASFW_LOG(Async, "⚠️ [Wrapper Lambda] callback is NULL!");
            }
        });

        // Transition to Submitted state (Created → Submitted)
        txn->TransitionTo(TransactionState::Submitted, "RegisterTx");

        ASFW_LOG_V2(Async,
                    "✅ RegisterTx: Created txn (tLabel=%u gen=%u nodeID=0x%04X tCode=0x%02X)",
                    label, meta.generation, meta.destinationNodeID, meta.tCode);

        ::IOLockUnlock(lock_);

        // Return AsyncHandle encoded as (label + 1) to ensure handle is never 0
        // This allows tLabel 0-63 to map to handles 1-64, avoiding handle=0 sentinel
        return AsyncHandle{static_cast<uint32_t>(label) + 1};
    }

    [[nodiscard]] std::optional<uint8_t> GetLabelFromHandle(AsyncHandle handle) const {
        if (!txnMgr_ || !lock_) {
            return std::nullopt;
        }

        // Decode handle back to label: handle = label + 1, so label = handle - 1
        // Handles are 1-64, labels are 0-63
        if (handle.value == 0 || handle.value > 64) {
            return std::nullopt;  // Invalid handle
        }
        uint8_t label = static_cast<uint8_t>(handle.value - 1);
        if (label >= 64) {
            return std::nullopt;
        }
        
        Transaction* txn = txnMgr_->Find(TLabel{label});
        if (!txn) {
            return std::nullopt;
        }

        return txn->label().value;
    }

    void OnTxPosted(AsyncHandle handle, uint64_t nowUsec, uint64_t timeoutUsec) {
        if (!txnMgr_ || !lock_) {
            return;
        }

        // Decode handle back to label: handle = label + 1
        if (handle.value == 0 || handle.value > 64) {
            return;  // Invalid handle
        }
        uint8_t label = static_cast<uint8_t>(handle.value - 1);

        bool found = txnMgr_->WithTransaction(TLabel{label}, [&](Transaction* txn) {
            // Transition to ATPosted state
            txn->TransitionTo(TransactionState::ATPosted, "OnTxPosted");

            // EXPLICIT: Read operations bypass AT completion (go straight to AwaitingAR)
            // This matches Apple's IOFWReadQuadCommand gotAck() pattern:
            // - gotAck() stores ackCode but doesn't complete
            // - gotPacket() completes the command with response data
            if (txn->GetCompletionStrategy() == CompletionStrategy::CompleteOnAR) {
                txn->TransitionTo(TransactionState::ATCompleted, "OnTxPosted: CompleteOnAR bypass");
                txn->TransitionTo(TransactionState::AwaitingAR, "OnTxPosted: CompleteOnAR bypass");
                ASFW_LOG_V3(Async, "  📤 Read operation: bypassing AT completion, going to AwaitingAR");
            }

            // Set deadline for timeout
            txn->SetDeadline(nowUsec + timeoutUsec);

            ASFW_LOG_V3(Async,
                        "📤 OnTxPosted: tLabel=%u deadline=%llu state=%{public}s strategy=%{public}s",
                        txn->label().value,
                        static_cast<unsigned long long>(nowUsec + timeoutUsec),
                        ToString(txn->state()),
                        ToString(txn->GetCompletionStrategy()));
        });

        if (!found) {
            ASFW_LOG(Async, "⚠️  OnTxPosted: Transaction tLabel=%u not found", label);
        }
    }

    // AR Response Reception - FINAL TRANSACTION STATE
    // Per Apple IOFWAsyncCommand::gotPacket() and Linux close_transaction() (core-transaction.c:60-70):
    // Response packet arrival is the definitive completion event that overrides AT completion status.
    // Even if AT reported eventCode 0x10 or other errors, successful AR response means transaction succeeded.
    // This matches FireWire spec: split transactions complete on response, not on request ack.
    void OnRxResponse(const RxResponse& response) {
        ASFW_LOG(Async, "📥 OnRxResponse: tLabel=%u gen=%u tCode=0x%X rCode=0x%X event=0x%02X len=%zu ts=0x%04X",
                 response.tLabel, response.generation, response.tCode, response.rCode,
                 static_cast<uint8_t>(response.eventCode), response.payload.size(), response.hardwareTimeStamp);

        if (!txnHandler_ || !txnMgr_ || !lock_) {
            return;
        }

        // Phase 2.0: Transaction-only path
        MatchKey key{
            .node = NodeID{response.sourceNodeID},
            .generation = BusGeneration{response.generation},
            .label = TLabel{response.tLabel}
        };

        txnHandler_->OnARResponse(key, response.rCode, response.payload);
    }


    void OnTimeoutTick(uint64_t nowUsec) {
        if (!txnMgr_ || !txnHandler_) {
            return;
        }

        // Phase 2.0: Check all transactions for timeout
        // TODO: Optimize with priority queue/timer wheel if performance becomes issue
        std::vector<TLabel> timedOutLabels;

        // Collect timed-out transactions
        txnMgr_->ForEachTransaction([&](Transaction* txn) {
            if (!txn) return;

            // Skip transactions in terminal states (already completed/failed/cancelled/timed out)
            TransactionState state = txn->state();
            if (state == TransactionState::Completed ||
                state == TransactionState::Failed ||
                state == TransactionState::Cancelled ||
                state == TransactionState::TimedOut) {
                return;  // Don't check deadline for terminal states
            }

            uint64_t deadline = txn->deadlineUs();
            if (deadline > 0 && nowUsec >= deadline) {
                // Transaction has timed out
                timedOutLabels.push_back(txn->label());

                ASFW_LOG_V2(Async,
                            "⏱️ Timeout: tLabel=%u state=%{public}s deadline=%llu now=%llu",
                            txn->label().value, ToString(txn->state()),
                            static_cast<unsigned long long>(deadline),
                            static_cast<unsigned long long>(nowUsec));
            }
        });

        // Handle timeouts outside iteration (avoid modifying during iteration)
        for (TLabel label : timedOutLabels) {
            txnHandler_->OnTimeout(label);
        }
    }

    void CancelByGeneration(uint16_t oldGeneration) {
        if (!txnMgr_) {
            return;
        }

        // Phase 2.0: Cancel all transactions with old generation and FREE their labels.
        // Previously we transitioned transactions to Cancelled but left them in the manager,
        // which leaked label allocations across bus resets. That forced subsequent requests
        // to reuse a single label (e.g. tLabel=3 forever). Extract and free here to release
        // the bitmap slots.
        ASFW_LOG(Async, "🔄 CancelByGeneration: gen=%u (will extract and free labels)", oldGeneration);

        // Collect labels to cancel (avoid modifying during iteration)
        std::vector<TLabel> victims;
        txnMgr_->ForEachTransaction([&](Transaction* txn) {
            if (!txn) return;
            if (txn->generation().value == oldGeneration) {
                victims.push_back(txn->label());
            }
        });

        // Cancel collected transactions
        for (TLabel label : victims) {
            auto txnPtr = txnMgr_->Extract(label);
            if (!txnPtr) {
                continue;
            }

            if (!IsTerminalState(txnPtr->state())) {
                txnPtr->TransitionTo(TransactionState::Cancelled, "CancelByGeneration");
                txnPtr->InvokeResponseHandler(kIOReturnAborted, {});
            }

            // Free the label so subsequent transactions can rotate through all 0-63 slots.
            if (labelAllocator_) {
                labelAllocator_->Free(label.value);
            }
        }

        ASFW_LOG(Async, "✅ CancelByGeneration: Cancelled %zu transactions", victims.size());
    }

    // Cancel ALL transactions regardless of generation and free labels.
    void CancelAllAndFreeLabels() {
        if (!txnMgr_) {
            return;
        }

        std::vector<TLabel> victims;
        txnMgr_->ForEachTransaction([&](Transaction* txn) {
            if (!txn) return;
            victims.push_back(txn->label());
        });

        for (TLabel label : victims) {
            auto txnPtr = txnMgr_->Extract(label);
            if (!txnPtr) {
                continue;
            }

            if (!IsTerminalState(txnPtr->state())) {
                txnPtr->TransitionTo(TransactionState::Cancelled, "CancelAll");
                txnPtr->InvokeResponseHandler(kIOReturnAborted, {});
            }

            if (labelAllocator_) {
                labelAllocator_->Free(label.value);
            }
        }

        ASFW_LOG(Async, "✅ CancelAllAndFreeLabels: cancelled %zu transactions", victims.size());
    }

    LabelAllocator* GetLabelAllocator() const { return labelAllocator_; }

    void OnTxCompletion(const TxCompletion& completion) {
        if (!txnHandler_) {
            return;
        }

        // Phase 2.0: Transaction-only path
        txnHandler_->OnATCompletion(completion);
    }

    // Accessors
    TransactionManager* GetTransactionManager() const { return txnMgr_; }  // Phase 2.0
    // Payload registry access (owned by tracking actor)
    PayloadRegistry* Payloads() const { return payloads_.get(); }
    
    // Context manager access (for AR-side stop behavior)
    void SetContextManager(Engine::ContextManager* ctxMgr) { contextManager_ = ctxMgr; }

private:
    // Phase 2.0: All legacy Phase 1.2 helper functions removed
    // (TracePayload, TCodeExpectsResponse, CompletionDispatch, DetachAndBuildDispatch_, DispatchCompletion_)
    // Transaction-only architecture uses TransactionCompletionHandler instead

    // Components
    LabelAllocator* labelAllocator_;
    TransactionManager* txnMgr_;  // Phase 2.0: Required (sole source of truth)
    TCompletionQueue& completionQueue_;
    Engine::ContextManager* contextManager_;  // For AR-side stop on empty (wired in Start)
    IOLock* lock_;
    std::unique_ptr<PayloadRegistry> payloads_;

    // Phase 2.0: Transaction infrastructure (required)
    std::unique_ptr<TransactionCompletionHandler> txnHandler_;
};

} // namespace ASFW::Async
