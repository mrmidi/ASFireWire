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

        const uint64_t callbackIndex =
            driverIvars->runtime.ioDebugCallbacks.fetch_add(1, std::memory_order_relaxed) + 1;

        if (callbackIndex <= 64 || (callbackIndex % 1024) == 0) {
            ASFW_LOG(DirectAudio,
                     "ADK IO cb entry index=%llu op=%u object=%u sampleTime=%llu hostTime=%llu frameCount=%u running=%d skeleton=%d control=%p hasIn=%d hasOut=%d deviceId=%u inStreamId=%u outStreamId=%u",
                     callbackIndex,
                     static_cast<uint32_t>(operation),
                     objectID,
                     sampleTime,
                     hostTime,
                     ioBufferFrameSize,
                     driverIvars->runtime.isRunning.load(std::memory_order_acquire),
                     driverIvars->runtime.directAudioSkeletonBound.load(std::memory_order_acquire),
                     static_cast<void*>(driverIvars->runtime.directAudioGraph.control),
                     driverIvars->runtime.directAudioGraph.HasInput(),
                     driverIvars->runtime.directAudioGraph.HasOutput(),
                     driverIvars->audioDevice ? driverIvars->audioDevice->GetObjectID() : 0,
                     driverIvars->inputStream ? driverIvars->inputStream->GetObjectID() : 0,
                     driverIvars->outputStream ? driverIvars->outputStream->GetObjectID() : 0);
        }

        const bool running = driverIvars->runtime.isRunning.load(std::memory_order_acquire);
        const bool skeletonBound =
            driverIvars->runtime.directAudioSkeletonBound.load(std::memory_order_acquire);
        if (callbackIndex <= 16 || (callbackIndex % 1024) == 0) {
            ASFW_LOG(DirectAudio,
                     "ADK DBG IO cb=%llu object=%u op=%u running=%d skeleton=%d frames=%u sample=%llu host=%llu period=%u outRing=%u inFrames=%u outFrames=%u",
                     callbackIndex,
                     objectID,
                     static_cast<uint32_t>(operation),
                     running,
                     skeletonBound,
                     ioBufferFrameSize,
                     sampleTime,
                     hostTime,
                     ASFW::Isoch::Config::kAudioIoPeriodFrames,
                     ASFW::Isoch::Config::kAudioOutputRingFrames,
                     driverIvars->runtime.directAudioGraph.memory.inputFrameCapacity,
                     driverIvars->runtime.directAudioGraph.memory.outputFrameCapacity);
        }

        if (!running) {
            if (callbackIndex <= 16 || (callbackIndex % 1024) == 0) {
                ASFW_LOG(DirectAudio,
                         "ADK DBG IO reject=not_running cb=%llu op=%u frames=%u sample=%llu",
                         callbackIndex,
                         static_cast<uint32_t>(operation),
                         ioBufferFrameSize,
                         sampleTime);
            }
            return kIOReturnNotReady;
        }

        if (ioBufferFrameSize > ASFW::Isoch::Config::kAudioIoPeriodFrames) {
            ASFW_LOG(DirectAudio,
                     "ADK DBG IO reject=bad_frames cb=%llu op=%u frames=%u periodMax=%u outRing=%u inFrames=%u outFrames=%u sample=%llu host=%llu",
                     callbackIndex,
                     static_cast<uint32_t>(operation),
                     ioBufferFrameSize,
                     ASFW::Isoch::Config::kAudioIoPeriodFrames,
                     ASFW::Isoch::Config::kAudioOutputRingFrames,
                     driverIvars->runtime.directAudioGraph.memory.inputFrameCapacity,
                     driverIvars->runtime.directAudioGraph.memory.outputFrameCapacity,
                     sampleTime,
                     hostTime);
            return kIOReturnBadArgument;
        }

        if (skeletonBound) {
            auto* control = driverIvars->runtime.directAudioGraph.control;
            if (!control) {
                ASFW_LOG(DirectAudio,
                         "ADK DBG IO reject=no_control cb=%llu op=%u frames=%u sample=%llu host=%llu",
                         callbackIndex,
                         static_cast<uint32_t>(operation),
                         ioBufferFrameSize,
                         sampleTime,
                         hostTime);
                return kIOReturnNotReady;
            }

            if (operation == IOUserAudioIOOperationBeginRead) {
                control->client.PublishBeginRead(sampleTime, hostTime, ioBufferFrameSize);
                const bool capturePrepared =
                    PrepareCaptureRingForBeginRead(driverIvars->runtime.directAudioGraph,
                                                   *control,
                                                   sampleTime,
                                                   ioBufferFrameSize);
                control->counters.CountBeginRead();
                (void)PublishSharedZeroTimestampToHAL(*driverIvars, "io", false);
                const uint64_t beginCount =
                    control->counters.ioBeginReadCount.load(std::memory_order_relaxed);
                if (beginCount <= 8 || (beginCount % 1024) == 0) {
                    ASFW_LOG(DirectAudio,
                             "ADK DBG IO begin_read count=%llu cb=%llu frames=%u sample=%llu host=%llu capturePrepared=%d capWr=%llu capRd=%llu starve=%llu",
                             beginCount,
                             callbackIndex,
                             ioBufferFrameSize,
                             sampleTime,
                             hostTime,
                             capturePrepared,
                             control->captureRingWriteFrame.load(std::memory_order_acquire),
                             control->captureRingReadFrame.load(std::memory_order_acquire),
                             control->captureRingStarvations.load(std::memory_order_relaxed));
                }
            } else if (operation == IOUserAudioIOOperationWriteEnd) {
                control->client.PublishWriteEnd(sampleTime, hostTime, ioBufferFrameSize);
                PublishPlaybackRingWriteEnd(driverIvars->runtime.directAudioGraph, *control);
                control->counters.CountWriteEnd();
                (void)PublishSharedZeroTimestampToHAL(*driverIvars, "io", false);
                const uint64_t writeCount =
                    control->counters.ioWriteEndCount.load(std::memory_order_relaxed);
                const uint64_t writtenEnd = control->client.OutputWrittenEndFrame();
                if (writeCount <= 8 || (writeCount % 1024) == 0) {
                    ASFW_LOG(DirectAudio,
                             "ADK DBG IO write_end count=%llu cb=%llu frames=%u sample=%llu host=%llu writtenEnd=%llu playWr=%llu playRd=%llu overrun=%llu",
                             writeCount,
                             callbackIndex,
                             ioBufferFrameSize,
                             sampleTime,
                             hostTime,
                             writtenEnd,
                             control->playbackRingWriteFrame.load(std::memory_order_acquire),
                             control->playbackRingReadFrame.load(std::memory_order_acquire),
                             control->playbackRingOverruns.load(std::memory_order_relaxed));
                }
                // Resurrected periodic TX/playback snapshot (pcmNZ/pcmZero,
                // prepared/startup/retired, anchor, faults). Self-gated by
                // statistics+verbosity and a 5 s throttle; the modulo only
                // bounds capture cost on the real-time IO thread.
                if ((writeCount % 256) == 0) {
                    DirectDiagnostics::MaybeLogDirectAudioDebugSnapshot(driverIvars->runtime);
                }
            } else if (callbackIndex <= 16 || (callbackIndex % 1024) == 0) {
                ASFW_LOG(DirectAudio,
                         "ADK DBG IO op_ignored cb=%llu op=%u frames=%u sample=%llu host=%llu",
                         callbackIndex,
                         static_cast<uint32_t>(operation),
                         ioBufferFrameSize,
                         sampleTime,
                         hostTime);
            }
        } else {
            if (callbackIndex <= 16 || (callbackIndex % 1024) == 0) {
                ASFW_LOG(DirectAudio,
                         "ADK DBG IO skeleton_unbound cb=%llu op=%u frames=%u sample=%llu host=%llu",
                         callbackIndex,
                         static_cast<uint32_t>(operation),
                         ioBufferFrameSize,
                         sampleTime,
                         hostTime);
            }
            return kIOReturnNotReady;
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
