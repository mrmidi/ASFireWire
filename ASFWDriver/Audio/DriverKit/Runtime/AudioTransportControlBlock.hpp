#pragma once

#include "AudioClientCursor.hpp"
#include "AudioRtCounters.hpp"
#include "DeviceTimeline.hpp"
#include "Audio/Runtime/ZtsAuthority.hpp"

#include <atomic>
#include <cstdint>

enum class FatalStreamReason : uint32_t {
    None = 0,
    RxAuthorityLost,
    InvalidGeometry,
    MirrorPumpFailed,
};

namespace ASFW::Audio::Runtime {

struct AudioTransportControlBlock final {
    std::atomic<uint64_t> generation{0};

    AudioClientCursor client{};
    DeviceTimeline device{};
    AudioRtCounters counters{};
    ZtsAuthorityState ztsState{};

    std::atomic<FatalStreamReason> fatalReason{FatalStreamReason::None};
    std::atomic<uint64_t> fatalGeneration{0};

    std::atomic<uint64_t> inputProducedEndFrame{0};
    std::atomic<uint64_t> outputConsumedEndFrame{0};

    std::atomic<uint64_t> inputOverruns{0};
    std::atomic<uint64_t> outputUnderruns{0};
    std::atomic<uint64_t> discontinuities{0};

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

        inputProducedEndFrame.store(0, std::memory_order_release);
        outputConsumedEndFrame.store(0, std::memory_order_release);

        inputOverruns.store(0, std::memory_order_release);
        outputUnderruns.store(0, std::memory_order_release);
        discontinuities.store(0, std::memory_order_release);

        generation.fetch_add(1, std::memory_order_acq_rel);
    }
};

} // namespace ASFW::Audio::Runtime
