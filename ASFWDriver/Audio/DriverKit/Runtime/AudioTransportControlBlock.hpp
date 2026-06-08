#pragma once

#include "AudioClientCursor.hpp"
#include "AudioRtCounters.hpp"
#include "DeviceTimeline.hpp"
#include "../../Runtime/ZtsAuthority.hpp"


#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

enum class FatalStreamReason : uint32_t {
    None = 0,
    RxAuthorityLost,
    InvalidGeometry,
    MirrorPumpFailed,
    TxReadAhead,
    TxSourceOverwritten,
    TxPreparationMissedDeadline,
    TxSlotInvariant,
    TxPayloadMismatch,
};

struct TxFatalSnapshot final {
    std::atomic<uint64_t> audioFrame{0};
    std::atomic<int64_t> outputPhaseTicks{-1};
    std::atomic<uint64_t> oldestValidFrame{0};
    std::atomic<uint64_t> writtenEndFrame{0};
    std::atomic<uint32_t> packetIndex{0};
    std::atomic<uint32_t> distanceToHardware{0};
    std::atomic<uint32_t> slotState{0};
    std::atomic<uint32_t> dbc{0};
    std::atomic<uint32_t> syt{0};
    std::atomic<uint64_t> preparedPayloadHash{0};
    std::atomic<uint64_t> completedPayloadHash{0};

    void Reset() noexcept {
        audioFrame.store(0, std::memory_order_relaxed);
        outputPhaseTicks.store(-1, std::memory_order_relaxed);
        oldestValidFrame.store(0, std::memory_order_relaxed);
        writtenEndFrame.store(0, std::memory_order_relaxed);
        packetIndex.store(0, std::memory_order_relaxed);
        distanceToHardware.store(0, std::memory_order_relaxed);
        slotState.store(0, std::memory_order_relaxed);
        dbc.store(0, std::memory_order_relaxed);
        syt.store(0, std::memory_order_relaxed);
        preparedPayloadHash.store(0, std::memory_order_relaxed);
        completedPayloadHash.store(0, std::memory_order_relaxed);
    }
};

struct TxPreparationRequestState final {
    std::atomic<uint64_t> requestedGeneration{0};
    std::atomic<uint64_t> handledGeneration{0};
    std::atomic<uint64_t> requestHostTicks{0};
    std::atomic<uint64_t> handledHostTicks{0};

    [[nodiscard]] uint64_t PublishRequest(uint64_t hostTicks) noexcept {
        requestHostTicks.store(hostTicks, std::memory_order_relaxed);
        return requestedGeneration.fetch_add(1, std::memory_order_release) + 1;
    }

    [[nodiscard]] uint64_t RequestedGeneration() const noexcept {
        return requestedGeneration.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool NeedsHandling() const noexcept {
        return handledGeneration.load(std::memory_order_acquire) <
               requestedGeneration.load(std::memory_order_acquire);
    }

    void MarkHandled(uint64_t generation, uint64_t hostTicks) noexcept {
        handledHostTicks.store(hostTicks, std::memory_order_relaxed);
        handledGeneration.store(generation, std::memory_order_release);
    }

    void Reset() noexcept {
        requestedGeneration.store(0, std::memory_order_relaxed);
        handledGeneration.store(0, std::memory_order_relaxed);
        requestHostTicks.store(0, std::memory_order_relaxed);
        handledHostTicks.store(0, std::memory_order_relaxed);
    }
};

struct AudioTransportControlBlock final {
    std::atomic<uint64_t> generation{0};

    AudioClientCursor client{};
    DeviceTimeline device{};
    AudioRtCounters counters{};
    ZtsAuthorityState ztsState{};

    std::atomic<FatalStreamReason> fatalReason{FatalStreamReason::None};
    std::atomic<uint64_t> fatalGeneration{0};
    TxFatalSnapshot txFatalSnapshot{};
    TxPreparationRequestState txPreparationRequests{};

    std::atomic<uint64_t> inputProducedEndFrame{0};
    std::atomic<uint64_t> outputConsumedEndFrame{0};

    std::atomic<uint64_t> inputOverruns{0};
    std::atomic<uint64_t> outputUnderruns{0};
    std::atomic<uint64_t> discontinuities{0};

    std::atomic<uint64_t> playbackRingWriteFrame{0};
    std::atomic<uint64_t> playbackRingReadFrame{0};
    std::atomic<uint64_t> playbackRingOldestValidFrame{0};
    std::atomic<uint64_t> playbackRingDiscontinuityGeneration{0};
    std::atomic<uint64_t> playbackRingUnderruns{0};
    std::atomic<uint64_t> playbackRingOverruns{0};
    std::atomic<uint64_t> txScheduledSampleFrame{0};
    std::atomic<uint64_t> txCompletedSampleFrame{0};
    std::atomic<uint32_t> txMinimumPreparationDistance{UINT32_MAX};
    std::atomic<uint64_t> txLastPreparationLatencyTicks{0};
    std::atomic<uint64_t> txMaxPreparationLatencyTicks{0};

