#pragma once

#include <memory>
#include <unordered_map>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOReturn.h>

#include "Transaction.hpp"
#include "Error.hpp"  // Phase 2.1: Rich error context

namespace ASFW::Async {

/**
 * \brief Manages lifecycle of all in-flight transactions.
 *
 * Single source of truth for transaction state. Replaces the scattered
 * state tracking across OutstandingTable, TimeoutEngine, PayloadRegistry.
 *
 * **Thread Safety**
 * All operations are serialized via internal IOLock.
 *
 * **Error Handling (Phase 2.1)**
 * Uses Result<T, Error> for rich error context with source location tracking.
 * Errors include file, line, function, and human-readable messages.
 *
 * **Design**
 * - Allocate(): Create new transaction with unique txid
 * - Find(): Lookup by txid or tLabel
 * - Remove(): Delete completed/failed transactions
 * - State transitions tracked via Transaction::TransitionTo()
 *
 * **Migration Path**
 * Phase 1.1 introduces this alongside OutstandingTable. Later phases
 * will deprecate OutstandingTable entirely.
 */
class TransactionManager {
public:
    TransactionManager() = default;
    ~TransactionManager();

    /**
     * \brief Initialize transaction manager.
     *
     * \return Result<void> - Success or error with context
     *
     * **Example**
     * \code
     * auto result = txnMgr->Initialize();
     * if (!result) {
     *     result.error().Log();  // Logs: "Initialize failed at TransactionManager.cpp:42 - No memory for lock"
     *     return result.error().kr;
     * }
     * \endcode
     */
    [[nodiscard]] Result<void> Initialize() noexcept;

    /**
     * \brief Shut down transaction manager and cancel all transactions.
     */
    void Shutdown() noexcept;

    /**
     * \brief Allocate new transaction at tLabel index.
     *
     * \param label FireWire tLabel (0-63) - used as array index
     * \param generation Bus generation
     * \param nodeID Destination node ID
     * \return Result<Transaction*> - Transaction pointer or error with context
     *
     * **Error Cases**
     * - label >= 64: ASFW_ERROR_INVALID("tLabel out of range")
     * - Not initialized: ASFW_ERROR_NOT_READY("TransactionManager not initialized")
     * - Allocation failed: ASFW_ERROR_NO_MEMORY("Failed to allocate Transaction")
     * - Slot occupied: ASFW_ERROR_INVALID("Transaction with tLabel=... already exists")
     *
     * **Thread Safety**
     * Serialized via lock. Safe to call concurrently.
     *
     * **Example**
     * \code
     * auto result = txnMgr->Allocate(label, gen, nodeID);
     * if (!result) {
     *     result.error().Log();
     *     return result.error().kr;
     * }
     * Transaction* txn = result.value();  // or *result
     * \endcode
     */
    [[nodiscard]] Result<Transaction*>
    Allocate(TLabel label, BusGeneration generation, NodeID nodeID) noexcept;

    /**
     * \brief Find transaction by tLabel.
     *
     * \param label FireWire tLabel (0-63)
     * \return Transaction pointer or nullptr if not found
     *
     * **Thread Safety**
     * Caller must hold lock or ensure transaction won't be deleted.
     * For safe access, use WithTransaction() instead.
     */
    [[nodiscard]] Transaction* Find(TLabel label) noexcept;

    // Alias for backwards compatibility
    [[nodiscard]] Transaction* FindByLabel(TLabel label) noexcept { return Find(label); }

    /**
     * \brief Find transaction by MatchKey (for AR response matching).
     *
     * \param key Match key (nodeID + generation + tLabel)
     * \return Transaction pointer or nullptr if not found
     *
     * **Thread Safety**
     * Caller must hold lock or ensure transaction won't be deleted.
     */
    [[nodiscard]] Transaction* FindByMatchKey(const MatchKey& key) noexcept;

