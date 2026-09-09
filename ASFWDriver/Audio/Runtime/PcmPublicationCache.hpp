#pragma once

#include "../Ports/ITxPcmSource.hpp"

#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace ASFW::Audio::Runtime {

struct PcmPublicationTelemetry final {
    static constexpr uint32_t kHistogramBuckets = 5;
    std::atomic<uint64_t> epoch{0};
    std::atomic<uint64_t> oldestValidFrame{0};
    std::atomic<uint64_t> publishedEndFrame{0};
    std::atomic<uint64_t> publications{0};
    std::atomic<uint64_t> framesPublished{0};
    std::atomic<uint64_t> discontinuities{0};
    std::atomic<uint64_t> duplicateFrames{0};
    std::atomic<uint64_t> expiredFrames{0};
    std::atomic<uint64_t> copiesReady{0};
    std::atomic<uint64_t> copiesNotYetPublished{0};
    std::atomic<uint64_t> copiesExpired{0};
    std::atomic<uint64_t> copiesWrongEpoch{0};
    std::atomic<uint64_t> copiesConcurrentRewrite{0};
    std::atomic<uint64_t> copiesInvalid{0};
    std::atomic<uint64_t> maximumPublicationFrames{0};
    std::atomic<uint64_t> maximumPublicationDurationTicks{0};
    std::array<std::atomic<uint64_t>, kHistogramBuckets>
        publicationSpanHistogram{};
    std::array<std::atomic<uint64_t>, kHistogramBuckets>
        publicationDurationHistogram{};

    void Reset(uint64_t nextEpoch = 0) noexcept {
        epoch.store(nextEpoch, std::memory_order_relaxed);
        oldestValidFrame.store(0, std::memory_order_relaxed);
        publishedEndFrame.store(0, std::memory_order_relaxed);
        publications.store(0, std::memory_order_relaxed);
        framesPublished.store(0, std::memory_order_relaxed);
        discontinuities.store(0, std::memory_order_relaxed);
        duplicateFrames.store(0, std::memory_order_relaxed);
        expiredFrames.store(0, std::memory_order_relaxed);
        copiesReady.store(0, std::memory_order_relaxed);
        copiesNotYetPublished.store(0, std::memory_order_relaxed);
        copiesExpired.store(0, std::memory_order_relaxed);
        copiesWrongEpoch.store(0, std::memory_order_relaxed);
        copiesConcurrentRewrite.store(0, std::memory_order_relaxed);
        copiesInvalid.store(0, std::memory_order_relaxed);
        maximumPublicationFrames.store(0, std::memory_order_relaxed);
        maximumPublicationDurationTicks.store(0, std::memory_order_relaxed);
        for (auto& bucket : publicationSpanHistogram) {
            bucket.store(0, std::memory_order_relaxed);
        }
        for (auto& bucket : publicationDurationHistogram) {
            bucket.store(0, std::memory_order_relaxed);
        }
    }
};

struct PcmPublicationView final {
    const float* interleavedFloat32{nullptr};
    uint64_t epoch{0};
    uint64_t firstFrame{0};
    uint32_t frameCount{0};
    uint32_t frameCapacity{0};
    uint32_t channels{0};
};

enum class PcmPublishResult : uint8_t {
    Published = 0,
    Duplicate,
    WrongEpoch,
    InvalidView,
    NotConfigured,
};

// Audio-owned publication cache between CoreAudio WriteEnd and the independent
// TX preparation queue. It owns bytes, not time: publishing never advances a
// packet, presentation frame, cadence, or hardware cursor.
//
// PCM words are atomic IEEE-754 bit patterns. Each physical frame is protected
// by an odd/even sequence and carries an explicit {epoch, absoluteFrame}
// identity. The producer marks the sequence odd, replaces identity and words,
// then release-publishes the next even sequence. The consumer acquire-checks
// the identity, copies, and accepts only an unchanged even sequence.
class PcmPublicationCache final : public ASFW::Audio::Ports::ITxPcmSource {
public:
    struct StagedStorage final {
        std::unique_ptr<std::atomic<uint32_t>[]> sampleBits{};
        std::unique_ptr<std::atomic<uint64_t>[]> frameSequences{};
        std::unique_ptr<std::atomic<uint64_t>[]> frameEpochs{};
        std::unique_ptr<std::atomic<uint64_t>[]> frameAbsoluteFrames{};
        uint32_t channels{0};
        uint32_t cacheCapacityFrames{0};
    };

    [[nodiscard]] static std::optional<StagedStorage> AllocateStorage(
        uint32_t channels, uint32_t cacheCapacityFrames) noexcept;

    void CommitStorage(StagedStorage&& staged) noexcept;

    [[nodiscard]] bool Configure(uint32_t channels,
                                 uint32_t cacheCapacityFrames) noexcept;
    void BindTelemetry(PcmPublicationTelemetry* telemetry) noexcept;
    void BeginEpoch(uint64_t epoch) noexcept;

    [[nodiscard]] PcmPublishResult Publish(
        const PcmPublicationView& hostView,
        uint64_t* outCommittedStart = nullptr,
        uint64_t* outCommittedEnd = nullptr) noexcept;

    [[nodiscard]] ASFW::Audio::Ports::PcmCopyResult CopyExact(
        const ASFW::Audio::Ports::TxPcmReadRequest& request,
        float* destination,
        uint32_t destinationSampleCapacity) const noexcept override;

    [[nodiscard]] uint64_t OldestValidFrame() const noexcept override;
    [[nodiscard]] uint64_t PublishedEndFrame() const noexcept override;
    [[nodiscard]] uint64_t Epoch() const noexcept override;

    [[nodiscard]] uint32_t ChannelCount() const noexcept { return channels_; }
    [[nodiscard]] uint32_t CacheCapacityFrames() const noexcept {
        return cacheCapacityFrames_;
    }
    [[nodiscard]] uint32_t FrameCapacity() const noexcept {
        return cacheCapacityFrames_;
    }

#if defined(ASFW_HOST_TEST)
    void ForceFrameWritingForTest(uint64_t absoluteFrame) noexcept {
        if (!frameSequences_ || cacheCapacityFrames_ == 0) return;
        const uint64_t physical = absoluteFrame % cacheCapacityFrames_;
        const uint64_t old = frameSequences_[physical].load(
            std::memory_order_relaxed);
        frameSequences_[physical].store(old | 1U, std::memory_order_release);
    }
#endif

private:
    static constexpr uint32_t kCopyAttempts = 4;

    void CountCopy(ASFW::Audio::Ports::PcmCopyResult result) const noexcept;

    std::unique_ptr<std::atomic<uint32_t>[]> sampleBits_{};
    std::unique_ptr<std::atomic<uint64_t>[]> frameSequences_{};
    std::unique_ptr<std::atomic<uint64_t>[]> frameEpochs_{};
    std::unique_ptr<std::atomic<uint64_t>[]> frameAbsoluteFrames_{};
    uint32_t channels_{0};
    uint32_t cacheCapacityFrames_{0};

    std::atomic<uint64_t> epoch_{0};
    // Odd while BeginEpoch invalidates physical slots. This makes a recovery
    // epoch safe against an in-flight WriteEnd without adding a lock to the RT
    // callback.
    std::atomic<uint64_t> epochTransitionSequence_{0};
    std::atomic<uint32_t> activePublishers_{0};
    std::atomic<uint64_t> oldestValidFrame_{0};
    std::atomic<uint64_t> publishedEndFrame_{0};
    std::atomic<bool> hasRange_{false};

    PcmPublicationTelemetry* telemetry_{nullptr};
};

} // namespace ASFW::Audio::Runtime
