#pragma once

#include "../Core/Transaction.hpp"
#include "../Core/TransactionManager.hpp"
#include "TxCompletion.hpp"
#include "LabelAllocator.hpp"
#include "../Engine/ATTrace.hpp"  // For NowUs()
#include "../../Logging/Logging.hpp"

#include <DriverKit/IOReturn.h>
#include <span>

namespace ASFW::Async {

/**
 * \brief Integration layer between OHCI completion events and Transaction state machine.
 *
 * This class bridges the gap between:
 * - Low-level OHCI driver (ATContextBase::ScanCompletion)
 * - High-level Transaction state machine (Transaction/TransactionManager)
 *
 * \par Design
 * Implements the two-path completion model from Apple's IOFWCommand:
 * - OnATCompletion(): Called when AT descriptor completes (gotAck equivalent)
 * - OnARResponse(): Called when AR response arrives (gotPacket equivalent)
 *
 * \par Migration Path
 * Phase 1.1: Runs in parallel with existing Tracking::OnTxCompletion
 * Phase 1.2: Replaces Tracking::OnTxCompletion entirely
 */
class TransactionCompletionHandler {
public:
    explicit TransactionCompletionHandler(TransactionManager* txnMgr, LabelAllocator* labelAllocator) noexcept
        : txnMgr_(txnMgr), labelAllocator_(labelAllocator), pendingLabelFree_(0xFF) {}

