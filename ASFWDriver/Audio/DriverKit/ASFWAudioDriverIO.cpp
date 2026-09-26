//
// ASFWAudioDriverIO.cpp
// ASFWDriver
//
// Real-time IO callback installation for ASFWAudioDriver.
//

#include "ASFWAudioDriverPrivate.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/DriverKit.h>

#include <cstring>

namespace ASFW::Audio::DriverKit {
namespace {

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
    }
    // Until the first complete read the capture ring is still filling behind
    // the HAL clock: count that as start-up, not as S_in headroom.
    if (!control.captureRingFirstCompleteRead.load(std::memory_order_relaxed)) {
        if (starved) {
            control.captureRingStartupStarvations.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        control.captureRingFirstCompleteRead.store(true, std::memory_order_relaxed);
    }
    if (starved) {
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
                if (!HandleOutputWriteEnd(*driverIvars, *control, sampleTime, hostTime,
                                          ioBufferFrameSize)) {
                    return returnError(kIOReturnBadArgument);
                }
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