    std::atomic<uint64_t> captureRingWriteFrame{0};
    std::atomic<uint64_t> captureRingReadFrame{0};
    std::atomic<uint64_t> captureRingOverruns{0};
    std::atomic<uint64_t> captureRingStarvations{0};

    bool UpdateAuthoritativeZtsFromRx(uint64_t sampleFrame,
                                      uint64_t hostTicks,
                                      uint32_t hostNanosPerSampleQ8) noexcept {
        if (ztsState.selectedSource.load(std::memory_order_relaxed) != ZtsAuthoritySource::RxClock) {
            ztsState.rejectedRxUpdates.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        if (hostTicks == 0 || hostNanosPerSampleQ8 == 0) {
            ztsState.staleSourceUpdates.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const uint64_t prevSample = ztsState.authoritativeSampleFrame.load(std::memory_order_relaxed);
        const uint64_t prevHost = ztsState.authoritativeHostTicks.load(std::memory_order_relaxed);
        if (sampleFrame >= prevSample && hostTicks < prevHost) {
            ztsState.staleSourceUpdates.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        ztsState.authoritativeSampleFrame.store(sampleFrame, std::memory_order_relaxed);
        ztsState.authoritativeHostTicks.store(hostTicks, std::memory_order_relaxed);
        ztsState.hostNanosPerSampleQ8.store(hostNanosPerSampleQ8, std::memory_order_relaxed);
        ztsState.rxSourceUpdates.fetch_add(1, std::memory_order_relaxed);
        ztsState.sourceGeneration.fetch_add(1, std::memory_order_release);
        return true;
    }

    bool UpdateAuthoritativeZtsFromTx(uint64_t sampleFrame,
                                      uint64_t hostTicks,
                                      uint32_t hostNanosPerSampleQ8) noexcept {
        if (ztsState.selectedSource.load(std::memory_order_relaxed) != ZtsAuthoritySource::TxClock) {
            ztsState.rejectedTxUpdates.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        if (hostTicks == 0 || hostNanosPerSampleQ8 == 0) {
            ztsState.staleSourceUpdates.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const uint64_t prevSample = ztsState.authoritativeSampleFrame.load(std::memory_order_relaxed);
        const uint64_t prevHost = ztsState.authoritativeHostTicks.load(std::memory_order_relaxed);
        if (sampleFrame >= prevSample && hostTicks < prevHost) {
            ztsState.staleSourceUpdates.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        ztsState.authoritativeSampleFrame.store(sampleFrame, std::memory_order_relaxed);
        ztsState.authoritativeHostTicks.store(hostTicks, std::memory_order_relaxed);
        ztsState.hostNanosPerSampleQ8.store(hostNanosPerSampleQ8, std::memory_order_relaxed);
        ztsState.txSourceUpdates.fetch_add(1, std::memory_order_relaxed);
        ztsState.sourceGeneration.fetch_add(1, std::memory_order_release);
        return true;
    }

    void ResetForStart() noexcept {
        client.Reset();
        device.Reset();
        counters.Reset();
        ztsState.Reset();

        fatalReason.store(FatalStreamReason::None, std::memory_order_release);
        fatalGeneration.store(0, std::memory_order_release);
        txFatalSnapshot.Reset();
        txPreparationRequests.Reset();

        inputProducedEndFrame.store(0, std::memory_order_release);
        outputConsumedEndFrame.store(0, std::memory_order_release);

        inputOverruns.store(0, std::memory_order_release);
        outputUnderruns.store(0, std::memory_order_release);
        discontinuities.store(0, std::memory_order_release);

        playbackRingWriteFrame.store(0, std::memory_order_release);
        playbackRingReadFrame.store(0, std::memory_order_release);
        playbackRingOldestValidFrame.store(0, std::memory_order_release);
        playbackRingDiscontinuityGeneration.store(0, std::memory_order_release);
        playbackRingUnderruns.store(0, std::memory_order_release);
        playbackRingOverruns.store(0, std::memory_order_release);
        txScheduledSampleFrame.store(0, std::memory_order_release);
        txCompletedSampleFrame.store(0, std::memory_order_release);
        txMinimumPreparationDistance.store(UINT32_MAX, std::memory_order_release);
        txLastPreparationLatencyTicks.store(0, std::memory_order_release);
        txMaxPreparationLatencyTicks.store(0, std::memory_order_release);

        captureRingWriteFrame.store(0, std::memory_order_release);
        captureRingReadFrame.store(0, std::memory_order_release);
        captureRingOverruns.store(0, std::memory_order_release);
        captureRingStarvations.store(0, std::memory_order_release);

        generation.fetch_add(1, std::memory_order_acq_rel);
    }
};

} // namespace ASFW::Audio::Runtime
