#include "AudioClockEngine.hpp"

#include "../../Logging/LogConfig.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/DriverKit.h>

#include <cmath>

namespace ASFW::Isoch::Audio {
namespace detail {

constexpr uint32_t kLegacyTxStartupPrefillMaxFrames = 2048;
constexpr uint32_t kLegacyTxStartupPrefillMinFrames = 512;

void ResetZeroCopyTimeline(ZeroCopyTimelineState& timeline) {
    timeline.valid = false;
    timeline.lastSampleTime = 0;
    timeline.publishedSampleTime = 0;
    timeline.discontinuities = 0;
    timeline.phaseFrames = 0;
}

const char* ClockAuthorityName(ClockAuthority authority) {
    switch (authority) {
        case ClockAuthority::kTransportRx:
            return "transport-rx";
        case ClockAuthority::kFallback:
        default:
            return "fallback";
    }
}

void ResetTimingModel(AudioTimingModel& model) {
    model.transportAnchorSample = 0;
    model.transportAnchorHost = 0;
    model.transportFresh = false;
    model.startupAligned = false;
    model.inputSampleOffset = 0;
    model.outputSampleOffset = 0;
    model.clockAuthority = ClockAuthority::kFallback;
    model.transportHostNanosPerSampleQ8 = 0;
    model.transportSeq = 0;
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
    ResetTimingModel(clockSync.timingModel);
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

double HostTicksPerSampleFromQ8(uint32_t q8) {
    if (q8 == 0) {
        return 0.0;
    }
    const double nanosPerSample = q8 / 256.0;
    struct mach_timebase_info tb;
    mach_timebase_info(&tb);
    return nanosPerSample * static_cast<double>(tb.denom) / static_cast<double>(tb.numer);
}

double SampleRateFromQ8(uint32_t q8) {
    if (q8 == 0) {
        return 0.0;
    }
    const double nanosPerSample = q8 / 256.0;
    return (nanosPerSample > 0.0) ? (1e9 / nanosPerSample) : 0.0;
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

void RefreshTimingModel(AudioClockEngineState& state,
                        uint64_t nowTicks,
                        uint32_t fallbackQ8,
                        uint64_t hostTicksPerBuffer) {
    auto& model = state.clockSync->timingModel;
    model.transportFresh = false;
    model.clockAuthority = ClockAuthority::kFallback;

    if (!state.rxQueueValid || !state.rxQueueReader) {
        return;
    }

    const auto transportTiming = state.rxQueueReader->ReadTransportTiming();
    if (!transportTiming.valid || transportTiming.anchorHostTicks == 0) {
        return;
    }

    model.transportAnchorSample = transportTiming.anchorSampleFrame;
    model.transportAnchorHost = transportTiming.anchorHostTicks;
    model.transportHostNanosPerSampleQ8 =
        (transportTiming.hostNanosPerSampleQ8 != 0)
            ? transportTiming.hostNanosPerSampleQ8
            : fallbackQ8;
    model.transportSeq = transportTiming.seq;

    const auto startupAlignment = state.rxQueueReader->ReadStartupAlignment();
    if (startupAlignment.valid) {
        const bool wasAligned = model.startupAligned;
        model.inputSampleOffset = startupAlignment.inputSampleOffset;
        model.outputSampleOffset = startupAlignment.outputSampleOffset;
        model.startupAligned = true;

        if (!wasAligned) {
            ASFW_LOG_RL(Audio,
                        "zts/model",
                        1000,
                        OS_LOG_TYPE_DEFAULT,
                        "ZTS model adopted startup alignment seq=%u transportSeq=%u sample=%llu inOff=%llu outOff=%llu",
                        startupAlignment.seq,
                        model.transportSeq,
                        startupAlignment.sampleTime,
                        model.inputSampleOffset,
                        model.outputSampleOffset);
        }
    }

    const uint64_t ageTicks = (nowTicks >= transportTiming.anchorHostTicks)
                                ? (nowTicks - transportTiming.anchorHostTicks)
                                : 0;
    const uint64_t maxFreshTicks = (hostTicksPerBuffer != 0) ? (hostTicksPerBuffer * 2) : 0;
    model.transportFresh = (model.transportHostNanosPerSampleQ8 != 0) &&
                           ((maxFreshTicks == 0) || (ageTicks <= maxFreshTicks));
    if (!model.transportFresh) {
        return;
    }

    model.clockAuthority = ClockAuthority::kTransportRx;
}

bool PublishFromTimingModel(AudioClockEngineState& state,
                            uint64_t nowTicks,
                            uint64_t hostTicksPerBuffer,
                            uint64_t& outSampleTime,
                            uint64_t& outHostTime,
                            uint32_t& outQ8) {
    const auto& model = state.clockSync->timingModel;
    if (model.clockAuthority != ClockAuthority::kTransportRx ||
        !model.transportFresh ||
        !model.startupAligned ||
        model.transportAnchorHost == 0) {
        return false;
    }

    outSampleTime = (model.transportAnchorSample >= model.outputSampleOffset)
                  ? (model.transportAnchorSample - model.outputSampleOffset)
                  : 0;
    outHostTime = model.transportAnchorHost;
    outQ8 = model.transportHostNanosPerSampleQ8;

    const double hostTicksPerSample = HostTicksPerSampleFromQ8(outQ8);
    uint64_t extrapolatedTicks = 0;
    if (nowTicks > model.transportAnchorHost && hostTicksPerSample > 0.0) {
        const uint64_t ageTicks = nowTicks - model.transportAnchorHost;
        const uint64_t maxExtrapolationTicks = hostTicksPerBuffer * 2;
        extrapolatedTicks = (ageTicks > maxExtrapolationTicks) ? maxExtrapolationTicks : ageTicks;
        const auto extrapolatedSamples =
            static_cast<uint64_t>(static_cast<double>(extrapolatedTicks) / hostTicksPerSample);
        outSampleTime += extrapolatedSamples;
        outHostTime += extrapolatedTicks;
    }

    ASFW_LOG_RL(Audio,
                "zts/src",
                1000,
                OS_LOG_TYPE_DEFAULT,
                "ZTS model authority=%{public}s seq=%u sample=%llu host=%llu q8=%u rate=%.2f fresh=%d aligned=%d inOff=%llu outOff=%llu",
                ClockAuthorityName(model.clockAuthority),
                model.transportSeq,
                outSampleTime,
                outHostTime,
                outQ8,
                SampleRateFromQ8(outQ8),
                model.transportFresh,
                model.startupAligned,
                model.inputSampleOffset,
                model.outputSampleOffset);
    return true;
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
    const uint64_t outputCallbacks =
        state.ioMetrics->outputCallbackCount.load(std::memory_order_relaxed);
    const uint64_t outputRequested =
        state.ioMetrics->outputFramesRequested.load(std::memory_order_relaxed);
    const uint64_t outputWritten =
        state.ioMetrics->outputFramesWritten.load(std::memory_order_relaxed);
    const uint64_t outputShortWrites =
        state.ioMetrics->outputShortWriteCount.load(std::memory_order_relaxed);
    const uint32_t lastIoBufferFrameSize =
        state.ioMetrics->lastIoBufferFrameSize.load(std::memory_order_relaxed);
    const int64_t lastCallbackSampleDelta =
        state.ioMetrics->lastCallbackSampleDelta.load(std::memory_order_relaxed);
    const uint32_t lastRxQueueFillFrames =
        state.ioMetrics->lastRxQueueFillFrames.load(std::memory_order_relaxed);
    const uint32_t lastTxQueueFillFrames =
        state.ioMetrics->lastTxQueueFillFrames.load(std::memory_order_relaxed);
    const uint32_t lastAssemblerFillFrames =
        state.ioMetrics->lastAssemblerFillFrames.load(std::memory_order_relaxed);
    const uint32_t lastOutputRequested =
        state.ioMetrics->lastOutputFramesRequested.load(std::memory_order_relaxed);
    const uint32_t lastOutputWritten =
        state.ioMetrics->lastOutputFramesWritten.load(std::memory_order_relaxed);
    const uint32_t lastOutputShortfall =
        state.ioMetrics->lastOutputWriteShortfall.load(std::memory_order_relaxed);

    uint32_t ringFillLevel = lastAssemblerFillFrames;
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

    if (outputCallbacks > 0 || ringUnderruns > 0 || underruns > 0) {
        const uint32_t txFill = state.txQueueValid ? state.txQueueWriter->FillLevelFrames() : 0;
        const auto& model = state.clockSync->timingModel;
        ASFW_LOG(Audio,
                 "IO-TX: %.1fs cb=%llu outCb=%llu outReq=%llu outWrote=%llu short=%llu last(req=%u wrote=%u short=%u) rxFill=%u txFill=%u asmFill=%u ringUnd=%llu q8=%u rate=%.2f authority=%{public}s align(in=%llu out=%llu)",
                 elapsedSec,
                 callbacks,
                 outputCallbacks,
                 outputRequested,
                 outputWritten,
                 outputShortWrites,
                 lastOutputRequested,
                 lastOutputWritten,
                 lastOutputShortfall,
                 rxFill,
                 txFill,
                 ringFillLevel,
                 ringUnderruns,
                 q8,
                 SampleRateFromQ8(q8),
                 ClockAuthorityName(model.clockAuthority),
                 model.inputSampleOffset,
                 model.outputSampleOffset);
    }

    if (::ASFW::LogConfig::Shared().GetIsochVerbosity() >= 3) {
        ASFW_LOG(Audio,
                 "IO: %.1fs recv=%llu sent=%llu (%.0f/s) cb=%llu buf=%u sampleDelta=%lld rxFill=%u txFill=%u asmFill=%u overruns=%llu underruns=%llu/%llu | LocalEnc:%{public}s %llu pkts (%.0f/s, D:%llu N:%llu)",
                 elapsedSec,
                 framesReceived,
                 framesSent,
                 framesPerSec,
                 callbacks,
                 lastIoBufferFrameSize,
                 lastCallbackSampleDelta,
                 lastRxQueueFillFrames,
                 lastTxQueueFillFrames,
                 ringFillLevel,
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
                     "CLK: q8=%u corr=%.1f ppm rxFill=%u txFill=%u authority=%{public}s aligned=%d",
                     q8,
                     corrPpm,
                     rxFill,
                     txFill,
                     ClockAuthorityName(state.clockSync->timingModel.clockAuthority),
                     state.clockSync->timingModel.startupAligned);
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

uint32_t LegacyTxStartupPrefillTargetFrames(uint32_t capacityFrames) noexcept {
    if (capacityFrames == 0) {
        return 0;
    }

    const uint32_t halfCapacity = capacityFrames / 2;
    uint32_t target = (halfCapacity < detail::kLegacyTxStartupPrefillMaxFrames)
                    ? halfCapacity
                    : detail::kLegacyTxStartupPrefillMaxFrames;

    if (target == 0) {
        target = capacityFrames;
    } else if (target < detail::kLegacyTxStartupPrefillMinFrames &&
               capacityFrames >= detail::kLegacyTxStartupPrefillMinFrames) {
        target = detail::kLegacyTxStartupPrefillMinFrames;
    }

    return (target <= capacityFrames) ? target : capacityFrames;
}

uint32_t PrimeLegacyTxQueueForTransportStart(ASFW::Shared::TxSharedQueueSPSC& queue) noexcept {
    if (!queue.IsValid()) {
        return 0;
    }

    const uint32_t dropped = queue.ProducerDropQueuedFrames();
    const uint32_t target = LegacyTxStartupPrefillTargetFrames(queue.CapacityFrames());
    const uint32_t written = queue.WriteSilence(target);
    ASFW_LOG(Audio,
             "ASFWAudioDriver: Primed legacy TX queue with silence target=%u wrote=%u dropped=%u fill=%u cap=%u",
             target,
             written,
             dropped,
             queue.FillLevelFrames(),
             queue.CapacityFrames());
    return written;
}

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
    state.ioMetrics->outputCallbackCount.store(0, std::memory_order_relaxed);
    state.ioMetrics->outputFramesRequested.store(0, std::memory_order_relaxed);
    state.ioMetrics->outputFramesWritten.store(0, std::memory_order_relaxed);
    state.ioMetrics->outputShortWriteCount.store(0, std::memory_order_relaxed);
    state.ioMetrics->lastIoBufferFrameSize.store(0, std::memory_order_relaxed);
    state.ioMetrics->lastCallbackSampleTime.store(0, std::memory_order_relaxed);
    state.ioMetrics->lastCallbackSampleDelta.store(0, std::memory_order_relaxed);
    state.ioMetrics->lastCallbackOperation.store(0, std::memory_order_relaxed);
    state.ioMetrics->lastRxQueueFillFrames.store(0, std::memory_order_relaxed);
    state.ioMetrics->lastTxQueueFillFrames.store(0, std::memory_order_relaxed);
    state.ioMetrics->lastAssemblerFillFrames.store(0, std::memory_order_relaxed);
    state.ioMetrics->lastOutputFramesRequested.store(0, std::memory_order_relaxed);
    state.ioMetrics->lastOutputFramesWritten.store(0, std::memory_order_relaxed);
    state.ioMetrics->lastOutputWriteShortfall.store(0, std::memory_order_relaxed);
    state.ioMetrics->startTime = mach_absolute_time();
    *state.metricsLogCounter = 0;

    state.packetAssembler->reset();
    *state.rxStartupDrained = false;
    detail::ResetZeroCopyTimeline(*state.zeroCopyTimeline);
    if (state.rxQueueValid && state.rxQueueReader) {
        state.rxQueueReader->ResetStartupAlignment();
    }

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

    if (state.txQueueValid && state.txQueueWriter && state.zeroCopyEnabled) {
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
            const uint32_t capacity = state.txQueueWriter ? state.txQueueWriter->CapacityFrames() : 0;
            state.clockSync->targetFillLevel = LegacyTxStartupPrefillTargetFrames(capacity);
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
    if (state.rxQueueValid && state.rxQueueReader) {
        state.rxQueueReader->ResetStartupAlignment();
    }

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

    const uint32_t q8 = state.rxQueueValid ? state.rxQueueReader->CorrHostNanosPerSampleQ8() : 0;
    const uint64_t hostTicksPerBuffer = detail::ComputeHostTicksPerBuffer(state, q8, rxPllReady);
    uint64_t publishedSampleTime = 0;
    uint64_t publishedHostTime = time;
    uint32_t publishedQ8 = q8;
    detail::RefreshTimingModel(state, time, q8, hostTicksPerBuffer);

    if (!detail::PublishFromTimingModel(state,
                                        time,
                                        hostTicksPerBuffer,
                                        publishedSampleTime,
                                        publishedHostTime,
                                        publishedQ8)) {
        uint64_t currentSampleTime = 0;
        uint64_t currentHostTime = 0;
        state.audioDevice->GetCurrentZeroTimestamp(&currentSampleTime, &currentHostTime);

        if (currentHostTime != 0) {
            currentSampleTime += state.ioBufferPeriodFrames;
            currentHostTime += hostTicksPerBuffer;
        } else {
            currentSampleTime = 0;
            currentHostTime = time;
        }

        publishedSampleTime = currentSampleTime;
        publishedHostTime = currentHostTime;
        ASFW_LOG_RL(Audio,
                    "zts/fallback",
                    1000,
                    OS_LOG_TYPE_DEFAULT,
                    "ZTS fallback synthetic sample=%llu host=%llu q8=%u aligned=%d",
                    publishedSampleTime,
                    publishedHostTime,
                    q8,
                    state.clockSync->timingModel.startupAligned);
    }

    state.audioDevice->UpdateCurrentZeroTimestamp(publishedSampleTime, publishedHostTime);
    state.timestampTimer->WakeAtTime(kIOTimerClockMachAbsoluteTime,
                                     time + hostTicksPerBuffer,
                                     0);

    detail::LogPeriodicMetrics(state, time, localEncodingActive, rxFill, rxPllReady, q8);

    if (localEncodingActive) {
        detail::DrainLocalEncoding(state);
    }
}

} // namespace ASFW::Isoch::Audio
