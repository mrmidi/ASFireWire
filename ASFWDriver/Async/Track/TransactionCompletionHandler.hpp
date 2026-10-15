#pragma once

#include "../Core/Transaction.hpp"
#include "../Core/TransactionManager.hpp"
#include "TxCompletion.hpp"
#include "../../Bus/Timing/SplitTimeout.hpp"
#include <memory>
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
    explicit TransactionCompletionHandler(TransactionManager* txnMgr, LabelAllocator* labelAllocator,
        std::shared_ptr<FW::SplitTimeout> splitTimeout = std::make_shared<FW::SplitTimeout>()) noexcept
        : txnMgr_(txnMgr), labelAllocator_(labelAllocator), splitTimeout_(std::move(splitTimeout)) {}

    /**
     * \brief Handle AT descriptor completion (gotAck equivalent).
     *
     * Called from ATContextBase::ScanCompletion when AT descriptor completes.
     * Extracts ACK code from xferStatus and transitions transaction state.
     *
     * \param rawComp TxCompletion from OHCI driver (ackCode normalized from eventCode here)
     *
     * \par State Transitions
     * - ackCode==0x1 (pending): ATCompleted → AwaitingAR (wait for AR response)
     * - ackCode==0x0 (complete): ATCompleted → Completed (immediate completion)
     * - event ack_busy_X/A/B (0x14-0x16): → Failed with kIOReturnBusy, reads included
     * - ackCode==0xF (timeout): ATCompleted → Failed
     * - eventCode errors: ATCompleted → Failed
     *
     * \par Critical Logic
     * OHCI 1.2 Table 3-2 defines these completion events. Internal ACK values
     * below are normalized and are not the IEEE wire ACK values.
     * - ack_pending (0x1): Split transaction, wait for response packet
     * - ack_complete (0x0): Unified transaction, done immediately
     * - ack_busy_X/A/B (0x4-0x6): final — the controller has already retried the
     *   request ATRetries times; any further retry belongs to the caller
     */
    void OnATCompletion(const TxCompletion& rawComp, uint64_t requestTimeUs = Engine::NowUs()) noexcept {
        if (!txnMgr_) {
            return;
        }

        // eventCode is authoritative for AT completions. Validated with Linux —
        // refs: ohci.c (handle_at_packet): the acknowledge is carried in the event-code
        // field, so derive the internal ack scheme from it here at the point of
        // consumption. This makes the handler robust to controllers that surface a
        // raw/packed ack value (e.g. 0x8, the ContextControl RUN bit) in the
        // descriptor's ack nibble regardless of how the TxCompletion was produced.
        const uint8_t rawAckCode = rawComp.ackCode;
        TxCompletion comp = rawComp;
        comp.ackCode = NormalizeAckFromEvent(comp.eventCode, comp.ackCode);

        if (rawAckCode == 0x8 && comp.ackCode == rawAckCode) {
            ASFW_LOG_V1(Async,
                        "AT completion raw ack nibble 0x8 survived normalization: "
                        "tLabel=%u event=0x%02X (%{public}s) ts=%u spd=%u. "
                        "Linux treats eventCode, not this nibble, as authoritative.",
                        comp.tLabel, static_cast<uint8_t>(comp.eventCode),
                        ToString(comp.eventCode), comp.timeStamp, comp.speed);
        }

        // AT Response context completions correspond to WrResp acks we send back
        // to devices. They are not tracked as transactions; skip quietly — but a
        // response the device never received (evt_timeout = expired before
        // transmit, busy = device didn't take it) leaves the requester's split
        // transaction dangling. LS-9000 firmware wedges its execution engine on
        // exactly that, so surface it at default level.
        if (comp.isResponseContext) {
            if (comp.eventCode != OHCIEventCode::kAckComplete &&
                comp.eventCode != OHCIEventCode::kAckPending) {
                ASFW_LOG(Async,
                         "AT-resp DROPPED: tLabel=%u event=0x%02X (%{public}s) ts=%u",
                         comp.tLabel, static_cast<uint8_t>(comp.eventCode),
                         ToString(comp.eventCode), comp.timeStamp);
            } else {
                ASFW_LOG_V3(Async, "OnATCompletion: Ignoring AT Response completion (tLabel=%u)",
                            comp.tLabel);
            }
            return;
        }

        ATPostResult postResult;

        ASFW_LOG_V2(Async,
                    "🔄 OnATCompletion: tLabel=%u ack=0x%X event=0x%02X ts=%u spd=%u",
                    comp.tLabel, comp.ackCode, static_cast<uint8_t>(comp.eventCode), comp.timeStamp, comp.speed);

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

            // A final ack_busy: the controller has spent its ATRetries and the node
            // never took the request, so no response will follow. End it now —
            // reads included, before the CompleteOnAR skip below — and leave any
            // retry to the caller. Validated with Linux — refs: core-transaction.c
            // (transmit_complete_callback) closes ACK_BUSY_X/A/B with RCODE_BUSY.
            if (IsFinalBusy(comp.eventCode)) {
                ASFW_LOG_V1(Async,
                            "[AsyncBusy] tLabel=%u node=0x%04x gen=%u requestTCode=0x%x state=%{public}s event=%{public}s",
                            txn->label().value, txn->nodeID().value, txn->generation().value,
                            txn->tCode(), ToString(txn->state()), ToString(comp.eventCode));
                postResult.action = ATPostAction::kCompleteBusy;
                postResult.transitionTag1 = "OnATCompletion: busy";
                postResult.postKr = kIOReturnBusy;
                return;
            }

            if (HandleHardwareEventCompletion(*txn, comp, postResult)) {
                return;
            }

            HandleAckCompletion(*txn, comp, postResult, requestTimeUs);
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
     * Per IEEE 1394-2008 7.3.3.2.3, response packet arrival is the
     * definitive completion event. Even if AT completion reported errors,
     * successful AR response means transaction succeeded.
     */
    void OnARResponse(const MatchKey& key, uint8_t rcode, std::span<const uint8_t> data,
                      uint8_t tCode = 0, uint16_t hardwareStamp = 0) noexcept {
        if (!txnMgr_) {
            return;
        }

        ASFW_LOG_V2(Async,
                    "📥 OnARResponse: tLabel=%u nodeID=0x%04X gen=%u rcode=0x%X len=%zu",
                    key.label.value, key.node.value, key.generation.value, rcode, data.size());



        Transaction* txn = txnMgr_->FindByMatchKey(key);
        if (!txn) {
            ASFW_LOG(Async, "[AsyncLateResponse] tLabel=%u node=0x%04x gen=%u tCode=0x%x rCode=0x%x bytes=%zu hwStamp=0x%04x previousHwStamp=0x%04x sincePreviousUs=%llu hostUs=%llu",
                     key.label.value, key.node.value, key.generation.value, tCode, rcode,
                     data.size(), hardwareStamp, lastResponseStamp_,
                     static_cast<unsigned long long>(lastResponseUs_ ? Engine::NowUs() - lastResponseUs_ : 0),
                     static_cast<unsigned long long>(Engine::NowUs()));
            lastResponseStamp_ = hardwareStamp;
            lastResponseUs_ = Engine::NowUs();
            return;
        }

        lastResponseStamp_ = hardwareStamp;
        lastResponseUs_ = Engine::NowUs();

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
     * - If ATPosted with no ACK yet: extend the deadline (AT completion may be queued)
     * - AwaitingAR: expire the fixed split-response deadline; do not extend it
     * - Otherwise: complete with timeout error
     * A final busy never reaches here: OnATCompletion ends it at once.
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
                        "[AsyncTimeout] tLabel=%u node=0x%04x gen=%u requestTCode=0x%x state=%{public}s ackCode=0x%X retries=%u hostUs=%llu",
                        txn->label().value, txn->nodeID().value, txn->generation().value,
                        txn->tCode(), ToString(state), ackCode, txn->retryCount(),
                        static_cast<unsigned long long>(Engine::NowUs()));

            if (HandleATPostedTimeout(*txn)) {
                return;
            }

            shouldFail = true;
        });

        if (shouldFail) {
            auto txnPtr = txnMgr_->Extract(label);
            if (txnPtr) {
                txnPtr->TransitionTo(TransactionState::TimedOut, "OnTimeout");
                // Protect a timed-out split label before making it available again.
                if (labelAllocator_) {
                    if (txnPtr->splitReuseAfterUs() != 0) {
                        labelAllocator_->Quarantine(label.value, txnPtr->nodeID().value,
                                                    txnPtr->splitReuseAfterUs());
                    }
                    labelAllocator_->Free(label.value);
                }
                txnPtr->InvokeResponseHandler(kIOReturnTimeout, 0xFF, {});
            }
        }

        if (!found) {
            ASFW_LOG(Async, "⚠️  OnTimeout: No transaction for tLabel=%u", label.value);
        }
    }

