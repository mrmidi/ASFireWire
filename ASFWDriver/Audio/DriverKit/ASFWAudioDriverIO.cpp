//
// ASFWAudioDriverIO.cpp
// ASFWDriver
//
// Real-time IO callback installation for ASFWAudioDriver.
//

#include "ASFWAudioDriverPrivate.hpp"
#include "../Runtime/PlaybackRingRange.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/DriverKit.h>

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
                const auto stageResult =
                    driverIvars->runtime.txPcmStagingRing.Stage({
                        .interleavedFloat32 = memory.outputBase,
                        .firstFrame = sampleTime,
                        .frameCount = ioBufferFrameSize,
                        .frameCapacity = memory.outputFrameCapacity,
                        .channels = memory.outputChannels,
                    });
                if (stageResult ==
                        ASFW::Audio::Runtime::TxPcmStageResult::kInvalidView ||
                    stageResult ==
                        ASFW::Audio::Runtime::TxPcmStageResult::kNotConfigured) {
                    return returnError(kIOReturnNotReady);
                }
                if (stageResult ==
                    ASFW::Audio::Runtime::TxPcmStageResult::kDuplicate) {
                    // A retried/out-of-order callback must not move W backward
                    // or reinterpret an old HAL span as newly writable PCM.
                    control->counters.CountWriteEnd();
                    return kIOReturnSuccess;
                }

                // Publish W only after the complete callback range has been
                // copied into durable staging. An acquire-reader that observes
                // this frontier can therefore always snapshot every frame below
                // it unless the bounded staging ring explicitly reports stale.
                control->client.PublishWriteEnd(
                    sampleTime, hostTime, ioBufferFrameSize);
                PublishPlaybackRingWriteEnd(
                    driverIvars->runtime.directAudioGraph, *control);

                // Keep packet preparation driven by the CoreAudio write
                // frontier as well as the OHCI refill path. The target is a
                // completed host-write frontier, not a request for transport
                // to manipulate audio cursors. Future PCM does not exist and
                // must never be represented by release-committed zero-filled
                // DATA placeholders. The
                // coalescing latch ensures this RT callback produces at most
                // one outstanding action.
                const uint64_t writeEndFrame = sampleTime + ioBufferFrameSize;
                const uint64_t targetFrameEnd = writeEndFrame;
                const uint64_t requestGeneration =
                    control->txPreparationRequests.PublishRequest(
                        hostTime, targetFrameEnd);
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
