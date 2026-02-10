#pragma once

#include "../Core/Transaction.hpp"
#include "../Core/TransactionManager.hpp"
#include "TxCompletion.hpp"
#include "LabelAllocator.hpp"
#include "../Engine/ATTrace.hpp"  // For NowUs()
#include "../../Logging/Logging.hpp"
#include "../../Logging/LogConfig.hpp"

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
        : txnMgr_(txnMgr), labelAllocator_(labelAllocator) {}

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

        // AT Response context completions correspond to WrResp acks we send back
        // to devices. They are not tracked as transactions; skip quietly.
        if (comp.isResponseContext) {
            ASFW_LOG_V3(Async, "OnATCompletion: Ignoring AT Response completion (tLabel=%u)", comp.tLabel);
            return;
        }

        enum class PostAction {
            kNone,
            kCompleteSuccess,
            kCompleteError,
            kCompleteTimeout,
            kCompleteCancelled,
            kCompletePhySuccess,
        };

        PostAction postAction = PostAction::kNone;
        const char* transitionTag1 = nullptr;
        const char* transitionTag2 = nullptr;
        kern_return_t postKr = kIOReturnSuccess;

        uint8_t eventCode = static_cast<uint8_t>(comp.eventCode);
        uint8_t ackCode = comp.ackCode;

        ASFW_LOG_V2(Async,
                    "🔄 OnATCompletion: tLabel=%u ack=0x%X event=0x%02X ts=%u ackCount=%u",
                    comp.tLabel, ackCode, eventCode, comp.timeStamp, comp.ackCount);



        // Find transaction by tLabel
        bool found = txnMgr_->WithTransactionByLabel(TLabel{comp.tLabel}, [&](Transaction* txn) {
            const auto state = txn->state();
            if (state == TransactionState::Completed ||
                state == TransactionState::TimedOut ||
                state == TransactionState::Failed ||
                state == TransactionState::Cancelled) {
                ASFW_LOG(Async, "  ⏭️  OnATCompletion: Transaction already terminal (%{public}s), ignoring",
                         ToString(state));
                return;
            }

            // Store ACK code in transaction for timeout handler
            txn->SetAckCode(ackCode);

            // PHY packets complete on AT path only (no AR response expected).
            if (txn->GetCompletionStrategy() == CompletionStrategy::CompleteOnPHY) {
                ASFW_LOG(Async, "  → Completed (PHY, AT-only) ackCode=0x%X event=0x%02X", ackCode, eventCode);
                postAction = PostAction::kCompletePhySuccess;
                transitionTag1 = "OnATCompletion: phy";
                transitionTag2 = "OnATCompletion: phy";
                return;
            }

            // CRITICAL FIX: For READ operations that were already transitioned to AwaitingAR in OnTxPosted,
            // skip AT completion processing entirely. Transaction is already in correct state.
            if (txn->ShouldSkipATCompletion()) {
                ASFW_LOG_V3(Async, "  ⏭️  OnATCompletion: Skipping (CompleteOnAR, already in %{public}s)",
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
                    
                    // Extract transaction to complete it safely outside lock
                    postAction = PostAction::kCompleteTimeout;
                    transitionTag1 = "OnATCompletion: hw_timeout";
                    transitionTag2 = "OnATCompletion: hw_timeout";
                    postKr = kIOReturnTimeout;
                    return;
                }
            }

            // Other hardware errors: fail immediately
            if (eventCode == static_cast<uint8_t>(OHCIEventCode::kEvtFlushed)) {
                ASFW_LOG(Async, "  → Cancelled (flushed)");
                
                // Extract transaction to complete it safely outside lock
                postAction = PostAction::kCompleteCancelled;
                transitionTag1 = "OnATCompletion: flushed";
                postKr = kIOReturnAborted;
                return;
            }

            // Now handle ACK code (IEEE 1394 acknowledgment from target device)
            // Per COMPLETION_ARCHITECTURE.md and IEEE 1394-1995 section 6.2.4.3
            const auto strategy = txn->GetCompletionStrategy();
            const bool needsARData = txn->IsReadOperation() || strategy == CompletionStrategy::CompleteOnAR;

            switch (ackCode) {
                case 0x1:  // kFWAckPending (split transaction)
                    ASFW_LOG_V2(Async, "  → AwaitingAR (ackPending, need AR response)");
                    txn->TransitionTo(TransactionState::ATCompleted, "OnATCompletion: ackPending");
                    txn->TransitionTo(TransactionState::AwaitingAR, "OnATCompletion: ackPending");
                    // Keep transaction alive, wait for AR response
                    break;

                case 0x0:  // kFWAckComplete (unified transaction)
                    if (needsARData) {
                        ASFW_LOG_V2(Async, "  → AwaitingAR (ackComplete but data required)");
                        txn->TransitionTo(TransactionState::ATCompleted, "OnATCompletion: ackComplete_read");
                        txn->TransitionTo(TransactionState::AwaitingAR, "OnATCompletion: ackComplete_read");
                        break;
                    }
                    // Only complete if AR hasn't already won the race
                    if (txn->TryMarkCompleted()) {
                        ASFW_LOG_V1(Async, "  → Completed (ackComplete, AT path won)");
                        postAction = PostAction::kCompleteSuccess;
                        transitionTag1 = "OnATCompletion: ackComplete";
                        transitionTag2 = "OnATCompletion: ackComplete";
                    } else {
                        ASFW_LOG_V3(Async, "  → ackComplete but AR already completed, ignoring");
                    }
                    break;

                case 0x4:  // kFWAckBusyX
                case 0x5:  // kFWAckBusyA
                case 0x6:  // kFWAckBusyB
                    ASFW_LOG_V2(Async, "  → Busy (0x%X), extending deadline for retry", ackCode);
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
                    ASFW_LOG_V2(Async, "  → AwaitingAR (ackCode=0x%X tardy/slow, wait for response)", ackCode);
                    txn->TransitionTo(TransactionState::ATCompleted, "OnATCompletion: tardy");
                    txn->TransitionTo(TransactionState::AwaitingAR, "OnATCompletion: tardy");
                    // Keep transaction alive, wait for AR response (don't fail!)
                    break;

                case 0xD:  // kFWAckDataError
                case 0xE:  // kFWAckTypeError
                    ASFW_LOG_V1(Async, "  → Failed (ackError 0x%X)", ackCode);
                    
                    postAction = PostAction::kCompleteError;
                    transitionTag1 = "OnATCompletion: ackError";
                    transitionTag2 = "OnATCompletion: ackError";
                    postKr = kIOReturnError;
                    break;

                default:
                    ASFW_LOG_V2(Async, "  → Unknown ackCode=0x%X, treating as tardy (wait for AR)", ackCode);
                    // CRITICAL FIX: Unknown ACKs should wait for AR response, not fail immediately.
                    // Per Apple's split-transaction model, only explicit errors (0xD, 0xE) should fail.
                    // Everything else might still result in a valid AR response.
                    txn->TransitionTo(TransactionState::ATCompleted, "OnATCompletion: unknown_ack");
                    txn->TransitionTo(TransactionState::AwaitingAR, "OnATCompletion: unknown_ack");
                    break;
            }
        });

        if (!found) {
            // Expected for split transactions: AR response completed the transaction
            // before AT completion interrupt arrived. This is a benign race.
            ASFW_LOG_V3(Async, "OnATCompletion: Transaction already completed for tLabel=%u (AR won race)", comp.tLabel);
        }

        if (postAction != PostAction::kNone) {
            auto txnPtr = txnMgr_->Extract(TLabel{comp.tLabel});
            if (txnPtr) {
                switch (postAction) {
                    case PostAction::kCompleteSuccess:
                    case PostAction::kCompletePhySuccess:
                        txnPtr->TransitionTo(TransactionState::ATCompleted, transitionTag1 ? transitionTag1 : "OnATCompletion");
                        txnPtr->TransitionTo(TransactionState::Completed, transitionTag2 ? transitionTag2 : "OnATCompletion");
                        txnPtr->InvokeResponseHandler(postKr, {});
                        break;
                    case PostAction::kCompleteError:
                    case PostAction::kCompleteTimeout:
                        txnPtr->TransitionTo(TransactionState::ATCompleted, transitionTag1 ? transitionTag1 : "OnATCompletion");
                        txnPtr->TransitionTo(TransactionState::Failed, transitionTag2 ? transitionTag2 : "OnATCompletion");
                        txnPtr->InvokeResponseHandler(postKr, {});
                        break;
                    case PostAction::kCompleteCancelled:
                        txnPtr->TransitionTo(TransactionState::Cancelled, transitionTag1 ? transitionTag1 : "OnATCompletion");
                        txnPtr->InvokeResponseHandler(postKr, {});
                        break;
                    case PostAction::kNone:
                        break;
                }
                if (labelAllocator_) {
                    labelAllocator_->Free(comp.tLabel);
                }
            }
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

        ASFW_LOG_V2(Async,
                    "📥 OnARResponse: tLabel=%u nodeID=0x%04X gen=%u rcode=0x%X len=%zu",
                    key.label.value, key.node.value, key.generation.value, rcode, data.size());



        Transaction* txn = txnMgr_->FindByMatchKey(key);
        if (!txn) {
            ASFW_LOG(Async, "⚠️  OnARResponse: No transaction for key");
            return;
        }

        // Verify we're in correct state
        const auto state = txn->state();

        // 1. If it's already terminal, AR is too late → ignore.
        if (state == TransactionState::Completed ||
            state == TransactionState::Failed ||
            state == TransactionState::Cancelled ||
            state == TransactionState::TimedOut) {
            ASFW_LOG_V3(Async, "OnARResponse: AR for terminal txn (state=%{public}s) – ignoring",
                        ToString(state));
            return;
        }

        // 2. Otherwise, accept AR in ATPosted / ATCompleted / AwaitingAR.
        if (state != TransactionState::AwaitingAR) {
            ASFW_LOG_V2(Async, "OnARResponse: AR in state=%{public}s (not AwaitingAR) – accepting as completion",
                        ToString(state));
        }

        // 3. Try to mark as completed (guards against double-completion with AT path)
        if (!txn->TryMarkCompleted()) {
            ASFW_LOG_V3(Async, "OnARResponse: AR arrived but AT already completed, ignoring");
            return;
        }

        // Transition: AwaitingAR → ARReceived
        
        // Extract transaction to complete it safely outside lock
        // This avoids holding the lock while invoking the callback
        auto txnPtr = txnMgr_->Extract(key.label);
        if (!txnPtr) {
            ASFW_LOG(Async, "⚠️  OnARResponse: Failed to extract transaction (concurrent removal?)");
            return;
        }

        txnPtr->TransitionTo(TransactionState::ARReceived, "OnARResponse");

        // Convert rcode to kern_return_t
        kern_return_t kr = (rcode == 0) ? kIOReturnSuccess : kIOReturnError;

        // Complete transaction
        ASFW_LOG_V2(Async, "  → Completed (rcode=0x%X, kr=0x%08X)", rcode, kr);
        
        if (txnPtr->state() != TransactionState::Completed &&
            txnPtr->state() != TransactionState::Failed &&
            txnPtr->state() != TransactionState::Cancelled &&
            txnPtr->state() != TransactionState::TimedOut) {
            txnPtr->TransitionTo(TransactionState::Completed, "OnARResponse");
        }

        // Invoke callback
        txnPtr->InvokeResponseHandler(kr, data);

        // Free label (allocator is thread-safe)
        if (labelAllocator_) {
            labelAllocator_->Free(key.label.value);
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
        bool shouldFail = false;

        bool found = txnMgr_->WithTransaction(label, [&](Transaction* txn) {
            uint8_t ackCode = txn->ackCode();
            TransactionState state = txn->state();

            ASFW_LOG_V1(Async,
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

                    ASFW_LOG_V1(Async, "🔄 RECOVERY: tLabel=%u Busy ACK (0x%X). "
                             "Device is busy, extending deadline +200ms (attempt %u/%u)",
                             txn->label().value, ackCode, txn->retryCount(), kMaxBusyRetries);

                    // Don't complete transaction - let timeout engine check again at new deadline
                    // If device becomes non-busy and sends AR response, OnARResponse will complete it
                    // If still busy after 3 retries, next timeout will fail the transaction
                    return;
                }
            }

            // ATPosted with ackCode=0x0: AT completion never arrived (packet wasn't sent)
            // This can happen if AT context is backed up or hardware issue.
            // Give it another chance - the packet might just be delayed in the queue.
            if (state == TransactionState::ATPosted && ackCode == 0x0) {
                constexpr uint8_t kMaxATRetries = 2;
                if (txn->retryCount() < kMaxATRetries) {
                    txn->IncrementRetry();
                    
                    // Extend deadline: give AT context more time to process
                    const uint64_t newDeadline = Engine::NowUs() + 250000;  // +250ms
                    txn->SetDeadline(newDeadline);
                    
                    ASFW_LOG_V1(Async, 
                             "🔄 RECOVERY: tLabel=%u ATPosted timeout with no ACK. "
                             "Packet may be queued in AT context. Extending deadline +250ms "
                             "(attempt %u/%u)",
                             txn->label().value, txn->retryCount(), kMaxATRetries);
                    return;  // Don't fail, let AT complete
                }
                ASFW_LOG_V1(Async, 
                         "❌ FAILED: tLabel=%u ATPosted - AT completion never arrived after %u attempts. "
                         "Possible AT context stall or hardware issue.",
                         txn->label().value, kMaxATRetries);
            }

            // Apple-style retry: When waiting for AR response and ACK indicated device
            // acknowledged the request (ackPending), give it more time instead of failing.
            // This matches IOFWAsyncCommand::complete() which retries on timeout when
            // ACK was pending/busy. See IOFireWireFamily.kmodproj/IOFWAsyncCommand.cpp:425-470
            if (state == TransactionState::AwaitingAR) {
                // ackCode 0x1 = ack_pending (device acknowledged, processing)
                // ackCode 0x8 = used in some device responses (observed in logs)
                // ackCode 0xC = ack_tardy (slow device)
                if (ackCode == 0x1 || ackCode == 0x8 || ackCode == 0xC) {
                    constexpr uint8_t kMaxPendingRetries = 3;  // Apple's kFWCmdDefaultRetries
                    if (txn->retryCount() < kMaxPendingRetries) {
                        txn->IncrementRetry();
                        
                        // Extend deadline: 250ms per retry (matching base timeout)
                        const uint64_t newDeadline = Engine::NowUs() + 250000;  // +250ms
                        txn->SetDeadline(newDeadline);
                        
                        // Log loudly for debugging - Apple-style recovery
                        ASFW_LOG_V1(Async, 
                                 "🔄 RECOVERY: tLabel=%u AwaitingAR timeout with ackCode=0x%X. "
                                 "Device acknowledged but response late. Extending deadline +250ms "
                                 "(attempt %u/%u)",
                                 txn->label().value, ackCode, txn->retryCount(), kMaxPendingRetries);
                        return;  // Don't fail, let AR response arrive or retry again
                    }
                    ASFW_LOG_V1(Async, 
                             "❌ FAILED: tLabel=%u AwaitingAR with ackCode=0x%X - max retries (%u) exhausted. "
                             "Device never sent response.",
                             txn->label().value, ackCode, kMaxPendingRetries);
                }
            }

            // Timeout is terminal
            
            // Extract transaction to complete it safely outside lock
            // We can't do this inside WithTransaction because it invalidates 'txn'
            // So we mark a flag and do it after
            shouldFail = true;
        });

        if (shouldFail) {
        auto txnPtr = txnMgr_->Extract(label);
        if (txnPtr) {
            txnPtr->TransitionTo(TransactionState::TimedOut, "OnTimeout");
            
            // Invoke callback
            txnPtr->InvokeResponseHandler(kIOReturnTimeout, {});
            
            // Free label
            if (labelAllocator_) {
                labelAllocator_->Free(label.value);
            }
        }
        }

        if (!found) {
            ASFW_LOG(Async, "⚠️  OnTimeout: No transaction for tLabel=%u", label.value);
        }
    }

private:
    TransactionManager* txnMgr_;
    LabelAllocator* labelAllocator_;
};

} // namespace ASFW::Async
