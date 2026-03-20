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

        ATPostResult postResult;

        ASFW_LOG_V2(Async,
                    "🔄 OnATCompletion: tLabel=%u ack=0x%X event=0x%02X ts=%u ackCount=%u",
                    comp.tLabel, comp.ackCode, static_cast<uint8_t>(comp.eventCode), comp.timeStamp, comp.ackCount);

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
            txn->SetAckCode(comp.ackCode);

            // PHY packets complete on AT path only (no AR response expected).
            if (txn->GetCompletionStrategy() == CompletionStrategy::CompleteOnPHY) {
                ASFW_LOG(Async, "  → Completed (PHY, AT-only) ackCode=0x%X event=0x%02X",
                         comp.ackCode, static_cast<uint8_t>(comp.eventCode));
                postResult.action = ATPostAction::kCompletePhySuccess;
                postResult.transitionTag1 = "OnATCompletion: phy";
                postResult.transitionTag2 = "OnATCompletion: phy";
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

            if (HandleHardwareEventCompletion(*txn, comp, postResult)) {
                return;
            }

            HandleAckCompletion(*txn, comp, postResult);
        });

        if (!found) {
            // Expected for split transactions: AR response completed the transaction
            // before AT completion interrupt arrived. This is a benign race.
            ASFW_LOG_V3(Async, "OnATCompletion: Transaction already completed for tLabel=%u (AR won race)", comp.tLabel);
        }

        FinalizeATPostAction(TLabel{comp.tLabel}, postResult);
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

        // Free the label before invoking the callback so re-entrant submits from the
        // callback do not inherit stale bitmap state from the just-completed transaction.
        if (labelAllocator_) {
            labelAllocator_->Free(key.label.value);
        }

        // Convert rcode to kern_return_t
        kern_return_t kr = (rcode == 0) ? kIOReturnSuccess : kIOReturnError;

        ASFW_LOG(Async,
                 "OnARResponse: completing tLabel=%u gen=%u node=0x%04X rcode=0x%X kr=0x%08x len=%zu",
                 key.label.value,
                 key.generation.value,
                 key.node.value,
                 rcode,
                 kr,
                 data.size());

        // Complete transaction
        ASFW_LOG_V2(Async, "  → Completed (rcode=0x%X, kr=0x%08X)", rcode, kr);
        
        if (txnPtr->state() != TransactionState::Completed &&
            txnPtr->state() != TransactionState::Failed &&
            txnPtr->state() != TransactionState::Cancelled &&
            txnPtr->state() != TransactionState::TimedOut) {
            txnPtr->TransitionTo(TransactionState::Completed, "OnARResponse");
        }

        // Invoke callback
        txnPtr->InvokeResponseHandler(kr, rcode, data);
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

            if (HandleBusyTimeout(*txn) ||
                HandleATPostedTimeout(*txn) ||
                HandleAwaitingARTimeout(*txn)) {
                return;
            }

            shouldFail = true;
        });

        if (shouldFail) {
        auto txnPtr = txnMgr_->Extract(label);
        if (txnPtr) {
            txnPtr->TransitionTo(TransactionState::TimedOut, "OnTimeout");

            // Free the label before invoking the callback so retries started from the
            // callback do not trip stale-bitmap recovery.
            if (labelAllocator_) {
                labelAllocator_->Free(label.value);
            }

            // Invoke callback
            txnPtr->InvokeResponseHandler(kIOReturnTimeout, 0xFF, {});
        }
        }

        if (!found) {
            ASFW_LOG(Async, "⚠️  OnTimeout: No transaction for tLabel=%u", label.value);
        }
    }

