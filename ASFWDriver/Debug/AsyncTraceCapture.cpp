#include "AsyncTraceCapture.hpp"
#include <cstring>

namespace ASFW::Debug {

AsyncTraceCapture::AsyncTraceCapture() noexcept {
    Clear();
}

void AsyncTraceCapture::CaptureEvent(const ASFWDiagAsyncEvent& event) noexcept {
    // Write atomically into the ring
    const uint32_t idx = writeIndex_.fetch_add(1, std::memory_order_relaxed) % ASFW_DIAG_MAX_ASYNC_EVENTS;
    ring_[idx] = event;

    // Increment count up to max capacity
    uint32_t currentCount = count_.load(std::memory_order_relaxed);
    while (currentCount < ASFW_DIAG_MAX_ASYNC_EVENTS &&
           !count_.compare_exchange_weak(currentCount, currentCount + 1,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
        // Retry
    }
}

void AsyncTraceCapture::RecordDrop() noexcept {
    droppedCount_.fetch_add(1, std::memory_order_relaxed);
}

void AsyncTraceCapture::PopulateSnapshot(ASFWDiagAsyncTrace* outTrace, uint32_t currentGeneration) const noexcept {
    if (!outTrace) {
        return;
    }

    outTrace->header.abiVersion = ASFW_DIAG_ABI_VERSION;
    outTrace->header.structSize = sizeof(ASFWDiagAsyncTrace);
    outTrace->header.status = ASFWDiagStatusOK;
    outTrace->header.generation = currentGeneration;
    outTrace->header.timestampNs = 0; // Filled by DiagnosticsService
    outTrace->header.snapshotSeq = 0; // Filled by DiagnosticsService

    const uint32_t currentWriteIdx = writeIndex_.load(std::memory_order_acquire);
    const uint32_t currentCount = count_.load(std::memory_order_acquire);
    outTrace->droppedCount = droppedCount_.load(std::memory_order_acquire);
    outTrace->eventCount = currentCount;

    // Chronological retrieval (oldest to newest)
    uint32_t startIdx = 0;
    if (currentCount >= ASFW_DIAG_MAX_ASYNC_EVENTS) {
        startIdx = currentWriteIdx % ASFW_DIAG_MAX_ASYNC_EVENTS;
    }

    for (uint32_t i = 0; i < currentCount; ++i) {
        const uint32_t ringIdx = (startIdx + i) % ASFW_DIAG_MAX_ASYNC_EVENTS;
        outTrace->events[i] = ring_[ringIdx];
    }
}

void AsyncTraceCapture::Clear() noexcept {
    std::memset(ring_.data(), 0, ring_.size() * sizeof(ASFWDiagAsyncEvent));
    writeIndex_.store(0, std::memory_order_release);
    count_.store(0, std::memory_order_release);
    droppedCount_.store(0, std::memory_order_release);
}

} // namespace ASFW::Debug
