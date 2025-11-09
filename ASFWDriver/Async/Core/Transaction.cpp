#include "Transaction.hpp"
#include "../../Logging/Logging.hpp"
#include <cassert>

namespace ASFW::Async {

void Transaction::TransitionTo(TransactionState newState, const char* reason) noexcept {
    // Runtime validation (debug builds only - optimized away in release)
    if (!IsValidTransition(state_, newState)) {
        ASFW_LOG_ERROR(Async,
                       "ILLEGAL STATE TRANSITION: tLabel=%u %{public}s → %{public}s (reason: %{public}s)",
                       static_cast<unsigned>(label_.value),
                       ToString(state_),
                       ToString(newState),
                       reason ? reason : "none");
        // In debug builds, assert will fire
        assert(IsValidTransition(state_, newState) && "Illegal state transition");
        return;
    }

    // Record history before transitioning
    history_[historyIdx_].oldState = state_;
    history_[historyIdx_].newState = newState;
    history_[historyIdx_].timestampUs = 0; // TODO: Get actual timestamp
    history_[historyIdx_].reason = reason;
    historyIdx_ = (historyIdx_ + 1) % 16;

    ASFW_LOG(Async,
             "  🔄 Transaction tLabel=%u: %{public}s → %{public}s (%{public}s)",
             static_cast<unsigned>(label_.value),
             ToString(state_),
             ToString(newState),
             reason ? reason : "no reason");

    state_ = newState;

    // Auto-release resources on terminal states
    if (newState == TransactionState::Completed ||
        newState == TransactionState::TimedOut ||
        newState == TransactionState::Failed ||
        newState == TransactionState::Cancelled) {
        ReleaseResources();
    }
}

void Transaction::ReleaseResources() noexcept {
    // Phase 1.3: UniquePayload automatically releases on destruction/reset
    // Just reset the payload wrapper (triggers automatic cleanup if owned)
    if (payload_.IsValid()) {
        ASFW_LOG(Async,
                 "  🗑️  Transaction tLabel=%u: releasing payload (automatic via UniquePayload)",
                 static_cast<unsigned>(label_.value));
        payload_.Reset();  // Automatic cleanup via RAII
    }

    // NOTE: We do NOT clear responseHandler_ here because:
    // 1. The callback must be invoked AFTER transitioning to terminal state
    // 2. TransitionTo(Completed) is called BEFORE InvokeResponseHandler in CompleteTransaction_
    // 3. Clearing here would destroy the callback before it can be invoked
    // 4. The std::function will be automatically destroyed when Transaction is destroyed
    // PREVIOUSLY BUGGY CODE:
    // responseHandler_ = nullptr;  ← This was clearing the callback too early!
}

std::span<const TransactionStateHistory> Transaction::GetHistory() const noexcept {
    return std::span<const TransactionStateHistory>(history_, 16);
}

void Transaction::DumpHistory() const noexcept {
    ASFW_LOG(Async,
             "📜 Transaction tLabel=%u (gen=%u, node=0x%04x) State History:",
             static_cast<unsigned>(label_.value),
             generation_.value,
             nodeID_.value);

    for (uint8_t i = 0; i < 16; ++i) {
        uint8_t idx = (historyIdx_ + i) % 16;
        const auto& entry = history_[idx];

        if (entry.timestampUs == 0 && entry.reason == nullptr) {
            continue; // Empty slot
        }

        ASFW_LOG(Async,
                 "  [%2u] %llu μs: %{public}s → %{public}s (%{public}s)",
                 i,
                 static_cast<unsigned long long>(entry.timestampUs),
                 ToString(entry.oldState),
                 ToString(entry.newState),
                 entry.reason ? entry.reason : "none");
    }
}

const char* ToString(TransactionState state) noexcept {
    switch (state) {
        case TransactionState::Created:     return "Created";
        case TransactionState::Submitted:   return "Submitted";
        case TransactionState::ATPosted:    return "ATPosted";
        case TransactionState::ATCompleted: return "ATCompleted";
        case TransactionState::AwaitingAR:  return "AwaitingAR";
        case TransactionState::ARReceived:  return "ARReceived";
        case TransactionState::Completed:   return "Completed";
        case TransactionState::TimedOut:    return "TimedOut";
        case TransactionState::Failed:      return "Failed";
        case TransactionState::Cancelled:   return "Cancelled";
        default:                            return "Unknown";
    }
}

} // namespace ASFW::Async