    /**
     * \brief Handle AT descriptor completion (gotAck equivalent).
     *
     * Called from ATContextBase::ScanCompletion when AT descriptor completes.
     * Extracts ACK code from xferStatus and transitions transaction state.
     *
     * \param comp TxCompletion from OHCI driver
     *
     * \par State Transitions
     * - ackCode==0x1 (pending): ATCompleted → AwaitingAR (wait for AR response)
     * - ackCode==0x0 (complete): ATCompleted → Completed (immediate completion)
     * - ackCode==0x4-0x6 (busy): ATCompleted (stay, timeout will retry)
     * - ackCode==0xF (timeout): ATCompleted → Failed
     * - eventCode errors: ATCompleted → Failed
     *
     * \par Critical Logic
     * Per IEEE 1394-1995 section 6.2.4.3, ACK codes determine transaction flow:
     * - ack_pending (0x1): Split transaction, wait for response packet
     * - ack_complete (0x0): Unified transaction, done immediately
     * - ack_busy_X/A/B (0x4-0x6): Retry after backoff
     */
    void OnATCompletion(const TxCompletion& comp) noexcept {
        if (!txnMgr_) {
            return;
        }

        uint8_t eventCode = static_cast<uint8_t>(comp.eventCode);
        uint8_t ackCode = comp.ackCode;

        ASFW_LOG(Async,
                 "🔄 OnATCompletion: tLabel=%u ackCode=0x%X eventCode=0x%02X ts=%u ackCount=%u",
                 comp.tLabel, ackCode, eventCode, comp.timeStamp, comp.ackCount);

        // CRITICAL: Reset pending label free before lambda (prevents stale value from previous completion)
        pendingLabelFree_ = 0xFF;

        // Find transaction by tLabel
        bool found = txnMgr_->WithTransactionByLabel(TLabel{comp.tLabel}, [&](Transaction* txn) {
            // Store ACK code in transaction for timeout handler
            txn->SetAckCode(ackCode);

            // CRITICAL FIX: For READ operations that were already transitioned to AwaitingAR in OnTxPosted,
            // skip AT completion processing entirely. Transaction is already in correct state.
            if (txn->ShouldSkipATCompletion()) {
                ASFW_LOG(Async, "  ⏭️  OnATCompletion: Skipping (CompleteOnAR, already in %{public}s)",
                         ToString(txn->state()));
                return;  // Transaction already in AwaitingAR from OnTxPosted
            }

            // Legacy fallback: For READ operations detected by tCode (shouldn't happen with new code)
            // This path exists for backward compatibility if metadata doesn't set completionStrategy.
            if (txn->IsReadOperation() && txn->state() != TransactionState::AwaitingAR) {
                ASFW_LOG(Async, "  → AwaitingAR (read operation, legacy fallback path)");
                txn->TransitionTo(TransactionState::ATCompleted, "OnATCompletion: read_legacy");
                txn->TransitionTo(TransactionState::AwaitingAR, "OnATCompletion: read_legacy");
                return;  // Don't process ack code for reads
            }

            // Check for hardware error events FIRST (these override ACK codes)
            // eventCode 0x0A = evt_timeout, 0x03 = evt_missing_ack
            if (eventCode == 0x0A || eventCode == 0x03) {
                // Hardware timeout - but ACK code tells us what actually happened
                // If ackCode is 0x1 (pending), the AT completed but we're waiting for AR
                // If ackCode is 0xF or invalid, the transmission truly failed
                ASFW_LOG(Async, "  → Hardware event: %{public}s (ackCode=0x%X)",
                         ToString(comp.eventCode), ackCode);

                if (ackCode == 0x1) {
                    // ack_pending: AT transmission succeeded, wait for AR response
                    ASFW_LOG(Async, "  → AwaitingAR (ackPending despite hw timeout)");
                    txn->TransitionTo(TransactionState::ATCompleted, "OnATCompletion: hw_timeout_pending");
                    txn->TransitionTo(TransactionState::AwaitingAR, "OnATCompletion: ackPending");
                    return;
                } else {
                    // True hardware failure
                    ASFW_LOG(Async, "  → Failed (hw timeout, ackCode=0x%X)", ackCode);
                    txn->TransitionTo(TransactionState::ATCompleted, "OnATCompletion: hw_timeout");
                    txn->TransitionTo(TransactionState::Failed, "OnATCompletion: hw_timeout");
                    CompleteTransaction_(txn, kIOReturnTimeout, {});
                    return;
                }
            }

            // Other hardware errors: fail immediately
            if (eventCode == static_cast<uint8_t>(OHCIEventCode::kEvtFlushed)) {
                ASFW_LOG(Async, "  → Cancelled (flushed)");
                txn->TransitionTo(TransactionState::Cancelled, "OnATCompletion: flushed");
                CompleteTransaction_(txn, kIOReturnAborted, {});
                return;
            }

            // Now handle ACK code (IEEE 1394 acknowledgment from target device)
            // Per COMPLETION_ARCHITECTURE.md and IEEE 1394-1995 section 6.2.4.3
            switch (ackCode) {
                case 0x1:  // kFWAckPending (split transaction)
                    ASFW_LOG(Async, "  → AwaitingAR (ackPending, need AR response)");
                    txn->TransitionTo(TransactionState::ATCompleted, "OnATCompletion: ackPending");
                    txn->TransitionTo(TransactionState::AwaitingAR, "OnATCompletion: ackPending");
                    // Keep transaction alive, wait for AR response
                    break;

                case 0x0:  // kFWAckComplete (unified transaction)
                    ASFW_LOG(Async, "  → Completed (ackComplete, immediate)");
                    txn->TransitionTo(TransactionState::ATCompleted, "OnATCompletion: ackComplete");
                    CompleteTransaction_(txn, kIOReturnSuccess, {});
                    break;

                case 0x4:  // kFWAckBusyX
                case 0x5:  // kFWAckBusyA
                case 0x6:  // kFWAckBusyB
                    ASFW_LOG(Async, "  → Busy (0x%X), extending deadline for retry", ackCode);
                    txn->TransitionTo(TransactionState::ATCompleted, "OnATCompletion: busy");

                    // Phase 2: Extend deadline immediately to prevent rapid timeout
                    // Device is busy, give it time to recover (200ms) before checking again
                    txn->SetDeadline(Engine::NowUs() + 200000);  // +200ms from now

                    // Stay in ATCompleted, timeout handler will retry if still busy
                    break;

                case 0xC:  // kFWAckTardy (CRITICAL FIX!)
                case 0x11: // Missing ACK after multiple retries
                case 0x1B: // Hardware-level tardy indication
                    // CRITICAL FIX: ack_tardy means the device acknowledged receipt but is slow to respond.
                    // Per Apple's IOFWAsyncCommand::gotAck(), we should NOT fail here - wait for AR response.
                    // The AT element completed successfully (packet was transmitted), now wait for the
                    // response packet to arrive via AR path.
                    ASFW_LOG(Async, "  → AwaitingAR (ackCode=0x%X tardy/slow, wait for response)", ackCode);
                    txn->TransitionTo(TransactionState::ATCompleted, "OnATCompletion: tardy");
                    txn->TransitionTo(TransactionState::AwaitingAR, "OnATCompletion: tardy");
                    // Keep transaction alive, wait for AR response (don't fail!)
                    break;

                case 0xD:  // kFWAckDataError
                case 0xE:  // kFWAckTypeError
                    ASFW_LOG(Async, "  → Failed (ackError 0x%X)", ackCode);
                    txn->TransitionTo(TransactionState::ATCompleted, "OnATCompletion: ackError");
                    txn->TransitionTo(TransactionState::Failed, "OnATCompletion: ackError");
                    CompleteTransaction_(txn, kIOReturnError, {});
                    break;

                default:
                    ASFW_LOG(Async, "  → Unknown ackCode=0x%X, treating as tardy (wait for AR)", ackCode);
                    // CRITICAL FIX: Unknown ACKs should wait for AR response, not fail immediately.
                    // Per Apple's split-transaction model, only explicit errors (0xD, 0xE) should fail.
                    // Everything else might still result in a valid AR response.
                    txn->TransitionTo(TransactionState::ATCompleted, "OnATCompletion: unknown_ack");
                    txn->TransitionTo(TransactionState::AwaitingAR, "OnATCompletion: unknown_ack");
                    break;
            }
        });

        // CRITICAL: Free label AFTER WithTransactionByLabel completes (prevents early reuse)
        // This ensures label is not reused until transaction is fully processed
        if (pendingLabelFree_ != 0xFF && labelAllocator_) {
            labelAllocator_->Free(pendingLabelFree_);
            ASFW_LOG(Async, "  🔓 Freed label=%u (after lambda completion)", pendingLabelFree_);
            pendingLabelFree_ = 0xFF;
        }

        if (!found) {
            ASFW_LOG(Async, "⚠️  OnATCompletion: No transaction for tLabel=%u", comp.tLabel);
        }
    }

