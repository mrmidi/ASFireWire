//
//  TransactionStorage.cpp
//  ASFWDriver
//
//  Storage for completed async transaction results
//

#include "TransactionStorage.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/IOLib.h>
#include <algorithm>
#include <cstring>

namespace ASFW::UserClient {

TransactionStorage::TransactionStorage() {
    completedLock_ = IOLockAlloc();
}

TransactionStorage::~TransactionStorage() {
    if (completedLock_) {
        IOLockFree(completedLock_);
        completedLock_ = nullptr;
    }
}

bool TransactionStorage::StoreResult(uint16_t handle, uint32_t status,
                                     const void* responsePayload,
                                     uint32_t responseLength) {
    if (!completedLock_) {
        return false;
    }

    IOLockLock(completedLock_);

    // Calculate next head position
    size_t nextHead = (completedHead_ + 1) % kMaxCompletedTransactions;

    bool droppedOldest = false;
    // If buffer is full, drop oldest result
    if (nextHead == completedTail_) {
        completedTail_ = (completedTail_ + 1) % kMaxCompletedTransactions;
        droppedOldest = true;
        ASFW_LOG(UserClient, "TransactionStorage: Dropped oldest result (buffer full)");
    }

    // Store result
    TransactionResult& result = completedTransactions_[completedHead_];
    result.handle = handle;
    result.status = status;
    result.dataLength = (responseLength > 512) ? 512 : responseLength;

    if (responsePayload && responseLength > 0 && result.dataLength > 0) {
        std::memcpy(result.data, responsePayload, result.dataLength);
    }

    completedHead_ = nextHead;

    IOLockUnlock(completedLock_);

    return !droppedOldest;
}

TransactionResult* TransactionStorage::FindResult(uint16_t handle, size_t* outIndex) {
    if (!completedLock_) {
        return nullptr;
    }

    // Caller must hold lock
    size_t index = completedTail_;
    while (index != completedHead_) {
        if (completedTransactions_[index].handle == handle) {
            if (outIndex) {
                *outIndex = index;
            }
            return &completedTransactions_[index];
        }
        index = (index + 1) % kMaxCompletedTransactions;
    }

    return nullptr;
}

void TransactionStorage::RemoveResultAtIndex(size_t index) {
    // Caller must hold lock

    // Only support removing from tail (oldest)
    if (index == completedTail_) {
        completedTail_ = (completedTail_ + 1) % kMaxCompletedTransactions;
    }
}

void TransactionStorage::Lock() {
    if (completedLock_) {
        IOLockLock(completedLock_);
    }
}

void TransactionStorage::Unlock() {
    if (completedLock_) {
        IOLockUnlock(completedLock_);
    }
}

} // namespace ASFW::UserClient
