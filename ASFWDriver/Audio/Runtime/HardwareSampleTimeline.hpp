#pragma once

#include "../../Common/TimingUtils.hpp"

#include <atomic>
#include <cstdint>
#include <limits>

namespace ASFW::Audio::Runtime {

enum class HardwareTimelineSource : uint8_t {
    None = 0,
    Receive = 1,
    Transmit = 2,
};

enum class HardwareTimelineDiscontinuity : uint8_t {
    StartIO = 0,
    BusGeneration,
    SampleRate,
    ClockSource,
    HardwareRestart,
    PresentationLoss,
    SourceSwitch,
};

enum class HardwareObservationResult : uint8_t {
    Accepted = 0,
    BoundaryReady,
    DuplicateBoundary,
    StaleEpoch,
    WrongSource,
    NonMonotonic,
    Invalid,
};

struct HardwarePresentationObservation final {
    uint64_t epoch{0};
    HardwareTimelineSource source{HardwareTimelineSource::None};
    uint64_t sampleFrame{0};
    uint32_t frameCount{0};

    // Unwrapped 24.576 MHz bus time for the first frame's physical
    // presentation and a bus/host pair sampled from the same hardware event.
    uint64_t presentationBusTicks{0};
    uint64_t correlationBusTicks{0};
    uint64_t correlationHostTicks{0};
};

struct HardwareZeroTimestamp final {
    uint64_t epoch{0};
    uint64_t sampleFrame{0};
    uint64_t hostTicks{0};
    uint32_t hostNanosPerSampleQ8{0};
};

struct TxPresentationRange final {
    uint64_t epoch{0};
    uint64_t firstAudioFrame{0};
    uint32_t frameCount{0};
    uint64_t presentationBusTicks{0};
};

// Cross-service hardware timeline. BeginEpoch is called by the AudioDriverKit
// owner while stream state is quiesced. During an epoch only the selected
// observation source may update hardware correlation. TX range allocation is
// independently single-writer on the serialized preparation queue.
class HardwareSampleTimeline final {
public:
    static constexpr uint32_t kZeroTimestampPeriodFrames = 8'192;

    [[nodiscard]] static constexpr uint32_t NominalBusTicksPerFrame(
        uint32_t sampleRateHz) noexcept {
        switch (sampleRateHz) {
            case 48'000: return 512;
            case 96'000: return 256;
            case 192'000: return 128;
            default: return 0;
        }
    }

    [[nodiscard]] uint64_t BeginEpoch(
        HardwareTimelineSource source,
        HardwareTimelineDiscontinuity reason,
        uint32_t sampleRateHz,
        uint64_t baseFrame) noexcept {
        const uint32_t nominalTicks = NominalBusTicksPerFrame(sampleRateHz);
        if (source == HardwareTimelineSource::None || nominalTicks == 0) {
            return 0;
        }
        epochTransitionSequence_.fetch_add(1, std::memory_order_acq_rel);
        while (activeObservers_.load(std::memory_order_acquire) != 0) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
        const auto previousSource = source_.load(std::memory_order_relaxed);
        const uint64_t nextEpoch = epoch_.load(std::memory_order_relaxed) + 1;
        sequence_.fetch_add(1, std::memory_order_acq_rel);
        source_.store(source, std::memory_order_relaxed);
        discontinuity_.store(reason, std::memory_order_relaxed);
        sampleRateHz_.store(sampleRateHz, std::memory_order_relaxed);
        nominalBusTicksPerFrame_.store(nominalTicks, std::memory_order_relaxed);
        epochBaseFrame_.store(baseFrame, std::memory_order_relaxed);
        lastObservationFrame_.store(0, std::memory_order_relaxed);
        lastObservationFrameCount_.store(0, std::memory_order_relaxed);
        lastPresentationBusTicks_.store(0, std::memory_order_relaxed);
        lastCorrelationBusTicks_.store(0, std::memory_order_relaxed);
        lastCorrelationHostTicks_.store(0, std::memory_order_relaxed);
        observationValid_.store(false, std::memory_order_relaxed);
        lastPublishedBoundary_.store(0, std::memory_order_relaxed);
        boundaryValid_.store(false, std::memory_order_relaxed);
        txNextFrame_.store(baseFrame, std::memory_order_relaxed);
        txCursorInitialized_.store(source == HardwareTimelineSource::Transmit,
                                   std::memory_order_relaxed);
        epoch_.store(nextEpoch, std::memory_order_relaxed);
        sequence_.fetch_add(1, std::memory_order_release);
        epochTransitionSequence_.fetch_add(1, std::memory_order_release);
        epochTransitions_.fetch_add(1, std::memory_order_relaxed);
        if (previousSource != HardwareTimelineSource::None &&
            previousSource != source) {
            sourceChanges_.fetch_add(1, std::memory_order_relaxed);
        }
        return nextEpoch;
    }

