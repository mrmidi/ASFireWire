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
    for (uint32_t i = 0; i < frameCount; ++i) {
        const uint64_t frame = sampleTime + i;
        if (frame < oldest || frame >= write) {
            ZeroInputFrameIfMissing(graph, frame);
            starved = true;
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
                control->client.PublishWriteEnd(sampleTime, hostTime, ioBufferFrameSize);
                PublishPlaybackRingWriteEnd(driverIvars->runtime.directAudioGraph, *control);

                const uint32_t channels = driverIvars->runtime.directAudioGraph.memory.outputChannels;
                const auto& memory =
                    driverIvars->runtime.directAudioGraph.memory;
                if (memory.outputBase && channels > 0) {
                    ASFW::Protocols::Audio::AMDTP::HostAudioBufferView hostBuffer{};
                    hostBuffer.interleavedFloat32 = memory.outputBase;
                    hostBuffer.firstFrame = sampleTime;
                    hostBuffer.frameCount = ioBufferFrameSize;
                    hostBuffer.frameCapacity = memory.outputFrameCapacity;
                    hostBuffer.channels = channels;

                    const uint64_t completionCursor = driverIvars->runtime.txSlotProvider.controlBlock
                        ? driverIvars->runtime.txSlotProvider.controlBlock->completionCursor.load(std::memory_order_acquire)
                        : 0;

                    driverIvars->runtime.txStreamEngine.WriteHostOutputFloat32(
                        hostBuffer,
                        completionCursor);

                    const auto& cw = driverIvars->runtime.txStreamEngine.PayloadWriterCounters();
                    ASFW::Audio::Runtime::PayloadWriterTelemetryRecord rec{};
                    rec.sampleTime = sampleTime;
                    rec.completionCursor = completionCursor;
                    rec.exposedFrameEnd = driverIvars->runtime.txStreamEngine.Timeline().ExposedFrameEnd();
                    rec.frameCount = ioBufferFrameSize;
                    rec.frameCapacity = memory.outputFrameCapacity;
                    rec.visited = cw.framesVisited.load(std::memory_order_relaxed);
                    rec.written = cw.framesWritten.load(std::memory_order_relaxed);
                    rec.withoutPacket = cw.framesWithoutPacket.load(std::memory_order_relaxed);
                    rec.outsidePacket = cw.framesOutsidePacket.load(std::memory_order_relaxed);
                    rec.racedReuse = cw.framesRacedReuse.load(std::memory_order_relaxed);
                    rec.wroteIntoTransmitted = cw.framesWroteIntoTransmitted.load(std::memory_order_relaxed);
                    rec.nonZeroFrames = cw.framesNonZero.load(std::memory_order_relaxed);
                    const uint32_t bits = cw.maxAbsSampleBits.load(std::memory_order_relaxed);
                    std::memcpy(&rec.maxAbsSample, &bits, sizeof(bits));

                    rec.playbackRingReadFrame = control->playbackRingReadFrame.load(std::memory_order_relaxed);
                    rec.playbackRingWriteFrame = control->playbackRingWriteFrame.load(std::memory_order_relaxed);
                    rec.outputBaseAddr = reinterpret_cast<uint64_t>(memory.outputBase);
                    rec.captureRingReadFrame = control->captureRingReadFrame.load(std::memory_order_relaxed);
                    rec.captureRingWriteFrame = control->captureRingWriteFrame.load(std::memory_order_relaxed);
                    rec.inputBaseAddr = reinterpret_cast<uint64_t>(memory.inputBase);

                    if (driverIvars->runtime.txSlotProvider.payloadBase &&
                        driverIvars->runtime.txSlotProvider.numSlots > 0 &&
                        completionCursor > 0) {
                        const uint64_t packetIndex = completionCursor - 1;
                        rec.lastReadPacketIndex = packetIndex;
                        const uint32_t slotIdx = static_cast<uint32_t>(packetIndex % driverIvars->runtime.txSlotProvider.numSlots);
                        const uint8_t* lastReadBytes = driverIvars->runtime.txSlotProvider.payloadBase +
                                                       (slotIdx * driverIvars->runtime.txSlotProvider.slotStrideBytes);
                        std::memcpy(rec.lastReadPacketBytes, lastReadBytes, 16);
                    }

                    control->payloadWriterTelemetry.Record(rec);

                    control->playbackRingReadFrame.store(sampleTime + ioBufferFrameSize, std::memory_order_release);
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
