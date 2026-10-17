//
// ASFWAudioDriverOutputWrite.cpp
// ASFWDriver
//
// The CoreAudio output side of TX:
//
// - WriteEnd publishes the client's write end W and the playback-ring range,
//   fills, and wakes the TX producer. The HAL output ring CoreAudio just
//   wrote is the PCM publication.
// - FillTransmitPayloads, run inside WriteEnd on the IO thread (its only
//   owner), copies each written frame once from that ring into the armed
//   packet that carries it, if the packet is still ahead of the hardware.
//   RT-safe: no locks, no allocation; the timeline and the clock pair are
//   read through their seqlocks.
//
// Both are called unchanged from the host TX tests.
//

#include "ASFWAudioDriverPrivate.hpp"
#include "../Runtime/PlaybackRingRange.hpp"
#include "../../Common/TimingUtils.hpp"
#ifdef ASFW_HOST_TEST
#include "../../Testing/HostDriverKitStubs.hpp"
#endif

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
    if (ivars.device.vendorId == 0x00130eU &&
        ivars.runtime.txStartupClientTrace.pending.load(std::memory_order_relaxed)) {
        ASFW::Audio::Runtime::HostClockAnchorSample anchor{};
        (void)control.hostClockAnchor.TryReadLatest(0, anchor);
        ivars.runtime.txStartupClientTrace.Publish({
            .sampleFrame = sampleTime,
            .hostTicks = hostTime,
            .filledBefore = ivars.runtime.txFilledFrameEnd,
            .exposedFrameEnd = ivars.runtime.txStreamEngine.Timeline().ExposedFrameEnd(),
            .writtenFrameEnd = control.playbackRingWriteFrame.load(std::memory_order_acquire),
            .anchorFrame = anchor.sampleFrame,
            .anchorEpoch = anchor.timelineEpoch,
            .frames = ioBufferFrameSize,
            .capacity = ivars.runtime.directAudioGraph.memory.outputFrameCapacity,
        });
    }

    // Fill here, on CoreAudio's IO thread, not on the TX preparation queue: a
    // frame has ~1 ms between this write and its packet's deadline, and queue
    // wakes were measured up to 13.7 ms late on hardware, leaving packets
    // silent. This thread is the fill's only owner.
    FillTransmitPayloads(ivars);

    // Wake the producer so packet arming keeps pace with CoreAudio. The
    // coalescing latch keeps this RT callback to at most one outstanding
    // action.
    const uint64_t requestGeneration =
        control.txPreparationRequests.PublishRequest(hostTime);
    if (ivars.device.audioNub && ivars.txPreparationAction &&
        control.txPreparationRequests.TryScheduleWake()) {
        // OSAction-targeted, one-way dispatch goes directly to TxPreparation.
        // Never wait behind a device-control RPC on the nub's Default queue.
        ivars.device.audioNub->TxPreparationReady(ivars.txPreparationAction.get(), requestGeneration);
    }

    control.counters.CountWriteEnd();
    return true;
}

namespace {

[[nodiscard]] uint64_t TxHostNow() noexcept {
#ifdef ASFW_HOST_TEST
    return ASFW::Testing::HostMonotonicNow();
#else
    return mach_absolute_time();
#endif
}

// The packet the hardware is sending now, projected from the last refill:
// completionCursor was recorded with the clock pair of that refill, and IT
// sends one packet per bus cycle, so the packets since then are the elapsed
// cycles. Rounded up, so the fill errs toward treating a packet as taken.
[[nodiscard]] uint64_t ProjectHardwarePacket(const ASFW::Isoch::IsochTxQueueControl& queue,
                                             uint64_t nowHost) noexcept {
    const uint64_t completion = queue.completionCursor.load(std::memory_order_acquire);
    ASFW::Isoch::IsochTxClockPairSample pair{};
    if (!queue.clockPair.TryRead(pair) || pair.hostTimeMid == 0 || nowHost < pair.hostTimeMid) {
        // No usable correlation: assume a whole interrupt group has gone out.
        return completion + ASFW::IsochTransport::AudioTimingGeometry::kTxPacketsPerGroup;
    }
    const uint64_t elapsedNanos = ASFW::Timing::hostTicksToNanos(nowHost - pair.hostTimeMid);
    constexpr uint64_t kCycleNanos = 125'000;
    return completion + (elapsedNanos + kCycleNanos - 1) / kCycleNanos;
}

} // namespace

uint64_t TxFirstWritablePacket(const ASFW::Isoch::IsochTxQueueControl& queue) noexcept {
    return ProjectHardwarePacket(queue, TxHostNow()) +
           ASFW::IsochTransport::AudioTimingGeometry::kTxFillFinalityGuardPackets;
}

