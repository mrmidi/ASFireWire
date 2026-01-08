#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>

#ifdef ASFW_HOST_TEST
#include "../../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODataQueueDispatchSource.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSSharedPtr.h>
#endif

#include "../../Logging/Logging.hpp"

namespace ASFW::Shared {

/**
 * CompletionToken concept - Requires trivially copyable types for IODataQueue
 *
 * Any type used with CompletionQueue must be:
 * - Trivially copyable (can be safely memcpy'd)
 * - 4-byte aligned (IODataQueue requirement)
 * - Size must be multiple of 4 bytes
 */
template<typename T>
concept CompletionToken = std::is_trivially_copyable_v<T> &&
                          (sizeof(T) % 4 == 0) &&
                          alignof(T) >= 4;

/**
 * CompletionQueue - Generic SPSC queue for completion tokens
 *
 * Thread-safe single-producer single-consumer queue using IODataQueueDispatchSource.
 * Provides atomic guards to prevent crashes when consumer isn't ready.
 *
 * @tparam TokenT Completion token type (must satisfy CompletionToken concept)
 *
 * Usage:
 *   1. Create with CompletionQueue<MyToken>::Create(...)
 *   2. Call Activate() when consumer is ready
 *   3. Push tokens from producer (typically IRQ context)
 *   4. Call Deactivate() before shutdown
 */
template<CompletionToken TokenT>
class CompletionQueue {
public:
    using Token = TokenT;

    /**
     * Create a completion queue
     *
     * @param consumerQueue Dispatch queue that will consume tokens
     * @param capacityBytes Queue capacity in bytes
     * @param dataAvailableAction Action to invoke when data is available
     * @param outQueue Output parameter for created queue
     * @return kIOReturnSuccess on success, error code otherwise
     */
    static kern_return_t Create(::IODispatchQueue* consumerQueue,
                                size_t capacityBytes,
                                OSAction* dataAvailableAction,
                                std::unique_ptr<CompletionQueue<TokenT>>& outQueue) {
        if (consumerQueue == nullptr || dataAvailableAction == nullptr || capacityBytes == 0) {
            return kIOReturnBadArgument;
        }

        IODataQueueDispatchSource* rawSource = nullptr;
        kern_return_t kr = IODataQueueDispatchSource::Create(capacityBytes, consumerQueue, &rawSource);
        if (kr != kIOReturnSuccess || rawSource == nullptr) {
            if (kr == kIOReturnSuccess) {
                kr = kIOReturnNoMemory;
            }
            ASFW_LOG(Async, "CompletionQueue: failed to create IODataQueueDispatchSource (0x%x)", kr);
            return kr;
        }

        // NOTE: IODataQueueDispatchSource handles notifications automatically via shared memory
        // No need to set a data available handler on the kernel side - client handles that
        // The OSAction parameter is unused for now but kept for future extensibility
        (void)dataAvailableAction;

        auto queue = std::unique_ptr<CompletionQueue<TokenT>>(new CompletionQueue<TokenT>());
        queue->source_ = OSSharedPtr(rawSource, OSNoRetain);
        queue->capacityBytes_ = capacityBytes;
        outQueue = std::move(queue);
        return kIOReturnSuccess;
    }

    ~CompletionQueue() {
        if (source_) {
            source_->SetEnable(false);
            source_->Cancel(nullptr);
            source_.reset();
        }
    }

    /**
     * Activate queue (must be called after Create, before any Push calls)
     */
    void Activate() noexcept {
        dqActive_.store(true, std::memory_order_release);
        // CRITICAL: Enable the dispatch source NOW that client is ready to receive notifications
        if (source_) {
            kern_return_t kr = source_->SetEnable(true);
            if (kr != kIOReturnSuccess) {
                ASFW_LOG(Async, "CompletionQueue::Activate() - SetEnable failed: 0x%x", kr);
            }
        }
        ASFW_LOG(Async, "CompletionQueue::Activate() - queue now active");
    }

    /**
     * Deactivate queue (must be called before stopping producers)
     */
    void Deactivate() noexcept {
        dqActive_.store(false, std::memory_order_release);
        ASFW_LOG(Async, "CompletionQueue::Deactivate() - queue now inactive");
        // CRITICAL: Disable and cancel notifications during runtime teardown
        if (source_) {
            source_->SetEnable(false);
            source_->Cancel(nullptr);
        }
    }

    /**
     * Mark that client is bound (set when dataAvailable handler is installed)
     */
    void SetClientBound() noexcept {
        clientBound_.store(true, std::memory_order_release);
        ASFW_LOG(Async, "CompletionQueue::SetClientBound() - client now bound");
    }

