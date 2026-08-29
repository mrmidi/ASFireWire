//
// ASFWAudioDriverIO.cpp
// ASFWDriver
//
// Real-time IO callback installation for ASFWAudioDriver.
//

#include "ASFWAudioDriverPrivate.hpp"
#include "../Runtime/PlaybackRingRange.hpp"
#include "../../Common/TimingUtils.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/DriverKit.h>

#include <atomic>
#include <cstring>

namespace ASFW::Audio::DriverKit {
namespace {

void PublishPlaybackRingWriteEnd(ASFW::Audio::Runtime::AudioGraphBinding& graph,
                                 ASFW::Audio::Runtime::AudioTransportControlBlock& control) noexcept {
    const uint64_t writeStart =
        control.client.outputWriteEndSampleFrame.load(std::memory_order_relaxed);
    const uint64_t writeEnd = control.client.OutputWrittenEndFrame();
    const uint64_t previous =
        control.playbackRingWriteFrame.load(std::memory_order_acquire);
    const uint64_t previousOldest =
        control.playbackRingOldestValidFrame.load(std::memory_order_acquire);
    const uint64_t consumed =
        control.playbackRingReadFrame.load(std::memory_order_acquire);
    const uint32_t capacity = graph.memory.outputFrameCapacity;
    const auto update = ASFW::Audio::Runtime::UpdatePlaybackRingRange(
        previous, previousOldest, writeStart, writeEnd, consumed, capacity);
    if (update.writtenEndFrame == previous) {
        return;
    }

    control.playbackRingOldestValidFrame.store(update.oldestValidFrame,
                                               std::memory_order_relaxed);
    if (update.discontinuity) {
        control.playbackRingDiscontinuityGeneration.fetch_add(1, std::memory_order_relaxed);
        control.discontinuities.fetch_add(1, std::memory_order_relaxed);
    }
    if (update.overrun) {
        control.playbackRingOverruns.fetch_add(1, std::memory_order_relaxed);
    }
    control.playbackRingWriteFrame.store(update.writtenEndFrame, std::memory_order_release);
}


void ZeroInputFrameIfMissing(ASFW::Audio::Runtime::AudioGraphBinding& graph,
                             uint64_t absoluteFrame) noexcept {
    auto* frame = graph.memory.InputFrame(absoluteFrame);
    if (!frame || graph.memory.inputChannels == 0) {
        return;
    }
    std::memset(frame,
                0,
                static_cast<size_t>(graph.memory.inputChannels) * sizeof(int32_t));
}

// A fixed budget spent from the first BeginRead is the wrong instrument here.
// On a device whose stream warms up with NO-DATA the entire budget burns during
// the warm-up window, every record reads `write=0`, and the steady state — the
// only state that says whether capture works — is never sampled. Report the
// *verdict* instead and log only when it changes, so a run costs a couple of
// lines and any transition into or out of starvation is always captured.
enum class CaptureReadVerdict : uint8_t {
    kUnknown = 0,
    kHealthy,        ///< Every requested frame came from the writer.
    kPartialStarve,  ///< Some frames were zero-filled.
    kTotalStarve,    ///< Nothing overlapped the writer's range.
};

[[nodiscard]] const char* CaptureReadVerdictName(CaptureReadVerdict verdict) noexcept {
    switch (verdict) {
        case CaptureReadVerdict::kHealthy:       return "healthy";
        case CaptureReadVerdict::kPartialStarve: return "partial-starve";
        case CaptureReadVerdict::kTotalStarve:   return "total-starve";
        case CaptureReadVerdict::kUnknown:       break;
    }
    return "unknown";
}

std::atomic<uint32_t> gCaptureReadVerdict{
    static_cast<uint32_t>(CaptureReadVerdict::kUnknown)};

// Transitions are rare by construction, but a stream oscillating on the edge of
// the ring could still flap at the IO rate. Cap the total so it can never
// become a hot-path log source.
constexpr uint32_t kCaptureReadTransitionBudget = 24;
std::atomic<uint32_t> gCaptureReadTransitionBudget{kCaptureReadTransitionBudget};

void UpdateMaximum(std::atomic<uint64_t>& target, uint64_t value) noexcept {
    uint64_t previous = target.load(std::memory_order_relaxed);
    while (value > previous &&
           !target.compare_exchange_weak(previous, value,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
    }
}

void RecordPcmPublicationCost(
    ASFW::Audio::Runtime::PcmPublicationTelemetry& telemetry,
    uint32_t frames,
    uint64_t durationTicks) noexcept {
    UpdateMaximum(telemetry.maximumPublicationFrames, frames);
    UpdateMaximum(telemetry.maximumPublicationDurationTicks, durationTicks);
    const uint32_t spanBucket = frames <= 32 ? 0 : frames <= 128 ? 1
        : frames <= 512 ? 2 : frames <= 4'096 ? 3 : 4;
    telemetry.publicationSpanHistogram[spanBucket].fetch_add(
        1, std::memory_order_relaxed);
    const uint64_t nanos = ASFW::Timing::hostTicksToNanos(durationTicks);
    const uint32_t durationBucket = nanos <= 50'000 ? 0
        : nanos <= 100'000 ? 1 : nanos <= 250'000 ? 2
        : nanos <= 500'000 ? 3 : 4;
    telemetry.publicationDurationHistogram[durationBucket].fetch_add(
        1, std::memory_order_relaxed);
}

bool PrepareCaptureRingForBeginRead(ASFW::Audio::Runtime::AudioGraphBinding& graph,
                                    ASFW::Audio::Runtime::AudioTransportControlBlock& control,
                                    uint64_t sampleTime,
                                    uint32_t frameCount) noexcept {
    if (frameCount == 0) {
        return true;
    }
    if (!graph.HasInput()) {
        return false;
    }

    const uint64_t write =
        control.captureRingWriteFrame.load(std::memory_order_acquire);
    const uint32_t capacity = graph.memory.inputFrameCapacity;
    const uint64_t oldest = (capacity != 0 && write > capacity) ? (write - capacity) : 0;
    bool starved = false;
    uint32_t starvedFrames = 0;
    for (uint32_t i = 0; i < frameCount; ++i) {
        const uint64_t frame = sampleTime + i;
        if (frame < oldest || frame >= write) {
            ZeroInputFrameIfMissing(graph, frame);
            starved = true;
            ++starvedFrames;
        }
    }

    // The HAL reads at `sampleTime`, which comes from the ZTS timeline; the RX
    // consumer writes at its own absolute frame cursor. If those two numbering
    // schemes do not share an origin, almost every requested frame falls
    // outside [oldest, write) and is zero-filled, and capture presents as
    // silence broken by isolated samples wherever the ranges happen to overlap.
    // `delta` is the whole diagnosis: 0 means the writer is exactly at the read
    // point, a large or drifting value means the two timelines are unrelated.
    const CaptureReadVerdict verdict =
        starvedFrames == 0          ? CaptureReadVerdict::kHealthy
        : starvedFrames < frameCount ? CaptureReadVerdict::kPartialStarve
                                     : CaptureReadVerdict::kTotalStarve;
    const auto previousVerdict = static_cast<CaptureReadVerdict>(
        gCaptureReadVerdict.exchange(static_cast<uint32_t>(verdict),
                                     std::memory_order_relaxed));
    if (verdict != previousVerdict &&
        gCaptureReadTransitionBudget.load(std::memory_order_relaxed) != 0) {
        gCaptureReadTransitionBudget.fetch_sub(1, std::memory_order_relaxed);
        ASFW_LOG(DirectAudio,
                 "[RxRead] %{public}s -> %{public}s sampleTime=%llu frames=%u "
                 "write=%llu oldest=%llu delta=%lld starvedFrames=%u capacity=%u",
                 CaptureReadVerdictName(previousVerdict),
                 CaptureReadVerdictName(verdict),
                 sampleTime, frameCount, write, oldest,
                 static_cast<long long>(static_cast<int64_t>(write) -
                                        static_cast<int64_t>(sampleTime)),
                 starvedFrames, capacity);
    }

    const uint64_t readEnd = sampleTime + frameCount;
    const uint64_t previousRead =
        control.captureRingReadFrame.load(std::memory_order_acquire);
    if (readEnd > previousRead) {
        control.captureRingReadFrame.store(readEnd, std::memory_order_release);
    }
    if (starved) {
        control.captureRingStarvations.fetch_add(1, std::memory_order_relaxed);
        control.rxCaptureBufferTelemetry.RecordStarvation(starvedFrames);
    }
    control.rxCaptureBufferTelemetry.Observe(
        write, readEnd, capacity);
    return true;
}

} // namespace

kern_return_t InstallIOOperationHandler(IOUserAudioDevice& audioDevice,
                                        ASFWAudioDriver_IVars& ivars) noexcept {
    gCaptureReadVerdict.store(static_cast<uint32_t>(CaptureReadVerdict::kUnknown),
                              std::memory_order_relaxed);
    gCaptureReadTransitionBudget.store(kCaptureReadTransitionBudget,
                                       std::memory_order_relaxed);
    auto* driverIvars = &ivars;
    const kern_return_t error = audioDevice.SetIOOperationHandler(
        ^kern_return_t(IOUserAudioObjectID           objectID,
                       IOUserAudioIOOperation        operation,
                       uint32_t                      ioBufferFrameSize,
                       uint64_t                      sampleTime,
                       uint64_t                      hostTime)
    {
        if (!driverIvars) {
            return kIOReturnNotReady;
        }

        auto* graphControl = driverIvars->runtime.directAudioGraph.control;
        auto& callbackState = graphControl
            ? *graphControl
            : driverIvars->runtime.directAudioControl;

        auto returnError = [&](kern_return_t kr) noexcept {
            callbackState.ioLastError.store(
                static_cast<uint32_t>(kr), std::memory_order_relaxed);
            callbackState.ioLastErrorOperation.store(
                static_cast<uint32_t>(operation), std::memory_order_relaxed);
            callbackState.ioLastErrorFrameCount.store(
                ioBufferFrameSize, std::memory_order_relaxed);
            callbackState.ioLastErrorObjectId.store(
                objectID, std::memory_order_relaxed);
            callbackState.ioLastErrorSampleTime.store(
                sampleTime, std::memory_order_relaxed);
            callbackState.ioLastErrorHostTime.store(
                hostTime, std::memory_order_relaxed);
            callbackState.ioCallbackErrorGeneration.fetch_add(
                1, std::memory_order_release);
            return kr;
        };

        (void)driverIvars->runtime.ioDebugCallbacks.fetch_add(1, std::memory_order_relaxed);
        callbackState.ioLastOperation.store(
            static_cast<uint32_t>(operation), std::memory_order_relaxed);
        callbackState.ioLastFrameCount.store(
            ioBufferFrameSize, std::memory_order_relaxed);
        callbackState.ioLastObjectId.store(
            objectID, std::memory_order_relaxed);
        callbackState.ioLastSampleTime.store(
            sampleTime, std::memory_order_relaxed);
        callbackState.ioLastHostTime.store(
            hostTime, std::memory_order_relaxed);
        callbackState.ioCallbackGeneration.fetch_add(
            1, std::memory_order_release);

        const bool running = driverIvars->runtime.isRunning.load(std::memory_order_acquire);
        const bool skeletonBound =
            driverIvars->runtime.directAudioSkeletonBound.load(std::memory_order_acquire);

        if (!running) {
            driverIvars->runtime.ioCallbacksOutsideRun.fetch_add(
                1, std::memory_order_relaxed);
            return kIOReturnSuccess;
        }

        if (skeletonBound) {
            auto* control = graphControl;
            if (!control) {
                return returnError(kIOReturnNotReady);
            }

            if (operation == IOUserAudioIOOperationBeginRead) {
                // ADK permits operation spans that differ from the nominal IO
                // size. The stream ring capacity is the actual hard bound.
                if (ioBufferFrameSize >
                    driverIvars->runtime.directAudioGraph.memory.inputFrameCapacity) {
                    return returnError(kIOReturnBadArgument);
                }
                control->client.PublishBeginRead(sampleTime, hostTime, ioBufferFrameSize);
                control->rxCaptureBufferTelemetry.RecordReaderBeginRead();
                (void)PrepareCaptureRingForBeginRead(driverIvars->runtime.directAudioGraph,
                                                     *control,
                                                     sampleTime,
                                                     ioBufferFrameSize);
                control->counters.CountBeginRead();
            } else if (operation == IOUserAudioIOOperationWriteEnd) {
                // See BeginRead above: CoreAudio may choose a larger span than
                // kHalIoPeriodFrames while remaining within the stream ring.
                if (ioBufferFrameSize >
                    driverIvars->runtime.directAudioGraph.memory.outputFrameCapacity) {
                    return returnError(kIOReturnBadArgument);
                }
                const auto& memory =
                    driverIvars->runtime.directAudioGraph.memory;
                const uint64_t publicationStart = mach_absolute_time();
                const auto publishResult =
                    driverIvars->runtime.pcmPublicationCache.Publish({
                        .interleavedFloat32 = memory.outputBase,
                        .epoch = control->hardwareTimeline.Epoch(),
                        .firstFrame = sampleTime,
                        .frameCount = ioBufferFrameSize,
                        .frameCapacity = memory.outputFrameCapacity,
                        .channels = memory.outputChannels,
                    });
                RecordPcmPublicationCost(
                    control->pcmPublicationTelemetry, ioBufferFrameSize,
                    mach_absolute_time() - publicationStart);
                if (publishResult ==
                        ASFW::Audio::Runtime::PcmPublishResult::InvalidView ||
                    publishResult ==
                        ASFW::Audio::Runtime::PcmPublishResult::NotConfigured ||
                    publishResult ==
                        ASFW::Audio::Runtime::PcmPublishResult::WrongEpoch) {
                    return returnError(kIOReturnNotReady);
                }
                if (publishResult ==
                    ASFW::Audio::Runtime::PcmPublishResult::Duplicate) {
                    // Diagnose a duplicate final-publication callback without
                    // moving hardware time or any packet cursor.
                    control->counters.CountWriteEnd();
                    return kIOReturnSuccess;
                }

                // Publish W only after the complete callback range has been
                // copied into the immutable publication cache.
                control->client.PublishWriteEnd(
                    sampleTime, hostTime, ioBufferFrameSize);
                PublishPlaybackRingWriteEnd(
                    driverIvars->runtime.directAudioGraph, *control);

                // Wake the physical scheduler after bytes become visible. The
                // write frontier is deliberately not a preparation horizon or
                // a timing coordinate.
                const uint64_t requestGeneration =
                    control->txPreparationRequests.PublishRequest(
                        hostTime);
                if (driverIvars->device.audioNub &&
                    control->txPreparationRequests.TryScheduleWake()) {
                    const kern_return_t requestKr =
                        driverIvars->device.audioNub->RequestTxPreparation(
                            requestGeneration);
                    if (requestKr != kIOReturnSuccess) {
                        control->txPreparationRequests.FinishWake();
                    }
                }

                control->counters.CountWriteEnd();
            } else {
                return returnError(kIOReturnBadArgument);
            }
        } else {
            return returnError(kIOReturnNotReady);
        }

        return kIOReturnSuccess;
    });

    if (error == kIOReturnSuccess) {
        ASFW_LOG(DirectAudio,
                 "ADK IO handler installed deviceId=%u inputStream=%u outputStream=%u",
                 audioDevice.GetObjectID(),
                 ivars.inputStream ? ivars.inputStream->GetObjectID() : 0,
                 ivars.outputStream ? ivars.outputStream->GetObjectID() : 0);
    } else {
        ASFW_LOG(Audio, "ASFWAudioDriver: SetIOOperationHandler failed: 0x%x", error);
    }
    return error;
}

} // namespace ASFW::Audio::DriverKit