void FillTransmitPayloads(ASFWAudioDriver_IVars& ivars) noexcept {
    auto* control = ivars.runtime.directAudioGraph.control;
    const auto* queue = ivars.runtime.txSlotProvider.queueControl;
    const auto& memory = ivars.runtime.directAudioGraph.memory;
    if (control == nullptr || queue == nullptr || memory.outputBase == nullptr ||
        memory.outputChannels == 0 || memory.outputFrameCapacity == 0) {
        return;
    }

    // What CoreAudio has written and the ring still holds.
    const uint64_t writtenEnd = control->playbackRingWriteFrame.load(std::memory_order_acquire);
    const uint64_t oldestValid =
        control->playbackRingOldestValidFrame.load(std::memory_order_acquire);
    uint64_t& filled = ivars.runtime.txFilledFrameEnd;
    if (filled == 0) {
        // Frames before CoreAudio's first write were never written: there is
        // nothing to fill, and they must not count as missed deadlines.
        filled = control->client.outputWriteEndSampleFrame.load(std::memory_order_acquire);
    }
    if (filled < oldestValid) {
        filled = oldestValid;  // frames the ring no longer holds stay silent
    }
    // Only frames whose packets exist yet; the rest wait for a later pass.
    const uint64_t exposedEnd = ivars.runtime.txStreamEngine.Timeline().ExposedFrameEnd();
    const uint64_t end = writtenEnd < exposedEnd ? writtenEnd : exposedEnd;
    if (end <= filled) {
        return;
    }

    const uint64_t firstWritable = TxFirstWritablePacket(*queue);

    const uint64_t frames = end - filled;
    ASFW::Protocols::Audio::AMDTP::HostAudioBufferView hostBuffer{};
    hostBuffer.interleavedFloat32 = memory.outputBase;
    hostBuffer.firstFrame = filled;
    hostBuffer.frameCount = frames > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(frames);
    hostBuffer.frameCapacity = memory.outputFrameCapacity;
    hostBuffer.channels = memory.outputChannels;

    const uint64_t fillStarted = TxHostNow();
    ivars.runtime.txStreamEngine.FillFromHostOutput(hostBuffer, firstWritable);
    // Match the completion sampling schedule. Copy evidence on the IO writer
    // after filling, while these frames are still ahead of finality. No logging.
    if (ivars.device.vendorId == 0x00130eU &&
        firstWritable < 61440 && memory.outputChannels >= 2) {
        auto& trace = ivars.runtime.txStartupFillTrace;
        uint64_t frame = hostBuffer.firstFrame;
        const uint64_t endFrame = frame + hostBuffer.frameCount;
        while (frame < endFrame) {
            ASFW::Protocols::Audio::AMDTP::PacketSlotSnapshot snap{};
            if (!ivars.runtime.txStreamEngine.Timeline().SnapshotSlotForAudioFrame(frame, snap)) break;
            const uint64_t next = snap.firstAudioFrame + snap.framesInPacket;
            if (next <= frame) break;
            if (frame == snap.firstAudioFrame && snap.packetIndex >= firstWritable &&
                ASFW::Isoch::CaptureTxDiagnosticPacket(snap.packetIndex) && snap.packetSizeBytes >= 16) {
                ASFW::Audio::Runtime::TxStartupFillSample sample{};
                sample.packet = snap.packetIndex;
                sample.frame = frame;
                sample.firstWritable = firstWritable;
                sample.payloadOffset = reinterpret_cast<uintptr_t>(snap.packetBytes) -
                    reinterpret_cast<uintptr_t>(ivars.runtime.txSlotProvider.payloadBase);
                const float* source = memory.outputBase +
                    (frame % memory.outputFrameCapacity) * memory.outputChannels;
                for (uint32_t ch = 0; ch < 2; ++ch) {
                    sample.sourceBits[ch] = std::bit_cast<uint32_t>(source[ch]);
                    const auto* bytes = snap.packetBytes + 8 + ch * 4;
                    sample.encodedWords[ch] = (uint32_t{bytes[0]} << 24) |
                        (uint32_t{bytes[1]} << 16) | (uint32_t{bytes[2]} << 8) | bytes[3];
                }
                if (snap.slot->generation.load(std::memory_order_acquire) == snap.generation &&
                    snap.slot->state.load(std::memory_order_acquire) ==
                        ASFW::Protocols::Audio::AMDTP::PacketSlotState::ExposedForAudio) {
                    trace.Publish(sample);
                }
            }
            frame = next;
        }
    }
    // The secondary stream carries host channels [16, 32) of the same frames
    // (its writer's sourceChannelOffset), so both streams take one decision.
    if (ivars.runtime.txSecondaryActive) {
        ivars.runtime.txStreamEngineSecondary.FillFromHostOutput(hostBuffer, firstWritable);
    }

    const uint64_t fillDuration = TxHostNow() - fillStarted;
    auto& maxDuration = ivars.runtime.txFillMaxDurationTicks;
    uint64_t observed = maxDuration.load(std::memory_order_relaxed);
    while (fillDuration > observed &&
           !maxDuration.compare_exchange_weak(observed, fillDuration,
                                             std::memory_order_relaxed)) {}
    filled += hostBuffer.frameCount;
    control->playbackRingReadFrame.store(filled, std::memory_order_release);
}

} // namespace ASFW::Audio::DriverKit
