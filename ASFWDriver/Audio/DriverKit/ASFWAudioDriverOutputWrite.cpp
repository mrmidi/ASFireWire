//
// ASFWAudioDriverOutputWrite.cpp
// ASFWDriver
//
// The CoreAudio WriteEnd step: publish the client's output write frontier and
// the playback-ring range, request TX preparation, and hand the written frames
// to the TX stream engine(s). Called from the IO handler
// (ASFWAudioDriverIO.cpp) and, unchanged, from the host TX tests, so the path
// milestone 6 rewrites has host coverage (documentation/TX_OWNERSHIP.md, T1).
//

#include "ASFWAudioDriverPrivate.hpp"
#include "../Runtime/PlaybackRingRange.hpp"

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

} // namespace

bool HandleOutputWriteEnd(ASFWAudioDriver_IVars& ivars,
                          ASFW::Audio::Runtime::AudioTransportControlBlock& control,
                          uint64_t sampleTime,
                          uint64_t hostTime,
                          uint32_t ioBufferFrameSize) noexcept {
    // CoreAudio may choose a larger span than kHalIoPeriodFrames while
    // remaining within the stream ring; the ring capacity is the hard bound.
    if (ioBufferFrameSize > ivars.runtime.directAudioGraph.memory.outputFrameCapacity) {
        return false;
    }
    control.client.PublishWriteEnd(sampleTime, hostTime, ioBufferFrameSize);
    PublishPlaybackRingWriteEnd(ivars.runtime.directAudioGraph, control);

    // Keep packet preparation driven by the CoreAudio write
    // frontier as well as the OHCI refill path. The target is a
    // 400-cycle content horizon (about 50 ms at 48 kHz), not a
    // request for transport to manipulate audio cursors. The
    // coalescing latch ensures this RT callback produces at most
    // one outstanding action.
    const uint64_t writeEndFrame = sampleTime + ioBufferFrameSize;
    const uint64_t targetFrameEnd =
        writeEndFrame +
        ASFW::IsochTransport::AudioTimingGeometry::
            TxDataHorizonFrames(
                ivars.runtime.txStreamEngine.StreamConfig()
                    .sampleRate);
    const uint64_t requestGeneration =
        control.txPreparationRequests.PublishRequest(
            hostTime, targetFrameEnd);
    if (ivars.device.audioNub &&
        control.txPreparationRequests.TryScheduleWake()) {
        const kern_return_t requestKr =
            ivars.device.audioNub->RequestTxPreparation(
                requestGeneration);
        if (requestKr != kIOReturnSuccess) {
            control.txPreparationRequests.FinishWake();
        }
    }

    const uint32_t channels = ivars.runtime.directAudioGraph.memory.outputChannels;
    const auto& memory =
        ivars.runtime.directAudioGraph.memory;
    if (memory.outputBase && channels > 0) {
        ASFW::Protocols::Audio::AMDTP::HostAudioBufferView hostBuffer{};
        hostBuffer.interleavedFloat32 = memory.outputBase;
        hostBuffer.firstFrame = sampleTime;
        hostBuffer.frameCount = ioBufferFrameSize;
        hostBuffer.frameCapacity = memory.outputFrameCapacity;
        hostBuffer.channels = channels;

        const uint64_t completionCursor = ivars.runtime.txSlotProvider.queueControl
            ? ivars.runtime.txSlotProvider.queueControl->completionCursor.load(std::memory_order_acquire)
            : 0;

        ivars.runtime.txStreamEngine.WriteHostOutputFloat32(
            hostBuffer,
            completionCursor);

        // Fan out the same host buffer to the secondary stream; its
        // payload writer reads channels [16, 32) via sourceChannelOffset.
        if (ivars.runtime.txSecondaryActive) {
            const uint64_t secondaryCompletion =
                ivars.runtime.txSlotProviderSecondary.queueControl
                    ? ivars.runtime.txSlotProviderSecondary.queueControl
                          ->completionCursor.load(std::memory_order_acquire)
                    : 0;
            ivars.runtime.txStreamEngineSecondary.WriteHostOutputFloat32(
                hostBuffer,
                secondaryCompletion);
        }

        control.playbackRingReadFrame.store(sampleTime + ioBufferFrameSize, std::memory_order_release);
    }

    control.counters.CountWriteEnd();
    return true;
}

} // namespace ASFW::Audio::DriverKit
