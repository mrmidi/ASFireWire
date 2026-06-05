//
// ASFWAudioDriverIO.cpp
// ASFWDriver
//
// Real-time IO callback installation for ASFWAudioDriver.
//

#include "ASFWAudioDriverPrivate.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/DriverKit.h>

namespace ASFW::Audio::DriverKit {

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
                control->counters.CountBeginRead();
                (void)PublishSharedZeroTimestampToHAL(*driverIvars, "io", false);
                const uint64_t beginCount =
                    control->counters.ioBeginReadCount.load(std::memory_order_relaxed);
                if (beginCount <= 8 || (beginCount % 1024) == 0) {
                    ASFW_LOG(DirectAudio,
                             "ADK DBG IO begin_read count=%llu cb=%llu frames=%u sample=%llu host=%llu",
                             beginCount,
                             callbackIndex,
                             ioBufferFrameSize,
                             sampleTime,
                             hostTime);
                }
            } else if (operation == IOUserAudioIOOperationWriteEnd) {
                control->client.PublishWriteEnd(sampleTime, hostTime, ioBufferFrameSize);
                control->counters.CountWriteEnd();
                (void)PublishSharedZeroTimestampToHAL(*driverIvars, "io", false);
                const uint64_t writeCount =
                    control->counters.ioWriteEndCount.load(std::memory_order_relaxed);
                const uint64_t writtenEnd = control->client.OutputWrittenEndFrame();
                if (writeCount <= 8 || (writeCount % 1024) == 0) {
                    ASFW_LOG(DirectAudio,
                             "ADK DBG IO write_end count=%llu cb=%llu frames=%u sample=%llu host=%llu writtenEnd=%llu",
                             writeCount,
                             callbackIndex,
                             ioBufferFrameSize,
                             sampleTime,
                             hostTime,
                             writtenEnd);
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

    if (error != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioDriver: SetIOOperationHandler failed: 0x%x", error);
    }
    return error;
}

} // namespace ASFW::Audio::DriverKit
