#pragma once

#include "../Ports/ITxPcmSource.hpp"

#include <atomic>
#include <cstdint>
#include <memory>

namespace ASFW::Audio::Runtime {

struct TxPcmStagingTelemetry final {
    std::atomic<uint64_t> oldestValidFrame{0};
    std::atomic<uint64_t> writtenEndFrame{0};
    std::atomic<uint64_t> writes{0};
    std::atomic<uint64_t> framesStaged{0};
    std::atomic<uint64_t> discontinuities{0};
    std::atomic<uint64_t> overlapFramesIgnored{0};
    std::atomic<uint64_t> overwrittenFrames{0};
    std::atomic<uint64_t> readsReady{0};
    std::atomic<uint64_t> readsNotYetWritten{0};
    std::atomic<uint64_t> readsStaleOverwritten{0};
    std::atomic<uint64_t> readsSnapshotBusy{0};
    std::atomic<uint64_t> readsInvalid{0};

    void Reset() noexcept {
        oldestValidFrame.store(0, std::memory_order_relaxed);
        writtenEndFrame.store(0, std::memory_order_relaxed);
        writes.store(0, std::memory_order_relaxed);
        framesStaged.store(0, std::memory_order_relaxed);
        discontinuities.store(0, std::memory_order_relaxed);
        overlapFramesIgnored.store(0, std::memory_order_relaxed);
        overwrittenFrames.store(0, std::memory_order_relaxed);
        readsReady.store(0, std::memory_order_relaxed);
        readsNotYetWritten.store(0, std::memory_order_relaxed);
        readsStaleOverwritten.store(0, std::memory_order_relaxed);
        readsSnapshotBusy.store(0, std::memory_order_relaxed);
        readsInvalid.store(0, std::memory_order_relaxed);
    }
};

struct TxPcmHostRingView final {
    const float* interleavedFloat32{nullptr};
    uint64_t firstFrame{0};
    uint32_t frameCount{0};
    uint32_t frameCapacity{0};
    uint32_t channels{0};
};

enum class TxPcmStageResult : uint8_t {
    kStaged = 0,
    kDuplicate,
    kInvalidView,
    kNotConfigured,
};

// One-producer/one-consumer PCM retention ring. CoreAudio WriteEnd is the sole
// producer; the TX preparation queue is the sole consumer. Samples are stored
// as atomic IEEE-754 bit patterns and each physical frame has its own absolute
// frame tag. That makes an overwrite formally race-free while allowing a
// concurrent append to unrelated frames (including a second DICE TX stream's
// snapshot) to proceed without a false whole-ring retry.
class TxPcmStagingRing final : public ASFW::Audio::Ports::ITxPcmSource {
public:
    TxPcmStagingRing() noexcept = default;

    [[nodiscard]] bool Configure(uint32_t channels,
                                 uint32_t frameCapacity) noexcept;
    void BindTelemetry(TxPcmStagingTelemetry* telemetry) noexcept;
    void ResetForStart() noexcept;

    [[nodiscard]] TxPcmStageResult Stage(
        const TxPcmHostRingView& hostView) noexcept;

    [[nodiscard]] ASFW::Audio::Ports::TxPcmReadResult
    ReadFloat32Interleaved(
        const ASFW::Audio::Ports::TxPcmReadRequest& request,
        float* destination,
        uint32_t destinationSampleCapacity) const noexcept override;

    [[nodiscard]] uint64_t OldestValidFrame() const noexcept override;
    [[nodiscard]] uint64_t WrittenEndFrame() const noexcept override;

    [[nodiscard]] uint32_t ChannelCount() const noexcept { return channels_; }
    [[nodiscard]] uint32_t FrameCapacity() const noexcept {
        return frameCapacity_;
    }

private:
    static constexpr uint32_t kReadAttempts = 4;

    void CountRead(ASFW::Audio::Ports::TxPcmReadResult result) const noexcept;

    std::unique_ptr<std::atomic<uint32_t>[]> sampleBits_{};
    std::unique_ptr<std::atomic<uint64_t>[]> frameTags_{};
    uint32_t channels_{0};
    uint32_t frameCapacity_{0};

    std::atomic<uint64_t> oldestValidFrame_{0};
    std::atomic<uint64_t> writtenEndFrame_{0};
    std::atomic<bool> hasRange_{false};

    TxPcmStagingTelemetry* telemetry_{nullptr};
};

} // namespace ASFW::Audio::Runtime
