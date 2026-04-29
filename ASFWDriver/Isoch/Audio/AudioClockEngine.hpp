#pragma once

#include "AudioIOPath.hpp"
#include "../../Shared/TxSharedQueue.hpp"
#include "../Encoding/PacketAssembler.hpp"

#include <AudioDriverKit/AudioDriverKit.h>

#include <atomic>
#include <cstdint>

namespace ASFW::Isoch::Audio {

struct IOMetricsState {
    std::atomic<uint64_t> totalFramesReceived{0};
    std::atomic<uint64_t> totalFramesSent{0};
    std::atomic<uint64_t> callbackCount{0};
    std::atomic<uint64_t> underruns{0};
    std::atomic<uint64_t> outputCallbackCount{0};
    std::atomic<uint64_t> outputFramesRequested{0};
    std::atomic<uint64_t> outputFramesWritten{0};
    std::atomic<uint64_t> outputShortWriteCount{0};
    std::atomic<uint32_t> lastIoBufferFrameSize{0};
    std::atomic<uint64_t> lastCallbackSampleTime{0};
    std::atomic<int64_t> lastCallbackSampleDelta{0};
    std::atomic<uint32_t> lastCallbackOperation{0};
    std::atomic<uint32_t> lastRxQueueFillFrames{0};
    std::atomic<uint32_t> lastTxQueueFillFrames{0};
    std::atomic<uint32_t> lastAssemblerFillFrames{0};
    std::atomic<uint32_t> lastOutputFramesRequested{0};
    std::atomic<uint32_t> lastOutputFramesWritten{0};
    std::atomic<uint32_t> lastOutputWriteShortfall{0};
    uint64_t startTime{0};
};

struct EncodingMetricsState {
    uint64_t packetsGenerated{0};
    uint64_t dataPackets{0};
    uint64_t noDataPackets{0};
    uint64_t overruns{0};
    uint64_t lastLogPackets{0};
    double lastLogElapsedSec{0.0};
};

enum class ClockAuthority : uint8_t {
    kFallback = 0,
    kTransportRx = 1,
};

struct AudioTimingModel {
    uint64_t transportAnchorSample{0};
    uint64_t transportAnchorHost{0};
    bool transportFresh{false};
    bool startupAligned{false};
    uint64_t inputSampleOffset{0};
    uint64_t outputSampleOffset{0};
    ClockAuthority clockAuthority{ClockAuthority::kFallback};

    // Transport-derived cadence metadata rides alongside the authority model so
    // the publisher does not need to re-interpret raw transport snapshots.
    uint32_t transportHostNanosPerSampleQ8{0};
    uint32_t transportSeq{0};
};

struct ClockSyncState {
    uint32_t targetFillLevel{0};
    int64_t fillErrorIntegral{0};
    int32_t lastFillError{0};

    double nominalTicksPerBuffer{0};
    double currentTicksPerBuffer{0};
    double fractionalTicks{0.0};

    uint64_t adjustmentCount{0};
    double maxCorrectionPpm{0.0};

    uint64_t saturationCount{0};
    bool wasSaturated{false};
    int32_t driftDirection{0};
    uint32_t monotoneDriftTicks{0};
    AudioTimingModel timingModel;
};

struct AudioClockEngineState {
    IOUserAudioDevice* audioDevice{nullptr};
    IOTimerDispatchSource* timestampTimer{nullptr};

    bool txQueueValid{false};
    ASFW::Shared::TxSharedQueueSPSC* txQueueWriter{nullptr};
    bool rxQueueValid{false};
    ASFW::Shared::TxSharedQueueSPSC* rxQueueReader{nullptr};

    bool zeroCopyEnabled{false};
    uint32_t zeroCopyFrameCapacity{0};
    ZeroCopyTimelineState* zeroCopyTimeline{nullptr};

    uint32_t ioBufferPeriodFrames{0};
    double currentSampleRate{0.0};
    uint64_t* hostTicksPerBuffer{nullptr};
    ClockSyncState* clockSync{nullptr};

    IOMetricsState* ioMetrics{nullptr};
    uint64_t* metricsLogCounter{nullptr};
    ASFW::Encoding::PacketAssembler* packetAssembler{nullptr};
    EncodingMetricsState* encodingMetrics{nullptr};

    bool* rxStartupDrained{nullptr};
};

void PrepareClockEngineForStart(AudioClockEngineState& state);
void PrepareClockEngineForStop(AudioClockEngineState& state);
void HandleClockTimerTick(AudioClockEngineState& state, uint64_t time);

uint32_t LegacyTxStartupPrefillTargetFrames(uint32_t capacityFrames) noexcept;
uint32_t PrimeLegacyTxQueueForTransportStart(ASFW::Shared::TxSharedQueueSPSC& queue) noexcept;

} // namespace ASFW::Isoch::Audio
