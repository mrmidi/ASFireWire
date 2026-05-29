#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include "../Shared/ASFWDiagnosticsABI.h"

namespace ASFW::Debug {

class AsyncTraceCapture {
public:
    AsyncTraceCapture() noexcept;
    ~AsyncTraceCapture() = default;

    // Disable copy/move
    AsyncTraceCapture(const AsyncTraceCapture&) = delete;
    AsyncTraceCapture& operator=(const AsyncTraceCapture&) = delete;

    // Retrieve stats and event list
    void CaptureEvent(const ASFWDiagAsyncEvent& event) noexcept;
    void RecordDrop() noexcept;

    // Populate the ABI structure for user-space querying
    void PopulateSnapshot(ASFWDiagAsyncTrace* outTrace, uint32_t currentGeneration) const noexcept;

    // Reset statistics
    void Clear() noexcept;

private:
    std::array<ASFWDiagAsyncEvent, ASFW_DIAG_MAX_ASYNC_EVENTS> ring_;
    std::atomic<uint32_t> writeIndex_{0};
    std::atomic<uint32_t> count_{0};
    std::atomic<uint32_t> droppedCount_{0};
};

} // namespace ASFW::Debug
