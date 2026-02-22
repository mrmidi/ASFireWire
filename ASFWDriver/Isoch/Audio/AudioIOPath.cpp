#include "AudioIOPath.hpp"

#include "../../Logging/Logging.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ASFW::Isoch::Audio {
namespace detail {

constexpr uint32_t kRxTargetFillFrames = 2048;

void MaybeDrainRxStartup(AudioIOPathState& state) {
    if (!state.rxQueueValid || !state.rxQueueReader || !state.rxStartupDrained || *state.rxStartupDrained) {
        return;
    }

    const uint32_t fill = state.rxQueueReader->FillLevelFrames();
    if (fill > kRxTargetFillFrames + 256U) {
        const uint32_t excess = fill - kRxTargetFillFrames;
        state.rxQueueReader->ConsumeFrames(excess);
    }
    *state.rxStartupDrained = true;
}

kern_return_t HandleBeginRead(AudioIOPathState& state,
                              uint32_t ioBufferFrameSize,
                              uint64_t sampleTime) {
    if (!state.inputBuffer) {
        return kIOReturnNotReady;
    }

    IOAddressSegment segment{};
    const kern_return_t status = state.inputBuffer->GetAddressRange(&segment);
    if (status != kIOReturnSuccess || segment.address == 0) {
        return kIOReturnSuccess;
    }

    const uint32_t bufferFrames = state.ioBufferPeriodFrames;
    const uint32_t offsetFrames = static_cast<uint32_t>(sampleTime % bufferFrames);
    uint32_t firstFrames = ioBufferFrameSize;
    uint32_t secondFrames = 0;
    if ((offsetFrames + ioBufferFrameSize) > bufferFrames) {
        firstFrames = bufferFrames - offsetFrames;
        secondFrames = ioBufferFrameSize - firstFrames;
    }

    const uint64_t offsetBytes = uint64_t(offsetFrames) * sizeof(int32_t) * state.channelCount;
    const size_t firstBytes = size_t(firstFrames) * sizeof(int32_t) * state.channelCount;
    const size_t secondBytes = size_t(secondFrames) * sizeof(int32_t) * state.channelCount;

    MaybeDrainRxStartup(state);

    if (state.rxQueueValid && state.rxQueueReader) {
        auto* pcmFirst = reinterpret_cast<int32_t*>(segment.address + offsetBytes);
        const uint32_t read1 = state.rxQueueReader->Read(pcmFirst, firstFrames);
        if (read1 < firstFrames) {
            memset(pcmFirst + read1 * state.channelCount,
                   0,
                   size_t(firstFrames - read1) * sizeof(int32_t) * state.channelCount);
        }

        if (secondFrames > 0) {
            auto* pcmSecond = reinterpret_cast<int32_t*>(segment.address);
            if (read1 == firstFrames) {
                const uint32_t read2 = state.rxQueueReader->Read(pcmSecond, secondFrames);
                if (read2 < secondFrames) {
                    memset(pcmSecond + read2 * state.channelCount,
                           0,
                           size_t(secondFrames - read2) * sizeof(int32_t) * state.channelCount);
                }
            } else {
                memset(pcmSecond, 0, secondBytes);
            }
        }
    } else {
        memset(reinterpret_cast<void*>(segment.address + offsetBytes), 0, firstBytes);
        if (secondFrames > 0) {
            memset(reinterpret_cast<void*>(segment.address), 0, secondBytes);
        }
    }

    return kIOReturnSuccess;
}

void RebaseZeroCopyTimeline(AudioIOPathState& state,
                            uint64_t sampleTime,
                            ZeroCopyTimelineState& timeline) {
    const uint32_t bufferFrames = (state.zeroCopyFrameCapacity > 0)
                                    ? state.zeroCopyFrameCapacity
                                    : state.ioBufferPeriodFrames;
    const uint32_t writeIdx = state.txQueueWriter->WriteIndexFrames();
    const uint32_t samplePos = static_cast<uint32_t>(sampleTime % bufferFrames);
    const uint32_t phase = (samplePos + bufferFrames - (writeIdx % bufferFrames)) % bufferFrames;

    timeline.phaseFrames = phase;
    state.txQueueWriter->ProducerSetZeroCopyPhaseFrames(phase);
    state.txQueueWriter->ProducerRequestConsumerResync();
}

uint32_t WriteEndZeroCopyPublish(AudioIOPathState& state,
                                 uint32_t ioBufferFrameSize,
                                 uint64_t sampleTime,
                                 uint32_t& outFramesRequested) {
    auto& timeline = *state.zeroCopyTimeline;
    bool rebased = false;

    if (!timeline.valid) {
        timeline.valid = true;
        timeline.lastSampleTime = sampleTime;
        timeline.publishedSampleTime = sampleTime;
        rebased = true;
    } else if (sampleTime < timeline.lastSampleTime) {
        timeline.discontinuities++;
        ASFW_LOG_RL(Audio,
                    "zc/disc",
                    500,
                    OS_LOG_TYPE_DEFAULT,
                    "ZERO-COPY DISCONTINUITY (rebase) sampleTime=%llu lastSampleTime=%llu gap=%lld disc=%llu",
                    sampleTime,
                    timeline.lastSampleTime,
                    static_cast<int64_t>(sampleTime) - static_cast<int64_t>(timeline.lastSampleTime),
                    timeline.discontinuities);
        timeline.lastSampleTime = sampleTime;
        timeline.publishedSampleTime = sampleTime;
        rebased = true;
    } else {
        timeline.lastSampleTime = sampleTime;
    }

    if (rebased) {
        RebaseZeroCopyTimeline(state, sampleTime, timeline);
    }

    uint64_t desiredPublishedSample = sampleTime + ioBufferFrameSize;
    if (desiredPublishedSample < timeline.publishedSampleTime) {
        timeline.discontinuities++;
        ASFW_LOG_RL(Audio,
                    "zc/disc",
                    500,
                    OS_LOG_TYPE_DEFAULT,
                    "ZERO-COPY DISCONTINUITY (publish) sampleTime=%llu published=%llu desired=%llu disc=%llu",
                    sampleTime,
                    timeline.publishedSampleTime,
                    desiredPublishedSample,
                    timeline.discontinuities);

        timeline.publishedSampleTime = sampleTime;
        RebaseZeroCopyTimeline(state, sampleTime, timeline);
        desiredPublishedSample = sampleTime + ioBufferFrameSize;
    }

    const uint64_t toPublish64 = desiredPublishedSample - timeline.publishedSampleTime;
    const uint32_t toPublish = (toPublish64 > 0xFFFFFFFFULL)
                                 ? 0xFFFFFFFFU
                                 : static_cast<uint32_t>(toPublish64);

    outFramesRequested = toPublish;
    const uint32_t framesWritten = state.txQueueWriter->PublishFrames(toPublish);
    timeline.publishedSampleTime += framesWritten;
    return framesWritten;
}

kern_return_t HandleWriteEnd(AudioIOPathState& state,
                             uint32_t ioBufferFrameSize,
                             uint64_t sampleTime) {
    if (!state.outputBuffer) {
        return kIOReturnNotReady;
    }

    IOAddressSegment segment{};
    const kern_return_t status = state.outputBuffer->GetAddressRange(&segment);
    if (status != kIOReturnSuccess || segment.address == 0) {
        return kIOReturnSuccess;
    }

    const uint32_t bufferFrames = state.ioBufferPeriodFrames;
    const uint32_t offsetFrames = static_cast<uint32_t>(sampleTime % bufferFrames);
    const uint64_t offsetBytes = uint64_t(offsetFrames) * sizeof(int32_t) * state.channelCount;
    uint32_t firstFrames = ioBufferFrameSize;
    uint32_t secondFrames = 0;
    if ((offsetFrames + ioBufferFrameSize) > bufferFrames) {
        firstFrames = bufferFrames - offsetFrames;
        secondFrames = ioBufferFrameSize - firstFrames;
    }

    const auto* pcmDataFirst = reinterpret_cast<const int32_t*>(segment.address + offsetBytes);
    const auto* pcmDataSecond = reinterpret_cast<const int32_t*>(segment.address);
    uint32_t framesWritten = 0;
    uint32_t framesRequested = ioBufferFrameSize;

    if (state.txQueueValid && state.txQueueWriter) {
        if (state.zeroCopyEnabled && state.zeroCopyTimeline) {
            framesWritten = WriteEndZeroCopyPublish(state,
                                                    ioBufferFrameSize,
                                                    sampleTime,
                                                    framesRequested);
        } else {
            const uint32_t firstWrite = state.txQueueWriter->Write(pcmDataFirst, firstFrames);
            framesWritten = firstWrite;
            if (firstWrite == firstFrames && secondFrames > 0) {
                framesWritten += state.txQueueWriter->Write(pcmDataSecond, secondFrames);
            }
        }
    } else if (state.packetAssembler) {
        const uint32_t firstWrite = state.packetAssembler->ringBuffer().write(pcmDataFirst, firstFrames);
        framesWritten = firstWrite;
        if (firstWrite == firstFrames && secondFrames > 0) {
            framesWritten += state.packetAssembler->ringBuffer().write(pcmDataSecond, secondFrames);
        }
    }

    if (framesWritten < framesRequested && state.encodingOverruns) {
        (*state.encodingOverruns)++;
    }

    return kIOReturnSuccess;
}

} // namespace detail

kern_return_t HandleIOOperation(AudioIOPathState& state,
                                IOUserAudioIOOperation operation,
                                uint32_t ioBufferFrameSize,
                                uint64_t sampleTime) {
    switch (operation) {
        case IOUserAudioIOOperationBeginRead:
            return detail::HandleBeginRead(state, ioBufferFrameSize, sampleTime);
        case IOUserAudioIOOperationWriteEnd:
            return detail::HandleWriteEnd(state, ioBufferFrameSize, sampleTime);
        default:
            return kIOReturnSuccess;
    }
}

} // namespace ASFW::Isoch::Audio