    void Reset() noexcept {
        sequence_.store(0, std::memory_order_relaxed);
        epochTransitionSequence_.store(0, std::memory_order_relaxed);
        activeObservers_.store(0, std::memory_order_relaxed);
        epoch_.store(0, std::memory_order_relaxed);
        source_.store(HardwareTimelineSource::None, std::memory_order_relaxed);
        discontinuity_.store(HardwareTimelineDiscontinuity::StartIO,
                             std::memory_order_relaxed);
        sampleRateHz_.store(0, std::memory_order_relaxed);
        nominalBusTicksPerFrame_.store(0, std::memory_order_relaxed);
        epochBaseFrame_.store(0, std::memory_order_relaxed);
        observationValid_.store(false, std::memory_order_relaxed);
        boundaryValid_.store(false, std::memory_order_relaxed);
        txCursorInitialized_.store(false, std::memory_order_relaxed);
        observations_.store(0, std::memory_order_relaxed);
        rejectedObservations_.store(0, std::memory_order_relaxed);
        duplicateBoundaries_.store(0, std::memory_order_relaxed);
        ztsPublications_.store(0, std::memory_order_relaxed);
        sourceChanges_.store(0, std::memory_order_relaxed);
        epochTransitions_.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t Epoch() const noexcept {
        return epoch_.load(std::memory_order_acquire);
    }

    [[nodiscard]] HardwareTimelineSource Source() const noexcept {
        return source_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint32_t SampleRateHz() const noexcept {
        return sampleRateHz_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint64_t NextTxFrame() const noexcept {
        return txNextFrame_.load(std::memory_order_acquire);
    }

    [[nodiscard]] HardwareTimelineDiscontinuity DiscontinuityReason()
        const noexcept {
        return discontinuity_.load(std::memory_order_acquire);
    }

    // Read-only views of the observation state PreviewTxRange decides on.
    // Without them a rejected TX plan can only report its own half of the
    // comparison, which is what made a permanent NO-DATA stream unattributable.
    [[nodiscard]] bool ObservationValid() const noexcept {
        return observationValid_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint64_t LastObservationFrame() const noexcept {
        return lastObservationFrame_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint64_t LastObservationBusTicks() const noexcept {
        return lastPresentationBusTicks_.load(std::memory_order_acquire);
    }

    // The receive-derived seed expression, in one place.
    //
    // PreviewTxRange uses this exactly once per epoch, to place the TX content
    // cursor against the newest RX observation. It is exposed because that is
    // also the only defensible way to ask, later in the same epoch, "where
    // would this presentation time put the content *now*?" -- the alignment
    // probe must evaluate the same expression the seed did, not a copy of it
    // that can drift. Const and non-mutating: asking never moves the cursor.
    [[nodiscard]] bool ProjectFirstFrameFromObservation(
        uint64_t presentationBusTicks, uint64_t& outFirstFrame) const noexcept {
        if (!observationValid_.load(std::memory_order_acquire)) return false;
        const uint64_t observedFrame =
            lastObservationFrame_.load(std::memory_order_relaxed);
        const uint64_t observedBus =
            lastPresentationBusTicks_.load(std::memory_order_relaxed);
        const uint32_t nominal =
            nominalBusTicksPerFrame_.load(std::memory_order_relaxed);
        if (presentationBusTicks < observedBus || nominal == 0) return false;
        outFirstFrame =
            observedFrame + (presentationBusTicks - observedBus) / nominal;
        return true;
    }

    [[nodiscard]] uint32_t NominalBusTicksPerFrame() const noexcept {
        return nominalBusTicksPerFrame_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool TxCursorInitialized() const noexcept {
        return txCursorInitialized_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint64_t LastPublishedBoundary() const noexcept {
        return boundaryValid_.load(std::memory_order_acquire)
            ? lastPublishedBoundary_.load(std::memory_order_relaxed) : 0;
    }

    [[nodiscard]] static constexpr uint64_t NextBoundaryAfter(
        uint64_t lastBoundary) noexcept {
        return lastBoundary > UINT64_MAX - kZeroTimestampPeriodFrames
            ? UINT64_MAX
            : lastBoundary + kZeroTimestampPeriodFrames;
    }

    void CountZtsPublication() noexcept {
        ztsPublications_.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] HardwareObservationResult Observe(
        const HardwarePresentationObservation& observation,
        HardwareZeroTimestamp* outBoundary = nullptr) noexcept {
        const uint64_t transitionBefore =
            epochTransitionSequence_.load(std::memory_order_acquire);
        if ((transitionBefore & 1U) != 0U) {
            rejectedObservations_.fetch_add(1, std::memory_order_relaxed);
            return HardwareObservationResult::StaleEpoch;
        }
        activeObservers_.fetch_add(1, std::memory_order_acq_rel);
        struct ObserverScope final {
            std::atomic<uint32_t>& active;
            ~ObserverScope() {
                active.fetch_sub(1, std::memory_order_release);
            }
        } observerScope{activeObservers_};
        if (epochTransitionSequence_.load(std::memory_order_acquire) !=
            transitionBefore) {
            rejectedObservations_.fetch_add(1, std::memory_order_relaxed);
            return HardwareObservationResult::StaleEpoch;
        }
        const uint64_t activeEpoch = epoch_.load(std::memory_order_acquire);
        if (observation.epoch == 0 || observation.epoch != activeEpoch) {
            rejectedObservations_.fetch_add(1, std::memory_order_relaxed);
            return HardwareObservationResult::StaleEpoch;
        }
        if (observation.source != source_.load(std::memory_order_acquire)) {
            rejectedObservations_.fetch_add(1, std::memory_order_relaxed);
            return HardwareObservationResult::WrongSource;
        }
        const uint32_t nominalTicks =
            nominalBusTicksPerFrame_.load(std::memory_order_relaxed);
        const uint32_t rate = sampleRateHz_.load(std::memory_order_relaxed);
        if (nominalTicks == 0 || rate == 0 || observation.frameCount == 0 ||
            observation.correlationHostTicks == 0 ||
            observation.sampleFrame >
                std::numeric_limits<uint64_t>::max() - observation.frameCount) {
            rejectedObservations_.fetch_add(1, std::memory_order_relaxed);
            return HardwareObservationResult::Invalid;
        }

        if (observationValid_.load(std::memory_order_acquire)) {
            const uint64_t previousFrame =
                lastObservationFrame_.load(std::memory_order_relaxed);
            const uint64_t previousBus =
                lastPresentationBusTicks_.load(std::memory_order_relaxed);
            if (observation.sampleFrame < previousFrame ||
                observation.presentationBusTicks < previousBus) {
                rejectedObservations_.fetch_add(1, std::memory_order_relaxed);
                return HardwareObservationResult::NonMonotonic;
            }
        }

        sequence_.fetch_add(1, std::memory_order_acq_rel);
        lastObservationFrame_.store(observation.sampleFrame,
                                    std::memory_order_relaxed);
        lastObservationFrameCount_.store(observation.frameCount,
                                         std::memory_order_relaxed);
        lastPresentationBusTicks_.store(observation.presentationBusTicks,
                                        std::memory_order_relaxed);
        lastCorrelationBusTicks_.store(observation.correlationBusTicks,
                                       std::memory_order_relaxed);
        lastCorrelationHostTicks_.store(observation.correlationHostTicks,
                                        std::memory_order_relaxed);
        observationValid_.store(true, std::memory_order_relaxed);
        sequence_.fetch_add(1, std::memory_order_release);
        observations_.fetch_add(1, std::memory_order_relaxed);

        const uint64_t endFrame = observation.sampleFrame + observation.frameCount;
        const uint64_t boundary =
            ((observation.sampleFrame + kZeroTimestampPeriodFrames - 1) /
             kZeroTimestampPeriodFrames) * kZeroTimestampPeriodFrames;
        if (boundary >= endFrame) return HardwareObservationResult::Accepted;
        if (boundaryValid_.load(std::memory_order_acquire) &&
            boundary <= lastPublishedBoundary_.load(std::memory_order_relaxed)) {
            duplicateBoundaries_.fetch_add(1, std::memory_order_relaxed);
            return HardwareObservationResult::DuplicateBoundary;
        }

        const uint64_t boundaryBusTicks = observation.presentationBusTicks +
            (boundary - observation.sampleFrame) * nominalTicks;
        uint64_t boundaryHostTicks = 0;
        if (!ProjectBusToHost(observation.correlationBusTicks,
                              observation.correlationHostTicks,
                              boundaryBusTicks, boundaryHostTicks)) {
            rejectedObservations_.fetch_add(1, std::memory_order_relaxed);
            return HardwareObservationResult::Invalid;
        }

        lastPublishedBoundary_.store(boundary, std::memory_order_relaxed);
        boundaryValid_.store(true, std::memory_order_release);
        if (outBoundary) {
            *outBoundary = {
                .epoch = activeEpoch,
                .sampleFrame = boundary,
                .hostTicks = boundaryHostTicks,
                .hostNanosPerSampleQ8 = static_cast<uint32_t>(
                    (1'000'000'000ULL << 8) / rate),
            };
        }
        return HardwareObservationResult::BoundaryReady;
    }

    // Preview does not mutate. CommitTxRange is the only operation that moves
    // the TX content coordinate, including the explicit missed-deadline path.
    [[nodiscard]] bool PreviewTxRange(uint64_t epoch,
                                      uint64_t presentationBusTicks,
                                      uint32_t frameCount,
                                      TxPresentationRange& out) noexcept {
        if (epoch == 0 || epoch != Epoch() || frameCount == 0) return false;
        uint64_t first = txNextFrame_.load(std::memory_order_acquire);
        if (!txCursorInitialized_.load(std::memory_order_acquire) &&
            !ProjectFirstFrameFromObservation(presentationBusTicks, first)) {
            return false;
        }
        out = {
            .epoch = epoch,
            .firstAudioFrame = first,
            .frameCount = frameCount,
            .presentationBusTicks = presentationBusTicks,
        };
        return true;
    }

    [[nodiscard]] bool CommitTxRange(const TxPresentationRange& range) noexcept {
        if (range.epoch == 0 || range.epoch != Epoch() || range.frameCount == 0 ||
            range.firstAudioFrame >
                std::numeric_limits<uint64_t>::max() - range.frameCount) {
            return false;
        }
        bool initialized = txCursorInitialized_.load(std::memory_order_acquire);
        if (!initialized) {
            txNextFrame_.store(range.firstAudioFrame, std::memory_order_relaxed);
            txCursorInitialized_.store(true, std::memory_order_release);
        }
        uint64_t expected = range.firstAudioFrame;
        return txNextFrame_.compare_exchange_strong(
            expected, range.firstAudioFrame + range.frameCount,
            std::memory_order_release, std::memory_order_relaxed);
    }

    [[nodiscard]] static bool ProjectBusToHost(
        uint64_t correlationBusTicks,
        uint64_t correlationHostTicks,
        uint64_t targetBusTicks,
        uint64_t& outHostTicks) noexcept {
        if (correlationHostTicks == 0) return false;
        const bool forward = targetBusTicks >= correlationBusTicks;
        const uint64_t delta = forward
            ? targetBusTicks - correlationBusTicks
            : correlationBusTicks - targetBusTicks;
        const unsigned __int128 nanosWide =
            static_cast<unsigned __int128>(delta) * 1'000'000'000ULL /
            ASFW::Timing::kTicksPerSecond;
        if (nanosWide > std::numeric_limits<uint64_t>::max()) return false;
        const uint64_t hostDelta = ASFW::Timing::nanosToHostTicks(
            static_cast<uint64_t>(nanosWide));
        if (forward) {
            if (correlationHostTicks >
                std::numeric_limits<uint64_t>::max() - hostDelta) return false;
            outHostTicks = correlationHostTicks + hostDelta;
        } else {
            if (correlationHostTicks < hostDelta) return false;
            outHostTicks = correlationHostTicks - hostDelta;
        }
        return true;
    }

    std::atomic<uint64_t> observations_{0};
    std::atomic<uint64_t> rejectedObservations_{0};
    std::atomic<uint64_t> duplicateBoundaries_{0};
    std::atomic<uint64_t> ztsPublications_{0};
    std::atomic<uint64_t> sourceChanges_{0};
    std::atomic<uint64_t> epochTransitions_{0};

private:
    std::atomic<uint64_t> sequence_{0};
    std::atomic<uint64_t> epochTransitionSequence_{0};
    std::atomic<uint32_t> activeObservers_{0};
    std::atomic<uint64_t> epoch_{0};
    std::atomic<HardwareTimelineSource> source_{HardwareTimelineSource::None};
    std::atomic<HardwareTimelineDiscontinuity> discontinuity_{
        HardwareTimelineDiscontinuity::StartIO};
    std::atomic<uint32_t> sampleRateHz_{0};
    std::atomic<uint32_t> nominalBusTicksPerFrame_{0};
    std::atomic<uint64_t> epochBaseFrame_{0};

    std::atomic<uint64_t> lastObservationFrame_{0};
    std::atomic<uint32_t> lastObservationFrameCount_{0};
    std::atomic<uint64_t> lastPresentationBusTicks_{0};
    std::atomic<uint64_t> lastCorrelationBusTicks_{0};
    std::atomic<uint64_t> lastCorrelationHostTicks_{0};
    std::atomic<bool> observationValid_{false};

    std::atomic<uint64_t> lastPublishedBoundary_{0};
    std::atomic<bool> boundaryValid_{false};

    std::atomic<uint64_t> txNextFrame_{0};
    std::atomic<bool> txCursorInitialized_{false};
};

static_assert(std::atomic<uint64_t>::is_always_lock_free);

} // namespace ASFW::Audio::Runtime
