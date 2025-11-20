#pragma once

#include "../Core/CompletionStrategy.hpp"
#include "AsyncCommand.hpp"

namespace ASFW::Async {

/**
 * @brief Base class for commands that complete on AR response only.
 *
 * Explicit type for read operations (read quadlet, read block, lock).
 * Automatically sets CompletionStrategy::CompleteOnAR.
 *
 * Compile-time guarantees:
 * - OnATCompletion() will NOT complete the transaction
 * - OnARResponse() is REQUIRED to complete
 * - Satisfies ARCompletingTransaction concept
 *
 * Reference: Apple's IOFWReadQuadCommand pattern
 * - gotAck(): stores ack code, doesn't complete
 * - gotPacket(): completes with response data
 *
 * Usage:
 * @code
 * class ReadQuadCommand : public ARCompletingCommand<ReadQuadCommand> {
 * public:
 *     ReadQuadCommand(ReadParams params, CompletionCallback callback)
 *         : ARCompletingCommand(std::move(callback)), params_(params) {}
 *
 *     TxMetadata BuildMetadata(const TransactionContext& txCtx) override {
 *         TxMetadata meta{};
 *         meta.tCode = 0x4;  // Read quadlet
 *         // completionStrategy already set to CompleteOnAR by base class
 *         return meta;
 *     }
 * };
 * @endcode
 */
template<typename Derived>
class ARCompletingCommand : public AsyncCommand<Derived> {
public:
    /**
     * @brief Get completion strategy (always CompleteOnAR).
     * @return CompletionStrategy::CompleteOnAR
     */
    static constexpr CompletionStrategy GetCompletionStrategy() noexcept {
        return CompletionStrategy::CompleteOnAR;
    }

    /**
     * @brief Check if this command requires AR response (always true).
     * @return true
     */
    static constexpr bool RequiresARResponse() noexcept {
        return true;
    }

    /**
     * @brief Check if AT completion should complete transaction (always false).
     * @return false
     */
    static constexpr bool CompletesOnATAck() noexcept {
        return false;
    }

protected:
    // Constructor for derived classes
    explicit ARCompletingCommand(CompletionCallback callback)
        : AsyncCommand<Derived>(std::move(callback)) {}
};

/**
 * @brief Base class for commands that complete on AT acknowledgment only.
 *
 * Explicit type for write operations that don't expect deferred response.
 * Automatically sets CompletionStrategy::CompleteOnAT.
 *
 * Compile-time guarantees:
 * - OnATCompletion() WILL complete the transaction
 * - OnARResponse() is unexpected (unified transaction)
 * - Satisfies ATCompletingTransaction concept
 *
 * Reference: Apple's IOFWWriteQuadCommand pattern
 * - gotAck(): completes immediately if ack_complete
 *
 * Usage:
 * @code
 * class WriteQuadCommand : public ATCompletingCommand<WriteQuadCommand> {
 * public:
 *     WriteQuadCommand(WriteParams params, CompletionCallback callback)
 *         : ATCompletingCommand(std::move(callback)), params_(params) {}
 *
 *     TxMetadata BuildMetadata(const TransactionContext& txCtx) override {
 *         TxMetadata meta{};
 *         meta.tCode = 0x0;  // Write quadlet
 *         // completionStrategy already set to CompleteOnAT by base class
 *         return meta;
 *     }
 * };
 * @endcode
 */
template<typename Derived>
class ATCompletingCommand : public AsyncCommand<Derived> {
public:
    /**
     * @brief Get completion strategy (always CompleteOnAT).
     * @return CompletionStrategy::CompleteOnAT
     */
    static constexpr CompletionStrategy GetCompletionStrategy() noexcept {
        return CompletionStrategy::CompleteOnAT;
    }

    /**
     * @brief Check if this command requires AR response (always false).
     * @return false
     */
    static constexpr bool RequiresARResponse() noexcept {
        return false;
    }