private:
    enum class ATPostAction {
        kNone,
        kCompleteSuccess,
        kCompleteError,
        kCompleteTimeout,
        kCompleteCancelled,
        kCompletePhySuccess,
    };

    struct ATPostResult {
        ATPostAction action{ATPostAction::kNone};
        const char* transitionTag1{nullptr};
        const char* transitionTag2{nullptr};
        kern_return_t postKr{kIOReturnSuccess};
    };

    [[nodiscard]] bool HandleHardwareEventCompletion(Transaction& txn,
                                                     const TxCompletion& comp,
                                                     ATPostResult& postResult) noexcept {
        const uint8_t eventCode = static_cast<uint8_t>(comp.eventCode);
        const uint8_t ackCode = comp.ackCode;

        if (eventCode == 0x0A || eventCode == 0x03) {
            ASFW_LOG(Async, "  → Hardware event: %{public}s (ackCode=0x%X)",
                     ToString(comp.eventCode), ackCode);

            if (ackCode == 0x1) {
                ASFW_LOG(Async, "  → AwaitingAR (ackPending despite hw timeout)");
                txn.TransitionTo(TransactionState::ATCompleted, "OnATCompletion: hw_timeout_pending");
                txn.TransitionTo(TransactionState::AwaitingAR, "OnATCompletion: ackPending");
            } else {
                ASFW_LOG(Async, "  → Failed (hw timeout, ackCode=0x%X)", ackCode);
                postResult.action = ATPostAction::kCompleteTimeout;
                postResult.transitionTag1 = "OnATCompletion: hw_timeout";
                postResult.transitionTag2 = "OnATCompletion: hw_timeout";
                postResult.postKr = kIOReturnTimeout;
            }
            return true;
        }

        if (eventCode == static_cast<uint8_t>(OHCIEventCode::kEvtFlushed)) {
            ASFW_LOG(Async, "  → Cancelled (flushed)");
            postResult.action = ATPostAction::kCompleteCancelled;
            postResult.transitionTag1 = "OnATCompletion: flushed";
            postResult.postKr = kIOReturnAborted;
            return true;
        }

        return false;
    }

    void HandleAckCompletion(Transaction& txn,
                             const TxCompletion& comp,
                             ATPostResult& postResult) noexcept {
        const uint8_t ackCode = comp.ackCode;
        const auto strategy = txn.GetCompletionStrategy();
        const bool needsARData = txn.IsReadOperation() || strategy == CompletionStrategy::CompleteOnAR;

        switch (ackCode) {
            case 0x1:
                ASFW_LOG_V2(Async, "  → AwaitingAR (ackPending, need AR response)");
                txn.TransitionTo(TransactionState::ATCompleted, "OnATCompletion: ackPending");
                txn.TransitionTo(TransactionState::AwaitingAR, "OnATCompletion: ackPending");
                break;

            case 0x0:
                if (needsARData) {
                    ASFW_LOG_V2(Async, "  → AwaitingAR (ackComplete but data required)");
                    txn.TransitionTo(TransactionState::ATCompleted, "OnATCompletion: ackComplete_read");
                    txn.TransitionTo(TransactionState::AwaitingAR, "OnATCompletion: ackComplete_read");
                    break;
                }
                if (txn.TryMarkCompleted()) {
                    ASFW_LOG_V1(Async, "  → Completed (ackComplete, AT path won)");
                    postResult.action = ATPostAction::kCompleteSuccess;
                    postResult.transitionTag1 = "OnATCompletion: ackComplete";
                    postResult.transitionTag2 = "OnATCompletion: ackComplete";
                } else {
                    ASFW_LOG_V3(Async, "  → ackComplete but AR already completed, ignoring");
                }
                break;

            case 0x4:
            case 0x5:
            case 0x6:
                ASFW_LOG_V2(Async, "  → Busy (0x%X), extending deadline for retry", ackCode);
                txn.TransitionTo(TransactionState::ATCompleted, "OnATCompletion: busy");
                txn.SetDeadline(Engine::NowUs() + 200000);
                break;

            case 0xC:
            case 0x11:
            case 0x1B:
                ASFW_LOG_V2(Async, "  → AwaitingAR (ackCode=0x%X tardy/slow, wait for response)", ackCode);
                txn.TransitionTo(TransactionState::ATCompleted, "OnATCompletion: tardy");
                txn.TransitionTo(TransactionState::AwaitingAR, "OnATCompletion: tardy");
                break;

            case 0xD:
            case 0xE:
                ASFW_LOG_V1(Async, "  → Failed (ackError 0x%X)", ackCode);
                postResult.action = ATPostAction::kCompleteError;
                postResult.transitionTag1 = "OnATCompletion: ackError";
                postResult.transitionTag2 = "OnATCompletion: ackError";
                postResult.postKr = kIOReturnError;
                break;

            default:
                ASFW_LOG_V2(Async, "  → Unknown ackCode=0x%X, treating as tardy (wait for AR)", ackCode);
                txn.TransitionTo(TransactionState::ATCompleted, "OnATCompletion: unknown_ack");
                txn.TransitionTo(TransactionState::AwaitingAR, "OnATCompletion: unknown_ack");
                break;
        }
    }

    void FinalizeATPostAction(TLabel label, const ATPostResult& postResult) noexcept {
        if (postResult.action == ATPostAction::kNone) {
            return;
        }

        auto txnPtr = txnMgr_->Extract(label);
        if (!txnPtr) {
            return;
        }

        switch (postResult.action) {
            case ATPostAction::kCompleteSuccess:
            case ATPostAction::kCompletePhySuccess:
                txnPtr->TransitionTo(TransactionState::ATCompleted,
                                     postResult.transitionTag1 ? postResult.transitionTag1 : "OnATCompletion");
                txnPtr->TransitionTo(TransactionState::Completed,
                                     postResult.transitionTag2 ? postResult.transitionTag2 : "OnATCompletion");
                break;
            case ATPostAction::kCompleteError:
            case ATPostAction::kCompleteTimeout:
                txnPtr->TransitionTo(TransactionState::ATCompleted,
                                     postResult.transitionTag1 ? postResult.transitionTag1 : "OnATCompletion");
                txnPtr->TransitionTo(TransactionState::Failed,
                                     postResult.transitionTag2 ? postResult.transitionTag2 : "OnATCompletion");
                break;
            case ATPostAction::kCompleteCancelled:
                txnPtr->TransitionTo(TransactionState::Cancelled,
                                     postResult.transitionTag1 ? postResult.transitionTag1 : "OnATCompletion");
                break;
            case ATPostAction::kNone:
                break;
        }

        if (labelAllocator_) {
            labelAllocator_->Free(label.value);
        }

        switch (postResult.action) {
            case ATPostAction::kCompleteSuccess:
            case ATPostAction::kCompletePhySuccess:
            case ATPostAction::kCompleteError:
            case ATPostAction::kCompleteTimeout:
            case ATPostAction::kCompleteCancelled:
                txnPtr->InvokeResponseHandler(postResult.postKr, 0xFF, {});
                break;
            case ATPostAction::kNone:
                break;
        }
    }

    [[nodiscard]] bool HandleBusyTimeout(Transaction& txn) noexcept {
        const uint8_t ackCode = txn.ackCode();
        if (ackCode != 0x4 && ackCode != 0x5 && ackCode != 0x6) {
            return false;
        }

        constexpr uint8_t kMaxBusyRetries = 3;
        if (txn.retryCount() >= kMaxBusyRetries) {
            return false;
        }

        txn.IncrementRetry();
        txn.SetDeadline(Engine::NowUs() + 200000);
        ASFW_LOG_V1(Async, "🔄 RECOVERY: tLabel=%u Busy ACK (0x%X). Device is busy, extending deadline +200ms (attempt %u/%u)",
                    txn.label().value, ackCode, txn.retryCount(), kMaxBusyRetries);
        return true;
    }

    [[nodiscard]] bool HandleATPostedTimeout(Transaction& txn) noexcept {
        if (txn.state() != TransactionState::ATPosted || txn.ackCode() != 0x0) {
            return false;
        }

        constexpr uint8_t kMaxATRetries = 2;
        if (txn.retryCount() < kMaxATRetries) {
            txn.IncrementRetry();
            txn.SetDeadline(Engine::NowUs() + 250000);
            ASFW_LOG_V1(Async,
                        "🔄 RECOVERY: tLabel=%u ATPosted timeout with no ACK. Packet may be queued in AT context. Extending deadline +250ms (attempt %u/%u)",
                        txn.label().value, txn.retryCount(), kMaxATRetries);
            return true;
        }

        ASFW_LOG_V1(Async,
                    "❌ FAILED: tLabel=%u ATPosted - AT completion never arrived after %u attempts. Possible AT context stall or hardware issue.",
                    txn.label().value, kMaxATRetries);
        return false;
    }

    [[nodiscard]] bool HandleAwaitingARTimeout(Transaction& txn) noexcept {
        if (txn.state() != TransactionState::AwaitingAR) {
            return false;
        }

        const uint8_t ackCode = txn.ackCode();
        if (ackCode != 0x1 && ackCode != 0x8 && ackCode != 0xC) {
            return false;
        }

        constexpr uint8_t kMaxPendingRetries = 3;
        if (txn.retryCount() < kMaxPendingRetries) {
            txn.IncrementRetry();
            txn.SetDeadline(Engine::NowUs() + 250000);
            ASFW_LOG_V1(Async,
                        "🔄 RECOVERY: tLabel=%u AwaitingAR timeout with ackCode=0x%X. Device acknowledged but response late. Extending deadline +250ms (attempt %u/%u)",
                        txn.label().value, ackCode, txn.retryCount(), kMaxPendingRetries);
            return true;
        }

        ASFW_LOG_V1(Async,
                    "❌ FAILED: tLabel=%u AwaitingAR with ackCode=0x%X - max retries (%u) exhausted. Device never sent response.",
                    txn.label().value, ackCode, kMaxPendingRetries);
        return false;
    }

    TransactionManager* txnMgr_;
    LabelAllocator* labelAllocator_;
};

} // namespace ASFW::Async