private:
    uint16_t lastResponseStamp_{0};
    uint64_t lastResponseUs_{0};
    enum class ATPostAction {
        kNone,
        kCompleteSuccess,
        kCompleteError,
        kCompleteTimeout,
        kCompleteCancelled,
        kCompletePhySuccess,
        kCompleteBusy,
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

            // The event field is authoritative (OHCI Table 3-2). A raw status
            // nibble cannot turn missing-ACK/expiry into packet acceptance.
            postResult.action = ATPostAction::kCompleteTimeout;
            postResult.transitionTag1 = "OnATCompletion: hw_timeout";
            postResult.postKr = kIOReturnTimeout;
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

    // Map the OHCI event code to this driver's internal ack scheme (complete=0x0,
    // pending=0x1, busy=0x4/0x5/0x6, tardy=0xC, dataError=0xD, typeError=0xE).
    // Non-ack event codes keep the raw nibble for the hardware-event / default paths.
    // Validated with Linux — refs: ohci.c (handle_at_packet): ack == evt - 0x10 for the
    // 0x11..0x1e range carried in the event-code field.
    [[nodiscard]] static uint8_t NormalizeAckFromEvent(OHCIEventCode eventCode,
                                                       uint8_t rawAckCode) noexcept {
        switch (eventCode) {
            case OHCIEventCode::kAckComplete:  return 0x0;
            case OHCIEventCode::kAckPending:   return 0x1;
            case OHCIEventCode::kAckBusyX:     return 0x4;
            case OHCIEventCode::kAckBusyA:     return 0x5;
            case OHCIEventCode::kAckBusyB:     return 0x6;
            case OHCIEventCode::kAckTardy:     return 0xC;
            case OHCIEventCode::kAckDataError: return 0xD;
            case OHCIEventCode::kAckTypeError: return 0xE;
            default:                           return rawAckCode;
        }
    }

    [[nodiscard]] static bool IsFinalBusy(OHCIEventCode eventCode) noexcept {
        return eventCode == OHCIEventCode::kAckBusyX ||
               eventCode == OHCIEventCode::kAckBusyA ||
               eventCode == OHCIEventCode::kAckBusyB;
    }

    void HandleAckCompletion(Transaction& txn,
                             const TxCompletion& comp,
                             ATPostResult& postResult, uint64_t requestTimeUs) noexcept {
        const uint8_t ackCode = comp.ackCode;
        const auto strategy = txn.GetCompletionStrategy();
        const bool needsARData = txn.IsReadOperation() || strategy == CompletionStrategy::CompleteOnAR;

        switch (ackCode) {
            case 0x1:
                if (txn.splitReuseAfterUs() != 0) break; // duplicate completion cannot restart the wait
                ASFW_LOG_V2(Async, "  → AwaitingAR (ackPending, need AR response)");
                // OHCI 1.2 Table 3-2; IEEE 1394-2008 7.3.3.1.2:
                // acceptance starts one split-response wait, never packet retransmission.
                if (txn.splitReuseAfterUs() == 0) {
                    const auto cycles = splitTimeout_->Cycles();
                    const auto timeoutUs = uint64_t{cycles} * 125u;
                    txn.SetDeadline(requestTimeUs + timeoutUs);
                    txn.SetSplitReuseAfter(requestTimeUs + 2 * timeoutUs);
                }
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
                ASFW_LOG_V2(Async,
                            "  → Unknown ackCode=0x%X event=0x%02X (%{public}s), treating as tardy (wait for AR)",
                            ackCode, static_cast<uint8_t>(comp.eventCode),
                            ToString(comp.eventCode));
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
            case ATPostAction::kCompleteBusy:
                // Straight to Failed: a read is already in AwaitingAR (CompleteOnAR).
                txnPtr->TransitionTo(TransactionState::Failed,
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
            case ATPostAction::kCompleteBusy:
                txnPtr->InvokeResponseHandler(postResult.postKr, 0xFF, {});
                break;
            case ATPostAction::kNone:
                break;
        }
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

    TransactionManager* txnMgr_;
    LabelAllocator* labelAllocator_;
    std::shared_ptr<FW::SplitTimeout> splitTimeout_;
};

} // namespace ASFW::Async
