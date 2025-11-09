#include "TransactionManager.hpp"

#include <DriverKit/IOLib.h>

namespace ASFW::Async {

TransactionManager::~TransactionManager() {
    if (initialized_) {
        Shutdown();
    }
}

Result<void> TransactionManager::Initialize() noexcept {
    if (initialized_) {
        return {};  // Already initialized, success
    }

    // Allocate lock
    lock_ = IOLockAlloc();
    if (!lock_) {
        return ASFW_ERROR_NO_MEMORY("Failed to allocate IOLock for TransactionManager");
    }

    // Initialize array to all nullptr
    for (auto& txn : transactions_) {
        txn = nullptr;
    }

    initialized_ = true;

    return {};  // Success
}

void TransactionManager::Shutdown() noexcept {
    if (!initialized_) {
        return;
    }

    // Cancel all transactions before shutting down
    CancelAll();

    // Free lock
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }

    initialized_ = false;
}

Result<Transaction*>
TransactionManager::Allocate(TLabel label, BusGeneration generation, NodeID nodeID) noexcept {
    if (!lock_ || !initialized_) {
        return ASFW_ERROR_NOT_READY("TransactionManager not initialized");
    }

    if (label.value >= 64) {
        return ASFW_ERROR_INVALID("tLabel must be 0-63");
    }

    IOLockLock(lock_);

    // Check if slot is already occupied
    if (transactions_[label.value]) {
        IOLockUnlock(lock_);
        return ASFW_ERROR_RECOVERABLE(kIOReturnBusy, "tLabel already in use (concurrent allocation)");
    }

    // Allocate new transaction (no txid needed - tLabel is the identifier)
    auto txn = std::make_unique<Transaction>(label, generation, nodeID);
    if (!txn) {
        IOLockUnlock(lock_);
        return ASFW_ERROR_NO_MEMORY("Failed to allocate Transaction object");
    }

    Transaction* result = txn.get();

    // Store in array at tLabel index
    transactions_[label.value] = std::move(txn);

    IOLockUnlock(lock_);

    return result;
}

Transaction* TransactionManager::Find(TLabel label) noexcept {
    if (!lock_ || !initialized_) {
        return nullptr;
    }

    if (label.value >= 64) {
        return nullptr;
    }

    return transactions_[label.value].get();
}

Transaction* TransactionManager::FindByMatchKey(const MatchKey& key) noexcept {
    if (!lock_ || !initialized_) {
        return nullptr;
    }

    // Fast path: lookup by tLabel (direct array access)
    Transaction* txn = Find(key.label);
    if (!txn) {
        return nullptr;
    }

    // Verify generation and nodeID match (for AR response validation)
    if (txn->generation() != key.generation || txn->nodeID() != key.node) {
        return nullptr;  // Stale transaction (bus reset or wrong node)
    }

    return txn;
}

void TransactionManager::Remove(TLabel label) noexcept {
    if (!lock_ || !initialized_) {
        ASFW_LOG(Async, "TransactionManager::Remove: not initialized");
        return;
    }

    if (label.value >= 64) {
        ASFW_LOG(Async, "TransactionManager::Remove: tLabel %u out of range", label.value);
        return;
    }

    IOLockLock(lock_);

    // Simply clear the slot (unique_ptr destructor handles cleanup)
    transactions_[label.value] = nullptr;

    IOLockUnlock(lock_);
}

void TransactionManager::CancelAll() noexcept {
    if (!lock_ || !initialized_) {
        return;
    }

    IOLockLock(lock_);

    // Transition all transactions to Cancelled state
    for (auto& txn : transactions_) {
        if (txn && 
            txn->state() != TransactionState::Completed &&
            txn->state() != TransactionState::Failed &&
            txn->state() != TransactionState::Cancelled) {
            txn->TransitionTo(TransactionState::Cancelled, "TransactionManager::CancelAll");

            // Invoke callback with cancellation error
            txn->InvokeResponseHandler(kIOReturnAborted, {});
        }
    }

    // Clear all slots
    for (auto& txn : transactions_) {
        txn = nullptr;
    }

    IOLockUnlock(lock_);
}

size_t TransactionManager::Count() const noexcept {
    if (!lock_ || !initialized_) {
        return 0;
    }

    size_t count = 0;
    for (const auto& txn : transactions_) {
        if (txn) {
            ++count;
        }
    }
    return count;
}

void TransactionManager::DumpAll() const noexcept {
    if (!lock_ || !initialized_) {
        return;
    }

    IOLockLock(lock_);

    size_t count = Count();
    IOLog("=== TransactionManager: %zu in-flight transactions ===\n", count);

    for (size_t i = 0; i < 64; ++i) {
        const auto& txn = transactions_[i];
        if (txn) {
            IOLog("  tLabel=%zu state=%{public}s nodeID=0x%04x gen=%u\n",
                  i,
                  ToString(txn->state()),
                  txn->nodeID().value,
                  txn->generation().value);

            // Dump recent state history
            txn->DumpHistory();
        }
    }

    IOLockUnlock(lock_);
}

} // namespace ASFW::Async
