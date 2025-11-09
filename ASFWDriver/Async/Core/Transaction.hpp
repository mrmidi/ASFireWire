#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>

#include <DriverKit/IOReturn.h>
#include "PayloadPolicy.hpp"  // Phase 1.3: UniquePayload ownership
#include "PayloadHandle.hpp"  // Required for UniquePayload<PayloadHandle> instantiation
#include "CompletionStrategy.hpp"  // Explicit two-path completion model
#include "../../Logging/Logging.hpp"  // For ASFW_LOG

namespace ASFW::Async {

// Forward declarations (PayloadHandle now fully defined above)

// =============================================================================
// Strong Types (Prevent Parameter Order Bugs)
// =============================================================================

template<typename T, typename Tag>
struct StrongType {
    T value;
    constexpr explicit StrongType(T v) noexcept : value(v) {}
    constexpr operator T() const noexcept { return value; }

    constexpr bool operator==(const StrongType& other) const noexcept {
        return value == other.value;
    }
    constexpr bool operator!=(const StrongType& other) const noexcept {
        return value != other.value;
    }
};

struct NodeIDTag {}; using NodeID = StrongType<uint16_t, NodeIDTag>;
struct BusGenTag {}; using BusGeneration = StrongType<uint32_t, BusGenTag>;
struct TLabelTag {}; using TLabel = StrongType<uint8_t, TLabelTag>;

// AR Response matching key (type-safe, prevents parameter order bugs)
struct MatchKey {
    NodeID node;
    BusGeneration generation;
    TLabel label;

    constexpr bool operator==(const MatchKey& other) const noexcept {
        return node == other.node &&
               generation == other.generation &&
               label == other.label;
    }
};

// =============================================================================
// Transaction State Machine (IEEE 1394 Protocol)
// =============================================================================

enum class TransactionState : uint8_t {
    Created,        // Transaction created but not submitted
    Submitted,      // Submitted to ATManager
    ATPosted,       // AT descriptor on hardware
    ATCompleted,    // ACK received (xferStatus valid) - gotAck() equivalent
    AwaitingAR,     // Waiting for AR response (ACK was pending) - waiting for gotPacket()
    ARReceived,     // AR response matched - gotPacket() called
    Completed,      // User callback invoked
    TimedOut,       // Timeout exceeded
    Failed,         // Error occurred
    Cancelled       // User or bus reset cancelled
};

// Compile-time state transition validation (encodes IEEE 1394 protocol)
constexpr bool IsValidTransition(TransactionState from, TransactionState to) noexcept {
    switch (from) {
        case TransactionState::Created:
            return to == TransactionState::Submitted;

        case TransactionState::Submitted:
            return to == TransactionState::ATPosted;

        case TransactionState::ATPosted:
            return to == TransactionState::ATCompleted ||
                   to == TransactionState::Failed ||
                   to == TransactionState::TimedOut;

        case TransactionState::ATCompleted:
            // gotAck() logic: pending → wait for AR, complete → done
            return to == TransactionState::AwaitingAR ||     // ackCode==0x1 (pending)
                   to == TransactionState::Completed ||       // ackCode==0x0 (complete)
                   to == TransactionState::Failed ||          // ackCode error
                   to == TransactionState::TimedOut;          // Timeout

        case TransactionState::AwaitingAR:
            return to == TransactionState::ARReceived ||
                   to == TransactionState::TimedOut ||
                   to == TransactionState::Cancelled;

        case TransactionState::ARReceived:
            return to == TransactionState::Completed;

        case TransactionState::Completed:
        case TransactionState::TimedOut:
        case TransactionState::Failed:
        case TransactionState::Cancelled:
            return false;  // Terminal states - no transitions out
    }
    return false;
}

// Compile-time validation of state machine invariants
static_assert(!IsValidTransition(TransactionState::Completed, TransactionState::Created),
              "Cannot transition from terminal state");
static_assert(IsValidTransition(TransactionState::ATCompleted, TransactionState::AwaitingAR),
              "Split transactions must wait for AR response");
static_assert(!IsValidTransition(TransactionState::ARReceived, TransactionState::ATCompleted),
              "Cannot go backwards from AR to AT");

// State history for debugging (circular buffer)
struct TransactionStateHistory {
    TransactionState oldState;
    TransactionState newState;
    uint64_t timestampUs;
    const char* reason;
};

// =============================================================================
// Transaction Class (Single Source of Truth)
// =============================================================================

class Transaction {
public:
    Transaction(TLabel label, BusGeneration gen, NodeID nodeID) noexcept
        : label_(label), generation_(gen), nodeID_(nodeID) {}