    /**
     * \brief Handle AR response reception (gotPacket equivalent).
     *
     * Called from ResponseMatcher when AR response packet arrives.
     *
     * \param key Match key (nodeID + generation + tLabel)
     * \param rcode Response code (0x0 = success, others = error)
     * \param data Response payload
     *
     * \par State Transitions
     * - AwaitingAR → ARReceived → Completed (normal path)
     * - If not in AwaitingAR state: log warning and ignore (stale/duplicate response)
     *
     * \par Critical Logic
     * Per IEEE 1394-1995 section 6.2.4.4, response packet arrival is the
     * definitive completion event. Even if AT completion reported errors,
     * successful AR response means transaction succeeded.
     */
    void OnARResponse(const MatchKey& key, uint8_t rcode, std::span<const uint8_t> data) noexcept {
        if (!txnMgr_) {
            return;
        }

        ASFW_LOG(Async,
                 "📥 OnARResponse: tLabel=%u nodeID=0x%04X gen=%u rcode=0x%X len=%zu",
                 key.label.value, key.node.value, key.generation.value, rcode, data.size());

        // CRITICAL: Reset pending label free before processing
        pendingLabelFree_ = 0xFF;

        Transaction* txn = txnMgr_->FindByMatchKey(key);
        if (!txn) {
            ASFW_LOG(Async, "⚠️  OnARResponse: No transaction for key");
            return;
        }

        // Verify we're in correct state
        if (txn->state() != TransactionState::AwaitingAR) {
            ASFW_LOG(Async,
                     "⚠️  OnARResponse: Unexpected state %{public}s (expected AwaitingAR)",
                     ToString(txn->state()));
            // This could be a duplicate/stale response - ignore it
            return;
        }

        // Transition: AwaitingAR → ARReceived
        txn->TransitionTo(TransactionState::ARReceived, "OnARResponse");

        // Convert rcode to kern_return_t
        kern_return_t kr = (rcode == 0) ? kIOReturnSuccess : kIOReturnError;

        // Complete transaction
        ASFW_LOG(Async, "  → Completed (rcode=0x%X, kr=0x%08X)", rcode, kr);
        CompleteTransaction_(txn, kr, data);

        // CRITICAL: Free label AFTER transaction completion (prevents early reuse)
        if (pendingLabelFree_ != 0xFF && labelAllocator_) {
            labelAllocator_->Free(pendingLabelFree_);
            ASFW_LOG(Async, "  🔓 Freed label=%u (after AR completion)", pendingLabelFree_);
            pendingLabelFree_ = 0xFF;
        }
    }

