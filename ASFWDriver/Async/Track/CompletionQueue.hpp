#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>

#include <DriverKit/IODataQueueDispatchSource.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSSharedPtr.h>

#include "AsyncTypes.hpp"
#include "OHCIEventCodes.hpp"
#include "../../Logging/Logging.hpp"

namespace ASFW::Async {

struct CompletionRecord {
    AsyncHandle handle{};
    OHCIEventCode eventCode{};
    uint32_t actualLength{0};
    uint16_t hardwareTimeStamp{0};

    static constexpr std::size_t kInlinePayloadSize = 16;
    std::byte inlinePayload[kInlinePayloadSize]{};
} __attribute__((aligned(4)));  // Ensure 4-byte alignment for IODataQueue

static_assert(std::is_trivially_copyable_v<CompletionRecord>,
              "CompletionRecord must be trivially copyable to enqueue across IODataQueue");
static_assert((sizeof(CompletionRecord) % 4) == 0,
              "CompletionRecord size must be a multiple of 4 bytes");

class CompletionQueue {
public:
    static kern_return_t Create(::IODispatchQueue* consumerQueue,
                                size_t capacityBytes,
                                OSAction* dataAvailableAction,
                                std::unique_ptr<CompletionQueue>& outQueue);

    ~CompletionQueue();

    // Activate queue (must be called after Create, before any Push calls)
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

    // Deactivate queue (must be called before stopping producers)
    void Deactivate() noexcept {
        dqActive_.store(false, std::memory_order_release);
        ASFW_LOG(Async, "CompletionQueue::Deactivate() - queue now inactive");
        // CRITICAL: Disable and cancel notifications during runtime teardown
        if (source_) {
            source_->SetEnable(false);
            source_->Cancel(nullptr);
        }
    }

    // Mark that client is bound (set when dataAvailable handler is installed)
    void SetClientBound() noexcept {
        clientBound_.store(true, std::memory_order_release);
        ASFW_LOG(Async, "CompletionQueue::SetClientBound() - client now bound");
    }

    // Mark that client is unbound (called during teardown)
    void SetClientUnbound() noexcept {
        clientBound_.store(false, std::memory_order_release);
        ASFW_LOG(Async, "CompletionQueue::SetClientUnbound() - client now unbound");
        // CRITICAL: Disable notifications during teardown
        if (source_) {
            source_->SetEnable(false);
        }
    }

    [[nodiscard]] bool Push(const CompletionRecord& record) noexcept;

    IODataQueueDispatchSource* GetSource() const { return source_.get(); }

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
};

} // namespace ASFW::Async

inline kern_return_t ASFW::Async::CompletionQueue::Create(::IODispatchQueue* consumerQueue,
                                                          size_t capacityBytes,
                                                          OSAction* dataAvailableAction,
                                                          std::unique_ptr<CompletionQueue>& outQueue) {
    if (consumerQueue == nullptr || dataAvailableAction == nullptr || capacityBytes == 0) {
        return kIOReturnBadArgument;
    }

    IODataQueueDispatchSource* rawSource = nullptr;
    kern_return_t kr = IODataQueueDispatchSource::Create(capacityBytes, consumerQueue, &rawSource);
    if (kr != kIOReturnSuccess || rawSource == nullptr) {
        if (kr == kIOReturnSuccess) {
            kr = kIOReturnNoMemory;
        }
        IOLog("CompletionQueue: failed to create IODataQueueDispatchSource (0x%x)\n", kr);
        return kr;
    }

    // NOTE: IODataQueueDispatchSource handles notifications automatically via shared memory
    // No need to set a data available handler on the kernel side - client handles that
    // The OSAction parameter is unused for now but kept for future extensibility
    (void)dataAvailableAction;

    auto queue = std::unique_ptr<CompletionQueue>(new CompletionQueue());
    queue->source_ = OSSharedPtr(rawSource, OSNoRetain);
    queue->capacityBytes_ = capacityBytes;  // Remember actual capacity for validation
    outQueue = std::move(queue);
    return kIOReturnSuccess;
}

inline ASFW::Async::CompletionQueue::~CompletionQueue() {
    if (source_) {
        source_->SetEnable(false);
        source_->Cancel(nullptr);
        source_.reset();
    }
}

inline bool ASFW::Async::CompletionQueue::Push(const CompletionRecord& record) noexcept {
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

    // Validate record size contract against ACTUAL capacity
    constexpr size_t kExpectedSize = sizeof(CompletionRecord);
    static_assert((kExpectedSize % 4) == 0, "CompletionRecord must be 4-byte aligned");
    static_assert(kExpectedSize > 0, "CompletionRecord must have non-zero size");

    // Validate against actual capacity (not hardcoded 64KB)
    if (kExpectedSize == 0 || kExpectedSize > capacityBytes_) {
        oversizeDropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Diagnostic logging before enqueue
    // ASFW_LOG(Async, "CompletionQueue::Push: About to enqueue - dqActive=%d clientBound=%d capacity=%zu recordSize=%zu",
    //          dqActive_.load(std::memory_order_acquire),
    //          clientBound_.load(std::memory_order_acquire),
    //          capacityBytes_,
    //          kExpectedSize);

    // Attempt enqueue with defensive lambda
    kern_return_t ret = source_->Enqueue(static_cast<unsigned>(kExpectedSize),
                                         ^(void* buffer, size_t size) {
        // Diagnostic logging inside lambda to see actual size provided
        ASFW_LOG(Async, "CompletionQueue::Push: Lambda invoked - requested=%zu actual=%zu",
                 kExpectedSize, size);

        if (size >= kExpectedSize) {
            std::memcpy(buffer, &record, kExpectedSize);
        } else {
            // CRITICAL: Don't crash, just log the mismatch
            ASFW_LOG(Async, "❌ CompletionQueue::Push: SIZE MISMATCH! requested=%zu actual=%zu - DROPPING",
                     kExpectedSize, size);
        }
    });

    ASFW_LOG(Async, "CompletionQueue::Push: Enqueue returned - ret=0x%x", ret);

    if (ret == kIOReturnSuccess) {
        ASFW_LOG(Async, "CompletionQueue::Push: SUCCESS - record enqueued");
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