    /**
     * \brief Execute callback with transaction under lock.
     *
     * \param label Transaction label (0-63)
     * \param fn Callback to invoke with transaction
     * \return true if transaction found and callback invoked, false otherwise
     *
     * **Example**
     * \code
     * txnMgr->WithTransaction(label, [](Transaction* txn) {
     *     txn->TransitionTo(TransactionState::ATCompleted, "OnATCompletion");
     *     txn->SetAckCode(ackCode);
     * });
     * \endcode
     */
    template<typename F>
    bool WithTransaction(TLabel label, F&& fn) noexcept;

    /**
     * \brief Alias for WithTransaction (backwards compatibility).
     */
    template<typename F>
    bool WithTransactionByLabel(TLabel label, F&& fn) noexcept;

    /**
     * \brief Remove transaction from manager.
     *
     * \param label tLabel (0-63) of transaction to remove
     *
     * **Lifecycle**
     * Called after transaction reaches terminal state (Completed, Failed, etc.)
     * to free resources.
     *
     * **Thread Safety**
     * Serialized via lock. Safe to call concurrently.
     */
    void Remove(TLabel label) noexcept;

    /**
     * \brief Cancel all transactions.
     *
     * **Usage**
     * Called on bus reset or driver shutdown.
     */
    void CancelAll() noexcept;

    /**
     * \brief Extract transaction from manager (transfer ownership).
     *
     * \param label tLabel (0-63) of transaction to extract
     * \return Unique pointer to transaction, or nullptr if not found
     *
     * **Usage**
     * Use this to remove a transaction from the manager BEFORE invoking
     * callbacks that might re-enter the manager (e.g. Retry -> Allocate).
     * This prevents deadlocks.
     */
    [[nodiscard]] std::unique_ptr<Transaction> Extract(TLabel label) noexcept;

    /**
     * \brief Get count of in-flight transactions.
     */
    [[nodiscard]] size_t Count() const noexcept;

    /**
     * \brief Iterate over all transactions.
     *
     * \param fn Callback to invoke for each transaction
     *
     * **Usage (Phase 2.0)**
     * Used by OnTimeoutTick to check all transactions for expiration.
     *
     * **Example**
     * \code
     * txnMgr->ForEachTransaction([](Transaction* txn) {
     *     if (txn->deadlineUs() < nowUs) {
     *         // Handle timeout
     *     }
     * });
     * \endcode
     *
     * **Thread Safety**
     * Holds lock during entire iteration. Keep callback fast!
     */
    template<typename F>
    void ForEachTransaction(F&& fn) noexcept;

    /**
     * \brief Dump all transaction states for debugging.
     */
    void DumpAll() const noexcept;

    // Non-copyable, non-movable
    TransactionManager(const TransactionManager&) = delete;
    TransactionManager& operator=(const TransactionManager&) = delete;
    TransactionManager(TransactionManager&&) = delete;
    TransactionManager& operator=(TransactionManager&&) = delete;

private:
    // Apple's pattern: Array indexed by tLabel (0-63)
    // Matches AsyncPendingTrans fPendingQ[64] from AppleFWOHCI.kext
    std::array<std::unique_ptr<Transaction>, 64> transactions_;

    IOLock* lock_{nullptr};
    bool initialized_{false};
};

// =============================================================================
// Template Implementation
// =============================================================================

template<typename F>
bool TransactionManager::WithTransaction(TLabel label, F&& fn) noexcept {
    if (!lock_ || !initialized_) {
        return false;
    }

    if (label.value >= 64) {
        return false;
    }

    IOLockLock(lock_);

    auto& txn = transactions_[label.value];
    if (!txn) {
        IOLockUnlock(lock_);
        return false;
    }

    fn(txn.get());
    IOLockUnlock(lock_);
    return true;
}

template<typename F>
bool TransactionManager::WithTransactionByLabel(TLabel label, F&& fn) noexcept {
    // WithTransactionByLabel is now identical to WithTransaction
    return WithTransaction(label, std::forward<F>(fn));
}

template<typename F>
void TransactionManager::ForEachTransaction(F&& fn) noexcept {
    if (!lock_ || !initialized_) {
        return;
    }

    IOLockLock(lock_);

    for (auto& txn : transactions_) {
        if (txn) {
            fn(txn.get());
        }
    }

    IOLockUnlock(lock_);
}

} // namespace ASFW::Async