    /**
     * \brief Handle timeout expiration.
     *
     * Called from TimeoutEngine::Tick when transaction times out.
     *
     * \param label tLabel of transaction that timed out
     *
     * \par Smart Retry Logic
     * - If ackCode is busy (0x4-0x6) and retries remain: resubmit
     * - If in AwaitingAR with ackPending (0x1): might be spurious timeout
     * - Otherwise: complete with timeout error
     */
    void OnTimeout(TLabel label) noexcept {
        if (!txnMgr_) {
            return;
        }

        // CRITICAL: Reset pending label free before lambda
        pendingLabelFree_ = 0xFF;

        bool found = txnMgr_->WithTransaction(label, [&](Transaction* txn) {
            uint8_t ackCode = txn->ackCode();
            TransactionState state = txn->state();

            ASFW_LOG(Async,
                     "⏱️ OnTimeout: tLabel=%u state=%{public}s ackCode=0x%X retries=%u",
                     txn->label().value, ToString(state), ackCode, txn->retryCount());

            // Smart retry based on ACK code and state
            if (ackCode == 0x4 || ackCode == 0x5 || ackCode == 0x6) {  // Busy
                const uint8_t kMaxBusyRetries = 3;
                if (txn->retryCount() < kMaxBusyRetries) {
                    txn->IncrementRetry();

                    // Extend deadline to allow device time to become non-busy
                    // Phase 2: Simple deadline extension (prevents rapid retimeout)
                    // Device returned busy ACK, give it 200ms to recover
                    const uint64_t newDeadline = Engine::NowUs() + 200000;  // +200ms
                    txn->SetDeadline(newDeadline);

                    ASFW_LOG(Async, "  → Retry (busy, attempt %u/%u) - deadline extended by 200ms",
                             txn->retryCount(), kMaxBusyRetries);

                    // Don't complete transaction - let timeout engine check again at new deadline
                    // If device becomes non-busy and sends AR response, OnARResponse will complete it
                    // If still busy after 3 retries, next timeout will fail the transaction
                    return;
                }
            }

            // Check for spurious timeout while waiting for AR response
            if (state == TransactionState::AwaitingAR && ackCode == 0x1) {
                ASFW_LOG(Async,
                         "  → Timeout while AwaitingAR (ackPending), might be spurious");
                // Could extend deadline here, but for now just fail
            }

            // Timeout is terminal
            txn->TransitionTo(TransactionState::TimedOut, "OnTimeout");
            CompleteTransaction_(txn, kIOReturnTimeout, {});
        });

        // CRITICAL: Free label AFTER WithTransaction completes (prevents early reuse)
        if (pendingLabelFree_ != 0xFF && labelAllocator_) {
            labelAllocator_->Free(pendingLabelFree_);
            ASFW_LOG(Async, "  🔓 Freed label=%u (after timeout)", pendingLabelFree_);
            pendingLabelFree_ = 0xFF;
        }

        if (!found) {
            ASFW_LOG(Async, "⚠️  OnTimeout: No transaction for tLabel=%u", label.value);
        }
    }

private:
    /**
     * \brief Complete transaction and invoke callback.
     *
     * \param txn Transaction to complete
     * \param kr Result code
     * \param data Response payload
     *
     * \par Lifecycle
     * - Transitions to Completed state
     * - Invokes response handler callback
     * - Removes transaction from manager (frees resources)
     */
    void CompleteTransaction_(Transaction* txn, kern_return_t kr, std::span<const uint8_t> data) noexcept {
        if (!txn) {
            return;
        }

        // Transition to Completed (if not already in terminal state)
        if (txn->state() != TransactionState::Completed &&
            txn->state() != TransactionState::Failed &&
            txn->state() != TransactionState::Cancelled &&
            txn->state() != TransactionState::TimedOut) {
            txn->TransitionTo(TransactionState::Completed, "CompleteTransaction");
        }

        // Invoke callback
        ASFW_LOG(Async, "🔍 [CompleteTransaction_] About to invoke: txn=%p tLabel=%u kr=0x%x",
                 txn, txn->label().value, kr);
        txn->InvokeResponseHandler(kr, data);

        // CRITICAL FIX (Issue #3): Defer label freeing until AFTER WithTransactionByLabel completes
        //
        // Previous bug: Freed label immediately, allowing reuse before labelToTxid_ was cleared
        // Apple's behavior: Ties label lifetime to command, frees atomically with command teardown
        //
        // Fix: Store label for deferred freeing. The caller (OnATCompletion/OnARResponse/OnTimeout)
        // will free it AFTER the WithTransactionByLabel/WithTransaction lambda exits, preventing
        // early label reuse and labelToTxid_ mismatch races.
        //
        // The Transaction stays in the manager for debugging/history and will be cleaned up on
        // bus reset (CancelAll) or when explicitly removed.
        pendingLabelFree_ = txn->label().value;
        ASFW_LOG(Async, "  📋 Marked tLabel=%u for deferred free (txn still in manager)",
                 txn->label().value);
    }

    TransactionManager* txnMgr_;
    LabelAllocator* labelAllocator_;
    uint8_t pendingLabelFree_;  // Label to free after lambda completes (0xFF = none)
};

} // namespace ASFW::Async