    ~Transaction() = default;

    // Core accessors (tLabel is the identifier)
    [[nodiscard]] TLabel label() const noexcept { return label_; }
    [[nodiscard]] BusGeneration generation() const noexcept { return generation_; }
    [[nodiscard]] NodeID nodeID() const noexcept { return nodeID_; }
    [[nodiscard]] TransactionState state() const noexcept { return state_; }

    // MatchKey for AR response matching
    [[nodiscard]] MatchKey GetMatchKey() const noexcept {
        return MatchKey{nodeID_, generation_, label_};
    }

    // State transitions with compile-time and runtime validation
    void TransitionTo(TransactionState newState, const char* reason) noexcept;

    // ACK code (from AT completion - Phase 0.5.2 fix moved here!)
    [[nodiscard]] uint8_t ackCode() const noexcept { return ackCode_; }
    void SetAckCode(uint8_t ack) noexcept { ackCode_ = ack; }

    // Transaction code (tCode) for distinguishing read vs write operations
    [[nodiscard]] uint8_t tCode() const noexcept { return tCode_; }
    void SetTCode(uint8_t tcode) noexcept { tCode_ = tcode; }

    // Check if this is a read operation (per IEEE 1394-1995 §6.2)
    // Read operations must ALWAYS wait for AR response regardless of AT ack code
    [[nodiscard]] bool IsReadOperation() const noexcept {
        return tCode_ == 0x4 ||  // Read quadlet request
               tCode_ == 0x5;     // Read block request
    }

    // Completion strategy (explicit two-path model)
    [[nodiscard]] CompletionStrategy GetCompletionStrategy() const noexcept { return completionStrategy_; }
    void SetCompletionStrategy(CompletionStrategy strategy) noexcept { completionStrategy_ = strategy; }

    // Check if AT completion should skip processing (for CompleteOnAR transactions)
    [[nodiscard]] bool ShouldSkipATCompletion() const noexcept {
        return skipATCompletion_;
    }
    void SetSkipATCompletion(bool skip) noexcept {
        skipATCompletion_ = skip;
    }

    // Payload management (Phase 1.3: UniquePayload ownership)
    /// Get raw pointer to payload (for backwards compatibility)
    /// Returns nullptr if no payload or payload is invalid
    [[nodiscard]] PayloadHandle* payload() noexcept {
        return payload_.IsValid() ? &payload_.Get() : nullptr;
    }

    /// Get const pointer to payload (for reading)
    [[nodiscard]] const PayloadHandle* payload() const noexcept {
        return payload_.IsValid() ? &payload_.Get() : nullptr;
    }

    /// Set payload (transfers ownership to transaction)
    /// Old payload automatically released before taking ownership of new one
    void SetPayload(UniquePayload<PayloadHandle>&& p) noexcept {
        payload_ = std::move(p);
    }

    /// Get reference to UniquePayload wrapper (for move operations)
    [[nodiscard]] UniquePayload<PayloadHandle>& GetPayload() noexcept {
        return payload_;
    }

    /// Check if transaction has a valid payload
    [[nodiscard]] bool HasPayload() const noexcept {
        return payload_.IsValid();
    }