    /**
     * Mark that client is unbound (called during teardown)
     */
    void SetClientUnbound() noexcept {
        clientBound_.store(false, std::memory_order_release);
        ASFW_LOG(Async, "CompletionQueue::SetClientUnbound() - client now unbound");
        // CRITICAL: Disable notifications during teardown
        if (source_) {
            source_->SetEnable(false);
        }
    }

    /**
     * Push a completion token onto the queue
     *
     * Thread-safe producer operation. Typically called from IRQ context.
     *
     * @param token Token to enqueue
     * @return true if enqueued successfully, false if queue is full or not ready
     */
    [[nodiscard]] bool Push(const TokenT& token) noexcept {
        // CRITICAL: Gate enqueue to prevent crashes when consumer isn't ready
        // This prevents the SIGABRT in IODataQueueDispatchSource::Enqueue that occurs
        // when trying to signal a data-available event to an unactivated/unbound queue

        if (!source_) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // Check atomic guards (acquire semantics to see latest state)
        if (!dqActive_.load(std::memory_order_acquire) ||
            !clientBound_.load(std::memory_order_acquire)) {
            // Queue is not ready to accept enqueues
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // Validate token size contract against ACTUAL capacity
        constexpr size_t kTokenSize = sizeof(TokenT);
        static_assert((kTokenSize % 4) == 0, "Token must be 4-byte aligned");
        static_assert(kTokenSize > 0, "Token must have non-zero size");

        // Validate against actual capacity
        if (kTokenSize == 0 || kTokenSize > capacityBytes_) {
            oversizeDropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // Attempt enqueue with defensive lambda
        kern_return_t ret = source_->Enqueue(static_cast<unsigned>(kTokenSize),
                                             ^(void* buffer, size_t size) {
            // Diagnostic logging inside lambda to see actual size provided
            ASFW_LOG(Async, "CompletionQueue::Push: Lambda invoked - requested=%zu actual=%zu",
                     kTokenSize, size);

            if (size >= kTokenSize) {
                std::memcpy(buffer, &token, kTokenSize);
            } else {
                // CRITICAL: Don't crash, just log the mismatch
                ASFW_LOG(Async, "❌ CompletionQueue::Push: SIZE MISMATCH! requested=%zu actual=%zu - DROPPING",
                         kTokenSize, size);
            }
        });

        ASFW_LOG(Async, "CompletionQueue::Push: Enqueue returned - ret=0x%x", ret);

        if (ret == kIOReturnSuccess) {
            ASFW_LOG(Async, "CompletionQueue::Push: SUCCESS - token enqueued");
            return true;
        }

        if (ret == kIOReturnOverrun || ret == kIOReturnNoSpace) {
            // Queue full - this is expected under heavy load
            ASFW_LOG(Async, "CompletionQueue::Push: QUEUE FULL - ret=0x%x", ret);
            dropped_.fetch_add(1, std::memory_order_relaxed);
        } else if (ret != kIOReturnNotReady) {
            // Other errors (notReady means queue deactivated, which we already checked)
            // Log but don't crash
            ASFW_LOG(Async, "CompletionQueue::Push: ENQUEUE FAILED - ret=0x%x", ret);
            dropped_.fetch_add(1, std::memory_order_relaxed);
        } else {
            ASFW_LOG(Async, "CompletionQueue::Push: NOT READY - ret=0x%x", ret);
        }
        return false;
    }

    /**
     * Get the underlying IODataQueueDispatchSource
     */
    IODataQueueDispatchSource* GetSource() const { return source_.get(); }

    /**
     * Get statistics
     */
    uint64_t DroppedCount() const { return dropped_.load(std::memory_order_relaxed); }
    uint64_t OversizeDroppedCount() const { return oversizeDropped_.load(std::memory_order_relaxed); }

private:
    CompletionQueue() = default;

    OSSharedPtr<IODataQueueDispatchSource> source_{};

    // Remember the actual capacity passed to Create() for validation
    size_t capacityBytes_{0};

    // Guards to prevent enqueueing when consumer isn't ready
    std::atomic<bool> dqActive_{false};
    std::atomic<bool> clientBound_{false};

    // Statistics
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> oversizeDropped_{0};

    // Disable copy/move
    CompletionQueue(const CompletionQueue&) = delete;
    CompletionQueue& operator=(const CompletionQueue&) = delete;
    CompletionQueue(CompletionQueue&&) = delete;
    CompletionQueue& operator=(CompletionQueue&&) = delete;
};

} // namespace ASFW::Shared
