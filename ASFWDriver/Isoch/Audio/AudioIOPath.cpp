#include "AudioIOPath.hpp"

#include "../Config/AudioRxProfiles.hpp"
#include "../../Logging/Logging.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ASFW::Isoch::Audio {
namespace detail {

[[nodiscard]] size_t PCMByteCount(uint32_t frames, uint32_t channels) noexcept {
    return size_t(frames) * sizeof(int32_t) * channels;
}

void ZeroFrames(int32_t* pcm, uint32_t frames, uint32_t channels) {
    if (!pcm || frames == 0 || channels == 0) {
        return;
    }
    std::memset(pcm, 0, PCMByteCount(frames, channels));
}

[[nodiscard]] uint32_t ReadFramesOrZero(Shared::TxSharedQueueSPSC& reader,
                                        int32_t* pcm,
                                        uint32_t frames,
                                        uint32_t channels,
                                        uint64_t sampleTime,
                                        uint32_t ioBufferFrameSize,
                                        const char* spanName) {
    const uint32_t fillBefore = reader.FillLevelFrames();
    const uint32_t framesRead = reader.Read(pcm, frames);
    if (framesRead < frames) {
        ZeroFrames(pcm + (static_cast<size_t>(framesRead) * channels), frames - framesRead, channels);
        ASFW_LOG_RL(Audio,
                    "rx/consumer-underread",
                    250,
                    OS_LOG_TYPE_DEFAULT,
                    "RX QUEUE UNDERREAD span=%{public}s requested=%u read=%u missing=%u fillBefore=%u fillAfter=%u events=%llu frames=%llu q8=%u sample=%llu io=%u",
                    spanName,
                    frames,
                    framesRead,
                    frames - framesRead,
                    fillBefore,
                    reader.FillLevelFrames(),
                    reader.ConsumerUnderreadEvents(),
                    reader.ConsumerUnderreadFrames(),
                    reader.CorrHostNanosPerSampleQ8(),
                    sampleTime,
                    ioBufferFrameSize);
    }
    return framesRead;
}

void RecordWrappedSpanUnderread(Shared::TxSharedQueueSPSC& reader,
                                uint32_t frames,
                                uint64_t sampleTime,
                                uint32_t ioBufferFrameSize) {
    const uint32_t fill = reader.FillLevelFrames();
    reader.RecordConsumerReadResult(frames, fill, 0);
    ASFW_LOG_RL(Audio,
                "rx/consumer-underread",
                250,
                OS_LOG_TYPE_DEFAULT,
                "RX QUEUE UNDERREAD span=wrapped-skip requested=%u read=0 missing=%u fillBefore=%u fillAfter=%u events=%llu frames=%llu q8=%u sample=%llu io=%u",
                frames,
                frames,
                fill,
                reader.FillLevelFrames(),
                reader.ConsumerUnderreadEvents(),
                reader.ConsumerUnderreadFrames(),
                reader.CorrHostNanosPerSampleQ8(),
                sampleTime,
                ioBufferFrameSize);
}

bool MaybeDrainRxStartup(AudioIOPathState& state,
                         uint32_t ioBufferFrameSize,
                         uint64_t sampleTime) {
    if (!state.rxQueueValid || !state.rxQueueReader || !state.rxStartupDrained || *state.rxStartupDrained) {
        return true;
    }

    const auto& rxProfile = ASFW::Isoch::Config::GetActiveRxProfile();
    const uint32_t targetFillFrames = rxProfile.startupFillTargetFrames;
    const uint32_t largeBacklogThreshold = targetFillFrames + rxProfile.startupDrainThresholdFrames;
    const uint32_t fill = state.rxQueueReader->FillLevelFrames();
    uint32_t drained = 0;

    if (fill < targetFillFrames) {
        state.rxQueueReader->RecordConsumerReadResult(ioBufferFrameSize, fill, 0);
        ASFW_LOG_RL(Audio,
                    "rx/startup-hold",
                    500,
                    OS_LOG_TYPE_DEFAULT,
                    "RX startup hold profile=%{public}s fill=%u target=%u zeroFrames=%u events=%llu frames=%llu sample=%llu io=%u",
                    rxProfile.name,
                    fill,
                    targetFillFrames,
                    ioBufferFrameSize,
                    state.rxQueueReader->ConsumerUnderreadEvents(),
                    state.rxQueueReader->ConsumerUnderreadFrames(),
                    sampleTime,
                    ioBufferFrameSize);
        return false;
    }

    if (fill > targetFillFrames) {
        drained = fill - targetFillFrames;
        drained = state.rxQueueReader->ConsumeFrames(drained);
    }

    const uint32_t fillAfter = state.rxQueueReader->FillLevelFrames();
    ASFW_LOG(Audio,
             "RX startup rebase profile=%{public}s fill=%u target=%u drained=%u result=%u%{public}s sample=%llu io=%u",
             rxProfile.name,
             fill,
             targetFillFrames,
             drained,
             fillAfter,
             fill > largeBacklogThreshold ? " large-backlog" : "",
             sampleTime,
             ioBufferFrameSize);
    *state.rxStartupDrained = true;
    return true;
}

void MaybeLatchTransportStartupAlignment(AudioIOPathState& state, uint64_t sampleTime) {
    if (!state.rxQueueValid || !state.rxQueueReader) {
        return;
    }

    const auto existingAlignment = state.rxQueueReader->ReadStartupAlignment();
    if (existingAlignment.valid) {
        return;
    }

    const auto transportTiming = state.rxQueueReader->ReadTransportTiming();
    if (!transportTiming.valid || transportTiming.anchorHostTicks == 0) {
        return;
    }

    const uint64_t offset = (transportTiming.anchorSampleFrame >= sampleTime)
                          ? (transportTiming.anchorSampleFrame - sampleTime)
                          : 0;
    state.rxQueueReader->SetStartupAlignment(sampleTime, offset, offset);

    ASFW_LOG(Audio,
             "RX startup timing aligned sample=%llu transportSample=%llu transportHost=%llu q8=%u seq=%u inOff=%llu outOff=%llu",
             sampleTime,
             transportTiming.anchorSampleFrame,
             transportTiming.anchorHostTicks,
             transportTiming.hostNanosPerSampleQ8,
             transportTiming.seq,
             offset,
             offset);
}

uint32_t RxTargetFillFrames() noexcept {
    return ASFW::Isoch::Config::GetActiveRxProfile().startupFillTargetFrames;
}

bool RxTransportRateReady(const AudioIOPathState& state) noexcept {
    return state.rxQueueValid &&
           state.rxQueueReader &&
           state.rxQueueReader->CorrHostNanosPerSampleQ8() != 0;
}

void MaybeRebaseRxOnTransportLock(AudioIOPathState& state,
                                  uint32_t ioBufferFrameSize,
                                  uint64_t sampleTime) {
    if (!RxTransportRateReady(state) || !state.rxTransportRebased || *state.rxTransportRebased) {
        return;
    }

    const uint32_t target = RxTargetFillFrames();
    const uint32_t fill = state.rxQueueReader->FillLevelFrames();
    uint32_t drained = 0;
    if (fill > target) {
        drained = state.rxQueueReader->ConsumeFrames(fill - target);
    }

    const uint32_t fillAfter = state.rxQueueReader->FillLevelFrames();
    *state.rxTransportRebased = true;
    ASFW_LOG(Audio,
             "RX transport rebase fill=%u target=%u drained=%u result=%u q8=%u sample=%llu io=%u",
             fill,
             target,
             drained,
             fillAfter,
             state.rxQueueReader->CorrHostNanosPerSampleQ8(),
             sampleTime,
             ioBufferFrameSize);
}

void MaybeSlewRxHighWater(AudioIOPathState& state,
                          uint32_t ioBufferFrameSize,
                          uint64_t sampleTime) {
    if (!state.rxQueueValid || !state.rxQueueReader) {
        return;
    }

    const auto& rxProfile = ASFW::Isoch::Config::GetActiveRxProfile();
    const uint32_t target = rxProfile.startupFillTargetFrames;
    const uint32_t guard = std::max({rxProfile.startupDrainThresholdFrames * 2U,
                                     ioBufferFrameSize * 2U,
                                     256U});
    const uint32_t highWater = target + guard;
    const uint32_t fill = state.rxQueueReader->FillLevelFrames();
    if (fill <= highWater) {
        return;
    }

    const uint32_t capacity = state.rxQueueReader->CapacityFrames();
    const bool emergency = capacity > ioBufferFrameSize && (fill + ioBufferFrameSize) >= capacity;
    const uint32_t maxSlew = std::max(1U, ioBufferFrameSize / 64U);
    const uint32_t desiredDrain = emergency
        ? (fill > highWater ? fill - highWater : ioBufferFrameSize)
        : std::min(fill - target, maxSlew);
    const uint32_t drained = state.rxQueueReader->ConsumeFrames(desiredDrain);
    ASFW_LOG_RL(Audio,
                emergency ? "rx/high-water-emergency-trim" : "rx/high-water-slew",
                500,
                emergency ? OS_LOG_TYPE_ERROR : OS_LOG_TYPE_DEFAULT,
                "RX high-water %{public}s fill=%u high=%u target=%u cap=%u drained=%u result=%u q8=%u rateReady=%u sample=%llu io=%u",
                emergency ? "emergency-trim" : "slew",
                fill,
                highWater,
                target,
                capacity,
                drained,
                state.rxQueueReader->FillLevelFrames(),
                state.rxQueueReader->CorrHostNanosPerSampleQ8(),
                RxTransportRateReady(state) ? 1U : 0U,
                sampleTime,
                ioBufferFrameSize);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
kern_return_t HandleBeginRead(AudioIOPathState& state,
                              uint32_t ioBufferFrameSize, // NOLINT(bugprone-easily-swappable-parameters)
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

    const uint32_t ch = state.inputChannelCount;
    if (ch == 0) {
        return kIOReturnBadArgument;
    }

    const uint64_t offsetBytes = uint64_t(offsetFrames) * sizeof(int32_t) * ch;
    auto* pcmFirst = reinterpret_cast<int32_t*>(segment.address + offsetBytes);
    auto* pcmSecond = reinterpret_cast<int32_t*>(segment.address);

    if (!MaybeDrainRxStartup(state, ioBufferFrameSize, sampleTime)) {
        ZeroFrames(pcmFirst, firstFrames, ch);
        ZeroFrames(pcmSecond, secondFrames, ch);
        return kIOReturnSuccess;
    }
    MaybeLatchTransportStartupAlignment(state, sampleTime);
    MaybeRebaseRxOnTransportLock(state, ioBufferFrameSize, sampleTime);
    MaybeSlewRxHighWater(state, ioBufferFrameSize, sampleTime);

    if (!state.rxQueueValid || !state.rxQueueReader) {
        ZeroFrames(pcmFirst, firstFrames, ch);
        ZeroFrames(pcmSecond, secondFrames, ch);
        return kIOReturnSuccess;
    }

    const uint32_t read1 = ReadFramesOrZero(*state.rxQueueReader,
                                            pcmFirst,
                                            firstFrames,
                                            ch,
                                            sampleTime,
                                            ioBufferFrameSize,
                                            "first");
    if (secondFrames == 0) {
        return kIOReturnSuccess;
    }

    if (read1 == firstFrames) {
        static_cast<void>(ReadFramesOrZero(*state.rxQueueReader,
                                           pcmSecond,
                                           secondFrames,
                                           ch,
                                           sampleTime,
                                           ioBufferFrameSize,
                                           "wrapped"));
    } else {
        RecordWrappedSpanUnderread(*state.rxQueueReader, secondFrames, sampleTime, ioBufferFrameSize);
        ZeroFrames(pcmSecond, secondFrames, ch);
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
    if (state.writeEndFramesRequested) {
        *state.writeEndFramesRequested = 0;
    }
    if (state.writeEndFramesWritten) {
        *state.writeEndFramesWritten = 0;
    }

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
    const uint32_t ch = state.outputChannelCount;
    if (ch == 0) {
        return kIOReturnBadArgument;
    }

    const uint64_t offsetBytes = uint64_t(offsetFrames) * sizeof(int32_t) * ch;
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
    if (state.writeEndFramesRequested) {
        *state.writeEndFramesRequested = framesRequested;
    }
    if (state.writeEndFramesWritten) {
        *state.writeEndFramesWritten = framesWritten;
    }

    return kIOReturnSuccess;
}

} // namespace detail

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
kern_return_t HandleIOOperation(AudioIOPathState& state,
                                IOUserAudioIOOperation operation, // NOLINT(bugprone-easily-swappable-parameters)
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
