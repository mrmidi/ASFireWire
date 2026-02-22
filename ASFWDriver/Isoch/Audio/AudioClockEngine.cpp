#include "AudioClockEngine.hpp"

#include "../../Logging/LogConfig.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/DriverKit.h>

#include <cmath>

namespace ASFW::Isoch::Audio {
namespace detail {

void ResetZeroCopyTimeline(ZeroCopyTimelineState& timeline) {
    timeline.valid = false;
    timeline.lastSampleTime = 0;
    timeline.publishedSampleTime = 0;
    timeline.discontinuities = 0;
    timeline.phaseFrames = 0;
}

void ResetClockSync(ClockSyncState& clockSync) {
    clockSync.fillErrorIntegral = 0;
    clockSync.lastFillError = 0;
    clockSync.fractionalTicks = 0.0;
    clockSync.adjustmentCount = 0;
    clockSync.maxCorrectionPpm = 0.0;
    clockSync.saturationCount = 0;
    clockSync.wasSaturated = false;
    clockSync.driftDirection = 0;
    clockSync.monotoneDriftTicks = 0;
}

uint64_t RoundWithFraction(double& fractionalTicks, double currentTicksPerBuffer) {
    const double exactTicks = currentTicksPerBuffer + fractionalTicks;
    const auto roundedTicks = static_cast<uint64_t>(exactTicks);
    fractionalTicks = exactTicks - static_cast<double>(roundedTicks);
    return roundedTicks;
}

uint64_t ApplyCycleTimeClock(AudioClockEngineState& state, uint32_t q8) {
    const double nanosPerSample = q8 / 256.0;
    struct mach_timebase_info tb;
    mach_timebase_info(&tb);
    const double hostTicksPerSample = nanosPerSample * static_cast<double>(tb.denom)
                                    / static_cast<double>(tb.numer);
    state.clockSync->currentTicksPerBuffer = hostTicksPerSample * state.ioBufferPeriodFrames;
    return RoundWithFraction(state.clockSync->fractionalTicks,
                             state.clockSync->currentTicksPerBuffer);
}

uint64_t ApplyZeroCopyPllClock(AudioClockEngineState& state) {
    const uint32_t fillLevel = state.txQueueWriter->FillLevelFrames();
    const int32_t fillError = static_cast<int32_t>(fillLevel)
                            - static_cast<int32_t>(state.clockSync->targetFillLevel);

    constexpr double kMaxPpm = 100.0;
    constexpr int32_t kDeadbandFrames = 8;
    constexpr double kPpmPerFrame = 0.45;
    constexpr double kIppmPerFrameTick = 0.0008;
    constexpr int64_t kIntegralClamp = 200000;

    int32_t controlError = fillError;
    if (std::abs(controlError) <= kDeadbandFrames) {
        controlError = 0;
    }

    double ppmUnclamped = (kPpmPerFrame * controlError)
                        + (kIppmPerFrameTick * static_cast<double>(state.clockSync->fillErrorIntegral));
    if (const bool satHigh = (ppmUnclamped > kMaxPpm) && (controlError > 0),
                  satLow = (ppmUnclamped < -kMaxPpm) && (controlError < 0);
        !(satHigh || satLow)) {
        state.clockSync->fillErrorIntegral += controlError;
        if (state.clockSync->fillErrorIntegral > kIntegralClamp) {
            state.clockSync->fillErrorIntegral = kIntegralClamp;
        }
        if (state.clockSync->fillErrorIntegral < -kIntegralClamp) {
            state.clockSync->fillErrorIntegral = -kIntegralClamp;
        }
    }

    ppmUnclamped = (kPpmPerFrame * controlError)
                 + (kIppmPerFrameTick * static_cast<double>(state.clockSync->fillErrorIntegral));
    double corrPpm = ppmUnclamped;
    if (corrPpm > kMaxPpm) {
        corrPpm = kMaxPpm;
    }
    if (corrPpm < -kMaxPpm) {
        corrPpm = -kMaxPpm;
    }

    const double correction = state.clockSync->nominalTicksPerBuffer * (corrPpm / 1e6);
    state.clockSync->currentTicksPerBuffer = state.clockSync->nominalTicksPerBuffer + correction;
    state.clockSync->lastFillError = fillError;
    state.clockSync->adjustmentCount++;

    if (std::fabs(corrPpm) > state.clockSync->maxCorrectionPpm) {
        state.clockSync->maxCorrectionPpm = std::fabs(corrPpm);
    }

    const bool saturated = (std::fabs(corrPpm) >= kMaxPpm - 0.1);
    if (saturated && !state.clockSync->wasSaturated) {
        state.clockSync->saturationCount++;
        ASFW_LOG_RL(Audio,
                    "pll/sat",
                    500,
                    OS_LOG_TYPE_DEFAULT,
                    "PLL SATURATED corr=%.1f ppm fill=%u target=%u err=%d sat#=%llu",
                    corrPpm,
                    fillLevel,
                    state.clockSync->targetFillLevel,
                    fillError,
                    state.clockSync->saturationCount);
    }
    state.clockSync->wasSaturated = saturated;

    int32_t curDir = 0;
    if (controlError > 0) {
        curDir = 1;
    } else if (controlError < 0) {
        curDir = -1;
    }
    if (curDir != 0 && curDir == state.clockSync->driftDirection) {
        state.clockSync->monotoneDriftTicks++;
        if (state.clockSync->monotoneDriftTicks == 200) {
            ASFW_LOG_RL(Audio,
                        "pll/drift",
                        2000,
                        OS_LOG_TYPE_DEFAULT,
                        "PLL MONOTONE DRIFT dir=%{public}s 200+ ticks fill=%u target=%u",
                        curDir > 0 ? "fast" : "slow",
                        fillLevel,
                        state.clockSync->targetFillLevel);
        }
    } else {
        state.clockSync->driftDirection = curDir;
        state.clockSync->monotoneDriftTicks = (curDir != 0) ? 1 : 0;
    }

    return RoundWithFraction(state.clockSync->fractionalTicks,
                             state.clockSync->currentTicksPerBuffer);
}

uint64_t ApplyNominalClock(AudioClockEngineState& state, bool withLegacyTxUpdate) {
    if (withLegacyTxUpdate) {
        const uint32_t fillLevel = state.txQueueWriter->FillLevelFrames();
        const int32_t fillError = static_cast<int32_t>(fillLevel)
                                - static_cast<int32_t>(state.clockSync->targetFillLevel);
        state.clockSync->lastFillError = fillError;
        state.clockSync->fillErrorIntegral = 0;
        state.clockSync->currentTicksPerBuffer = state.clockSync->nominalTicksPerBuffer;
        state.clockSync->fractionalTicks = 0.0;
        state.clockSync->maxCorrectionPpm = 0.0;
        return static_cast<uint64_t>(state.clockSync->nominalTicksPerBuffer);
    }

    return RoundWithFraction(state.clockSync->fractionalTicks,
                             state.clockSync->currentTicksPerBuffer);
}

uint64_t ComputeHostTicksPerBuffer(AudioClockEngineState& state,
                                   uint32_t q8,
                                   bool rxPllReady) {
    if (q8 > 0) {
        return ApplyCycleTimeClock(state, q8);
    }
    if (state.zeroCopyEnabled && state.txQueueValid) {
        return ApplyZeroCopyPllClock(state);
    }
    if (rxPllReady) {
        return ApplyNominalClock(state, false);
    }
    if (state.txQueueValid && !state.zeroCopyEnabled) {
        return ApplyNominalClock(state, true);
    }
    return static_cast<uint64_t>(state.clockSync->currentTicksPerBuffer);
}

void LogPeriodicMetrics(AudioClockEngineState& state,
                        uint64_t time,
                        bool localEncodingActive,
                        uint32_t rxFill,
                        bool rxPllReady,
                        uint32_t q8) {
    if (++(*state.metricsLogCounter) % 430 != 0) {
        return;
    }

    const uint64_t framesReceived = state.ioMetrics->totalFramesReceived.load(std::memory_order_relaxed);
    const uint64_t framesSent = state.ioMetrics->totalFramesSent.load(std::memory_order_relaxed);
    const uint64_t callbacks = state.ioMetrics->callbackCount.load(std::memory_order_relaxed);
    const uint64_t underruns = state.ioMetrics->underruns.load(std::memory_order_relaxed);

    uint32_t ringFillLevel = 0;
    uint64_t ringUnderruns = 0;
    if (localEncodingActive) {
        ringFillLevel = state.packetAssembler->bufferFillLevel();
        ringUnderruns = state.packetAssembler->underrunCount();
    }

    const uint64_t elapsed = time - state.ioMetrics->startTime;
    struct mach_timebase_info timebase;
    mach_timebase_info(&timebase);
    const double elapsedSec = static_cast<double>(elapsed) * static_cast<double>(timebase.numer)
                            / static_cast<double>(timebase.denom)
                            / 1e9;

    if (elapsedSec <= 0.0) {
        return;
    }

    const double framesPerSec = static_cast<double>(framesReceived) / elapsedSec;
    const double dt = elapsedSec - state.encodingMetrics->lastLogElapsedSec;
    const uint64_t dp = state.encodingMetrics->packetsGenerated - state.encodingMetrics->lastLogPackets;
    const double packetsPerSec = (dt > 0.0) ? static_cast<double>(dp) / dt : 0.0;

    if (::ASFW::LogConfig::Shared().GetIsochVerbosity() >= 3) {
        ASFW_LOG(Audio,
                 "IO: %.1fs recv=%llu sent=%llu (%.0f/s) cb=%llu ring=%u rxFill=%u overruns=%llu underruns=%llu/%llu | LocalEnc:%{public}s %llu pkts (%.0f/s, D:%llu N:%llu)",
                 elapsedSec,
                 framesReceived,
                 framesSent,
                 framesPerSec,
                 callbacks,
                 ringFillLevel,
                 rxFill,
                 state.encodingMetrics->overruns,
                 underruns,
                 ringUnderruns,
                 localEncodingActive ? "ON" : "OFF",
                 state.encodingMetrics->packetsGenerated,
                 packetsPerSec,
                 state.encodingMetrics->dataPackets,
                 state.encodingMetrics->noDataPackets);

        const double corrPpm = ((state.clockSync->currentTicksPerBuffer
                               - state.clockSync->nominalTicksPerBuffer)
                               / state.clockSync->nominalTicksPerBuffer) * 1e6;
        if (q8 > 0) {
            const uint32_t txFill = state.txQueueValid ? state.txQueueWriter->FillLevelFrames() : 0;
            ASFW_LOG(Audio,
                     "CLK: q8=%u corr=%.1f ppm rxFill=%u txFill=%u (cycle-time, unified)",
                     q8,
                     corrPpm,
                     rxFill,
                     txFill);
        } else if (state.zeroCopyEnabled && state.txQueueValid) {
            const uint32_t fill = state.txQueueWriter->FillLevelFrames();
            ASFW_LOG(Audio,
                     "CLK-TX: fill=%u target=%u err=%d integral=%lld corr=%.1f ppm (max=%.1f) zcDisc=%llu",
                     fill,
                     state.clockSync->targetFillLevel,
                     state.clockSync->lastFillError,
                     state.clockSync->fillErrorIntegral,
                     corrPpm,
                     state.clockSync->maxCorrectionPpm,
                     state.zeroCopyTimeline->discontinuities);
        } else if (rxPllReady) {
            ASFW_LOG(Audio,
                     "CLK-RX: fill=%u corr=0.0 ppm q8=0 (awaiting cycle-time)",
                     rxFill);
        } else if (state.txQueueValid) {
            const uint32_t fill = state.txQueueWriter->FillLevelFrames();
            ASFW_LOG(Audio,
                     "CLK: fill=%u target=%u err=%d nominal (legacy TX path)",
                     fill,
                     state.clockSync->targetFillLevel,
                     state.clockSync->lastFillError);
        }
    }

    state.encodingMetrics->lastLogPackets = state.encodingMetrics->packetsGenerated;
    state.encodingMetrics->lastLogElapsedSec = elapsedSec;
}

void DrainLocalEncoding(AudioClockEngineState& state) {
    while (state.packetAssembler->bufferFillLevel() >= state.packetAssembler->samplesPerDataPacket()) {
        const auto packet = state.packetAssembler->assembleNext(0xFFFF);
        state.encodingMetrics->packetsGenerated++;
        if (packet.isData) {
            state.encodingMetrics->dataPackets++;
        } else {
            state.encodingMetrics->noDataPackets++;
        }
    }
}

} // namespace detail

void PrepareClockEngineForStart(AudioClockEngineState& state) {
    if (!state.audioDevice || !state.timestampTimer || !state.clockSync ||
        !state.hostTicksPerBuffer || !state.ioMetrics || !state.metricsLogCounter ||
        !state.packetAssembler || !state.zeroCopyTimeline || !state.rxStartupDrained) {
        return;
    }

    state.ioMetrics->totalFramesReceived.store(0, std::memory_order_relaxed);
    state.ioMetrics->totalFramesSent.store(0, std::memory_order_relaxed);
    state.ioMetrics->callbackCount.store(0, std::memory_order_relaxed);
    state.ioMetrics->underruns.store(0, std::memory_order_relaxed);
    state.ioMetrics->startTime = mach_absolute_time();
    *state.metricsLogCounter = 0;

    state.packetAssembler->reset();
    *state.rxStartupDrained = false;
    detail::ResetZeroCopyTimeline(*state.zeroCopyTimeline);

    struct mach_timebase_info timebaseInfo;
    mach_timebase_info(&timebaseInfo);

    const double sampleRate = state.currentSampleRate;
    double hostTicksPerBuffer = static_cast<double>(state.ioBufferPeriodFrames * NSEC_PER_SEC) / sampleRate;
    hostTicksPerBuffer = (hostTicksPerBuffer * static_cast<double>(timebaseInfo.denom))
                       / static_cast<double>(timebaseInfo.numer);
    *state.hostTicksPerBuffer = static_cast<uint64_t>(hostTicksPerBuffer);

    state.clockSync->nominalTicksPerBuffer = hostTicksPerBuffer;
    state.clockSync->currentTicksPerBuffer = hostTicksPerBuffer;
    detail::ResetClockSync(*state.clockSync);

    if (state.txQueueValid && state.txQueueWriter) {
        state.txQueueWriter->ProducerSetZeroCopyPhaseFrames(0);
        state.txQueueWriter->ProducerRequestConsumerResync();
    }

    if (state.txQueueValid) {
        if (state.zeroCopyEnabled && state.zeroCopyFrameCapacity > 0) {
            uint32_t target = (state.zeroCopyFrameCapacity * 5) / 8;
            if (target < 8) {
                target = 8;
            }
            state.clockSync->targetFillLevel = target;
        } else {
            state.clockSync->targetFillLevel = 64;
        }
    } else {
        state.clockSync->targetFillLevel = 2048;
    }

    ASFW_LOG(Audio,
             "ASFWAudioDriver: Clock sync target fill=%u (zeroCopy=%{public}s)",
             state.clockSync->targetFillLevel,
             state.zeroCopyEnabled ? "YES" : "NO");

    ASFW_LOG(Audio,
             "ASFWAudioDriver: Timer interval = %llu ticks (%.0f Hz, period=%u frames)",
             *state.hostTicksPerBuffer,
             sampleRate,
             state.ioBufferPeriodFrames);

    state.audioDevice->UpdateCurrentZeroTimestamp(0, 0);

    const uint64_t currentTime = mach_absolute_time();
    state.timestampTimer->WakeAtTime(kIOTimerClockMachAbsoluteTime,
                                     currentTime + *state.hostTicksPerBuffer,
                                     0);
    state.timestampTimer->SetEnable(true);
}

void PrepareClockEngineForStop(AudioClockEngineState& state) {
    if (!state.clockSync || !state.zeroCopyTimeline) {
        return;
    }

    detail::ResetClockSync(*state.clockSync);
    state.zeroCopyTimeline->valid = false;

    if (state.timestampTimer) {
        state.timestampTimer->SetEnable(false);
        ASFW_LOG(Audio, "ASFWAudioDriver: Timestamp timer stopped");
    }
}

void HandleClockTimerTick(AudioClockEngineState& state, uint64_t time) {
    if (!state.audioDevice || !state.timestampTimer || !state.clockSync ||
        !state.ioMetrics || !state.metricsLogCounter || !state.packetAssembler ||
        !state.encodingMetrics || !state.rxQueueReader || !state.zeroCopyTimeline ||
        !state.txQueueWriter) {
        return;
    }

    const bool localEncodingActive = !state.txQueueValid;

    uint32_t rxFill = 0;
    bool rxPllReady = false;
    if (state.rxQueueValid) {
        rxFill = state.rxQueueReader->FillLevelFrames();
        rxPllReady = true;
    }

    uint64_t currentSampleTime = 0;
    uint64_t currentHostTime = 0;
    state.audioDevice->GetCurrentZeroTimestamp(&currentSampleTime, &currentHostTime);

    const uint32_t q8 = state.rxQueueValid ? state.rxQueueReader->CorrHostNanosPerSampleQ8() : 0;
    const uint64_t hostTicksPerBuffer = detail::ComputeHostTicksPerBuffer(state, q8, rxPllReady);

    if (currentHostTime != 0) {
        currentSampleTime += state.ioBufferPeriodFrames;
        currentHostTime += hostTicksPerBuffer;
    } else {
        currentSampleTime = 0;
        currentHostTime = time;
    }

    state.audioDevice->UpdateCurrentZeroTimestamp(currentSampleTime, currentHostTime);
    state.timestampTimer->WakeAtTime(kIOTimerClockMachAbsoluteTime,
                                     currentHostTime + hostTicksPerBuffer,
                                     0);

    detail::LogPeriodicMetrics(state, time, localEncodingActive, rxFill, rxPllReady, q8);

    if (localEncodingActive) {
        detail::DrainLocalEncoding(state);
    }
}

} // namespace ASFW::Isoch::Audio
