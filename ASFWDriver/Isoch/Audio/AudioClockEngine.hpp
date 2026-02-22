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

} // namespace ASFW::Isoch::Audio
