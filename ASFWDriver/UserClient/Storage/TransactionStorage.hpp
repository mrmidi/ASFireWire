//
//  TransactionStorage.hpp
//  ASFWDriver
//
//  Storage for completed async transaction results
//

#ifndef ASFW_USERCLIENT_TRANSACTION_STORAGE_HPP
#define ASFW_USERCLIENT_TRANSACTION_STORAGE_HPP

#include <cstdint>
#include <cstddef>

// Forward declarations
struct IOLock;

namespace ASFW::UserClient {

// Transaction result entry
struct TransactionResult {
    uint16_t handle{0};
    uint32_t status{0};  // AsyncStatus value
    uint8_t responseCode{0xFF};
    uint32_t dataLength{0};
    uint8_t data[512]{};  // Max response data size
};

// Ring buffer storage for completed transaction results
class TransactionStorage {
public:
    static constexpr size_t kMaxCompletedTransactions = 16;

    TransactionStorage();
    ~TransactionStorage();

    // Disable copy/move
    TransactionStorage(const TransactionStorage&) = delete;
    TransactionStorage& operator=(const TransactionStorage&) = delete;

    // Check if storage is valid (lock allocated)
    bool IsValid() const { return completedLock_ != nullptr; }

    // Store a completed transaction result
    // Returns true if stored, false if buffer full (oldest dropped)
    bool StoreResult(uint16_t handle, uint32_t status, uint8_t responseCode,
                    const void* responsePayload, uint32_t responseLength);

    // Find and retrieve a result by handle
    // Returns nullptr if not found
    // Caller must hold the lock when accessing the result
    TransactionResult* FindResult(uint16_t handle, size_t* outIndex = nullptr);

    // Remove a result at the given index
    void RemoveResultAtIndex(size_t index);

    // Lock/unlock for thread-safe access
    void Lock();
    void Unlock();

private:
    TransactionResult completedTransactions_[kMaxCompletedTransactions];
    size_t completedHead_{0};  // Next slot to write
    size_t completedTail_{0};  // Oldest unread result
    IOLock* completedLock_{nullptr};
};

} // namespace ASFW::UserClient

#endif // ASFW_USERCLIENT_TRANSACTION_STORAGE_HPP
