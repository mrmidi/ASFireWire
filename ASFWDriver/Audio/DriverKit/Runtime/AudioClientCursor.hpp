#pragma once

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

struct AudioClientCursor final {
    std::atomic<uint64_t> inputBeginReadSampleFrame{0};
    std::atomic<uint64_t> inputBeginReadHostTicks{0};
    std::atomic<uint32_t> inputBeginReadFrames{0};
    std::atomic<uint64_t> inputClientReadEndFrame{0};

    std::atomic<uint64_t> outputWriteEndSampleFrame{0};
    std::atomic<uint64_t> outputWriteEndHostTicks{0};
    std::atomic<uint32_t> outputWriteEndFrames{0};
    std::atomic<uint64_t> outputClientWriteEndFrame{0};

    void PublishBeginRead(uint64_t sampleFrame,
                          uint64_t hostTicks,
                          uint32_t frameCount) noexcept {
        inputBeginReadSampleFrame.store(sampleFrame, std::memory_order_relaxed);
        inputBeginReadHostTicks.store(hostTicks, std::memory_order_relaxed);
        inputBeginReadFrames.store(frameCount, std::memory_order_relaxed);
        inputClientReadEndFrame.store(sampleFrame + frameCount, std::memory_order_release);
    }

    void PublishWriteEnd(uint64_t sampleFrame,
                         uint64_t hostTicks,
                         uint32_t frameCount) noexcept {
        outputWriteEndSampleFrame.store(sampleFrame, std::memory_order_relaxed);
        outputWriteEndHostTicks.store(hostTicks, std::memory_order_relaxed);
        outputWriteEndFrames.store(frameCount, std::memory_order_relaxed);
        outputClientWriteEndFrame.store(sampleFrame + frameCount, std::memory_order_release);
    }

    [[nodiscard]] uint64_t OutputWrittenEndFrame() const noexcept {
        return outputClientWriteEndFrame.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint64_t InputReadEndFrame() const noexcept {
        return inputClientReadEndFrame.load(std::memory_order_acquire);
    }
};

} // namespace ASFW::Audio::Runtime