    /**
     * @brief Check if AT completion should complete transaction (always true).
     * @return true
     */
    static constexpr bool CompletesOnATAck() noexcept {
        return true;
    }

protected:
    // Constructor for derived classes
    explicit ATCompletingCommand(CompletionCallback callback)
        : AsyncCommand<Derived>(std::move(callback)) {}
};

/**
 * @brief Base class for commands that require both AT and AR paths.
 *
 * Explicit type for complex operations (deferred writes, multi-phase locks).
 * Automatically sets CompletionStrategy::RequireBoth.
 *
 * Compile-time guarantees:
 * - OnATCompletion() will validate ack, transition to AwaitingAR
 * - OnARResponse() is REQUIRED to complete
 *
 * Usage:
 * @code
 * class DeferredWriteCommand : public DualPathCommand<DeferredWriteCommand> {
 * public:
 *     DeferredWriteCommand(WriteParams params, CompletionCallback callback)
 *         : DualPathCommand(std::move(callback)), params_(params) {}
 *
 *     TxMetadata BuildMetadata(const TransactionContext& txCtx) override {
 *         TxMetadata meta{};
 *         meta.tCode = 0x1;  // Write block
 *         // completionStrategy already set to RequireBoth by base class
 *         return meta;
 *     }
 * };
 * @endcode
 */
template<typename Derived>
class DualPathCommand : public AsyncCommand<Derived> {
public:
    /**
     * @brief Get completion strategy (always RequireBoth).
     * @return CompletionStrategy::RequireBoth
     */
    static constexpr CompletionStrategy GetCompletionStrategy() noexcept {
        return CompletionStrategy::RequireBoth;
    }

    /**
     * @brief Check if this command requires AR response (always true).
     * @return true
     */
    static constexpr bool RequiresARResponse() noexcept {
        return true;
    }

    /**
     * @brief Check if AT completion should complete transaction (always false).
     * @return false
     */
    static constexpr bool CompletesOnATAck() noexcept {
        return false;
    }

protected:
    // Constructor for derived classes
    explicit DualPathCommand(CompletionCallback callback)
        : AsyncCommand<Derived>(std::move(callback)) {}
};

template<typename Derived>
class PHYCommand : public AsyncCommand<Derived> {
public:
    static constexpr CompletionStrategy GetCompletionStrategy() noexcept {
        return CompletionStrategy::CompleteOnPHY;
    }

    static constexpr bool RequiresARResponse() noexcept {
        return false;
    }

    static constexpr bool CompletesOnATAck() noexcept {
        return true;
    }

protected:
    explicit PHYCommand(CompletionCallback callback)
        : AsyncCommand<Derived>(std::move(callback)) {}
};

// Compile-time validation using concepts
namespace detail {
    // Mock transaction for concept checking
    struct MockARTransaction {
        static constexpr CompletionStrategy GetCompletionStrategy() noexcept {
            return CompletionStrategy::CompleteOnAR;
        }
    };

    struct MockATTransaction {
        static constexpr CompletionStrategy GetCompletionStrategy() noexcept {
            return CompletionStrategy::CompleteOnAT;
        }
    };

    struct MockPHYTransaction {
        static constexpr CompletionStrategy GetCompletionStrategy() noexcept {
            return CompletionStrategy::CompleteOnPHY;
        }
    };
}

// Verify concepts work correctly
static_assert(ARCompletingTransaction<detail::MockARTransaction>,
              "MockARTransaction should satisfy ARCompletingTransaction");
static_assert(ATCompletingTransaction<detail::MockATTransaction>,
              "MockATTransaction should satisfy ATCompletingTransaction");
static_assert(!ATCompletingTransaction<detail::MockARTransaction>,
              "MockARTransaction should NOT satisfy ATCompletingTransaction");
static_assert(PHYCompletingTransaction<detail::MockPHYTransaction>,
              "MockPHYTransaction should satisfy PHYCompletingTransaction");
static_assert(!PHYCompletingTransaction<detail::MockARTransaction>,
              "MockARTransaction should NOT satisfy PHYCompletingTransaction");

} // namespace ASFW::Async
