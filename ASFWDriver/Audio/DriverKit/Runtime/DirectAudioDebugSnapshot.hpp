#pragma once

#include "AudioGraphBinding.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

constexpr uint64_t kDirectAudioDebugLogIntervalNs = 5'000'000'000ULL;

struct DirectAudioDebugSnapshot final {
    bool bound{false};

    uint64_t inputBufferAddress{0};
    uint64_t outputBufferAddress{0};
    uint32_t inputFrameCapacity{0};
    uint32_t outputFrameCapacity{0};
    uint32_t inputChannels{0};
    uint32_t outputChannels{0};

    uint64_t ioBeginReadCount{0};
    uint64_t ioWriteEndCount{0};

    uint64_t inputBeginReadSampleFrame{0};
    uint64_t inputClientReadEndFrame{0};

    uint64_t outputWriteEndSampleFrame{0};
    uint64_t outputClientWriteEndFrame{0};

    uint32_t inputBeginReadFrameCount{0};
    uint32_t outputWriteEndFrameCount{0};
    uint32_t ioBufferFrameSize{0};
    uint32_t expectedIoBufferFrameSize{0};

    int64_t lastSampleDelta{0};
    uint64_t sampleTimeRegressionCount{0};
    uint64_t ioBufferFrameSizeChangeCount{0};

    uint64_t directTxPackets{0};
    uint64_t directTxUnderruns{0};
    uint64_t directTxSilenceSubstitutions{0};
    bool outputReaderAvailableAtWriteEnd{false};

    // Phase A counters
    uint64_t txValidPhasePcmPackets{0};
    uint64_t txValidPhaseSilencePackets{0};
    uint64_t txNoPhaseSilencePackets{0};
    uint64_t txUnderrunSilencePackets{0};
    uint64_t txStaleSyncPackets{0};
    uint64_t txInvalidGeometryPackets{0};
};

struct DirectAudioDebugLogState final {
    uint64_t lastLogTimeNs{0};
    bool hasLogged{false};
    bool lastBound{false};

    void Reset() noexcept {
        lastLogTimeNs = 0;
        hasLogged = false;
        lastBound = false;
    }
};

[[nodiscard]] inline DirectAudioDebugSnapshot CaptureDirectAudioDebugSnapshot(
    const AudioGraphBinding& binding,
    bool bound,
    uint32_t ioBufferFrameSize,
    uint32_t expectedIoBufferFrameSize,
    int64_t lastSampleDelta,
    uint64_t sampleTimeRegressionCount,
    uint64_t ioBufferFrameSizeChangeCount,
    bool outputReaderAvailableAtWriteEnd) noexcept {
    DirectAudioDebugSnapshot snapshot{};

    snapshot.bound = bound && binding.IsValid();
    snapshot.inputBufferAddress =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(binding.memory.inputBase));
    snapshot.outputBufferAddress =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(binding.memory.outputBase));
    snapshot.inputFrameCapacity = binding.memory.inputFrameCapacity;
    snapshot.outputFrameCapacity = binding.memory.outputFrameCapacity;
    snapshot.inputChannels = binding.memory.inputChannels;
    snapshot.outputChannels = binding.memory.outputChannels;
    snapshot.ioBufferFrameSize = ioBufferFrameSize;
    snapshot.expectedIoBufferFrameSize = expectedIoBufferFrameSize;
    snapshot.lastSampleDelta = lastSampleDelta;
    snapshot.sampleTimeRegressionCount = sampleTimeRegressionCount;
    snapshot.ioBufferFrameSizeChangeCount = ioBufferFrameSizeChangeCount;
    snapshot.outputReaderAvailableAtWriteEnd = outputReaderAvailableAtWriteEnd;

    if (!binding.control) {
        return snapshot;
    }

    const auto& control = *binding.control;
    snapshot.ioBeginReadCount = control.counters.ioBeginReadCount.load(std::memory_order_relaxed);
    snapshot.ioWriteEndCount = control.counters.ioWriteEndCount.load(std::memory_order_relaxed);

    snapshot.inputBeginReadSampleFrame =
        control.client.inputBeginReadSampleFrame.load(std::memory_order_relaxed);
    snapshot.inputClientReadEndFrame =
        control.client.inputClientReadEndFrame.load(std::memory_order_acquire);

    snapshot.outputWriteEndSampleFrame =
        control.client.outputWriteEndSampleFrame.load(std::memory_order_relaxed);
    snapshot.outputClientWriteEndFrame =
        control.client.outputClientWriteEndFrame.load(std::memory_order_acquire);

    snapshot.inputBeginReadFrameCount =
        control.client.inputBeginReadFrames.load(std::memory_order_relaxed);
    snapshot.outputWriteEndFrameCount =
        control.client.outputWriteEndFrames.load(std::memory_order_relaxed);

    snapshot.directTxPackets = control.counters.txPackets.load(std::memory_order_relaxed);
    snapshot.directTxUnderruns = control.counters.txUnderruns.load(std::memory_order_relaxed);
    snapshot.directTxSilenceSubstitutions =
        control.counters.txSilenceSubstitutions.load(std::memory_order_relaxed);

    snapshot.txValidPhasePcmPackets = control.counters.txValidPhasePcmPackets.load(std::memory_order_relaxed);
    snapshot.txValidPhaseSilencePackets = control.counters.txValidPhaseSilencePackets.load(std::memory_order_relaxed);
    snapshot.txNoPhaseSilencePackets = control.counters.txNoPhaseSilencePackets.load(std::memory_order_relaxed);
    snapshot.txUnderrunSilencePackets = control.counters.txUnderrunSilencePackets.load(std::memory_order_relaxed);
    snapshot.txStaleSyncPackets = control.counters.txStaleSyncPackets.load(std::memory_order_relaxed);
    snapshot.txInvalidGeometryPackets = control.counters.txInvalidGeometryPackets.load(std::memory_order_relaxed);

    return snapshot;
}

[[nodiscard]] inline bool ShouldLogDirectAudioDebugSnapshot(
    DirectAudioDebugLogState& state,
    const DirectAudioDebugSnapshot& snapshot,
    uint64_t nowNs,
    uint64_t intervalNs = kDirectAudioDebugLogIntervalNs) noexcept {
    const bool first = !state.hasLogged;
    const bool boundChanged = state.hasLogged && state.lastBound != snapshot.bound;
    const bool intervalElapsed =
        state.hasLogged &&
        intervalNs > 0 &&
        nowNs >= state.lastLogTimeNs &&
        (nowNs - state.lastLogTimeNs) >= intervalNs;

    if (!first && !boundChanged && !(snapshot.bound && intervalElapsed)) {
        return false;
    }

    state.lastLogTimeNs = nowNs;
    state.hasLogged = true;
    state.lastBound = snapshot.bound;
    return true;
}

} // namespace ASFW::Audio::Runtime