    // Timing
    [[nodiscard]] uint64_t submittedAtUs() const noexcept { return submittedAtUs_; }
    [[nodiscard]] uint64_t completedAtUs() const noexcept { return completedAtUs_; }
    [[nodiscard]] uint32_t timeoutMs() const noexcept { return timeoutMs_; }
    [[nodiscard]] uint64_t deadlineUs() const noexcept { return deadlineUs_; }  // Phase 2.0

    void SetSubmittedAt(uint64_t us) noexcept { submittedAtUs_ = us; }
    void SetCompletedAt(uint64_t us) noexcept { completedAtUs_ = us; }
    void SetTimeout(uint32_t ms) noexcept { timeoutMs_ = ms; }
    void SetDeadline(uint64_t us) noexcept { deadlineUs_ = us; }  // Phase 2.0

    // Response handling (concept-validated callback)
    template<typename F>
    void SetResponseHandler(F&& handler) noexcept {
        ASFW_LOG(Async, "🔍 [SetResponseHandler] tLabel=%u this=%p BEFORE: responseHandler_=%d",
                 label_.value, this, responseHandler_ ? 1 : 0);
        responseHandler_ = std::forward<F>(handler);
        ASFW_LOG(Async, "🔍 [SetResponseHandler] tLabel=%u this=%p AFTER: responseHandler_=%d",
                 label_.value, this, responseHandler_ ? 1 : 0);
    }

    void InvokeResponseHandler(kern_return_t kr, std::span<const uint8_t> data) noexcept {
        ASFW_LOG(Async, "🔍 [InvokeResponseHandler] tLabel=%u this=%p responseHandler_valid=%d kr=0x%x dataLen=%zu",
                 label_.value, this, responseHandler_ ? 1 : 0, kr, data.size());
        if (responseHandler_) {
            ASFW_LOG(Async, "🔍 [InvokeResponseHandler] Invoking responseHandler_ for tLabel=%u", label_.value);
            responseHandler_(kr, data);
            ASFW_LOG(Async, "🔍 [InvokeResponseHandler] responseHandler_ returned for tLabel=%u", label_.value);
        } else {
            ASFW_LOG(Async, "⚠️ [InvokeResponseHandler] responseHandler_ is NULL for tLabel=%u!", label_.value);
        }
    }

    // Retry tracking
    [[nodiscard]] uint8_t retryCount() const noexcept { return retryCount_; }
    void IncrementRetry() noexcept { retryCount_++; }

    // Debugging: state history
    [[nodiscard]] std::span<const TransactionStateHistory> GetHistory() const noexcept;
    void DumpHistory() const noexcept;

    // Non-copyable, non-movable (managed by TransactionManager)
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;

private:
    void ReleaseResources() noexcept;

    // Identification (tLabel is the identifier - matches Apple's pattern)
    TLabel label_;
    BusGeneration generation_;
    NodeID nodeID_;

    // State
    TransactionState state_{TransactionState::Created};
    uint8_t ackCode_{0};        // From AT completion (for timeout retry)
    uint8_t tCode_{0};          // Transaction code (0x4=read quad, 0x5=read block, etc.)
    uint8_t retryCount_{0};
    CompletionStrategy completionStrategy_{CompletionStrategy::CompleteOnAT};  // Explicit two-path model
    bool skipATCompletion_{false};  // For CompleteOnAR transactions

    // Resources (Phase 1.3: UniquePayload for automatic cleanup)
    UniquePayload<PayloadHandle> payload_;  // Automatically released on destruction
    std::function<void(kern_return_t, std::span<const uint8_t>)> responseHandler_;

    // Timing
    uint64_t submittedAtUs_{0};
    uint64_t completedAtUs_{0};
    uint32_t timeoutMs_{0};
    uint64_t deadlineUs_{0};  // Phase 2.0: Absolute timeout deadline

    // Debugging: circular buffer for last 16 state transitions
    TransactionStateHistory history_[16];
    uint8_t historyIdx_{0};
};

// Helper: Convert state to string for logging
[[nodiscard]] const char* ToString(TransactionState state) noexcept;

} // namespace ASFW::Async
