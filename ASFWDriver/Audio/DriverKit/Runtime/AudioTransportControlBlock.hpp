#pragma once

#include "AudioClientCursor.hpp"
#include "AudioRtCounters.hpp"
#include "DeviceTimeline.hpp"
#include "TxSytTrace.hpp"
#include "TxCycleTrace.hpp"
#include "TxWirePayloadTelemetry.hpp"
#include "../../Runtime/HostClockAnchor.hpp"
#include "../../Runtime/HardwareSampleTimeline.hpp"
#include "../../Runtime/PcmPublicationCache.hpp"
#include "../../Wire/AMDTP/RxSequenceReplay.hpp"
#include "../../Wire/AMDTP/RxSytCadence.hpp"
#include "../../Shared/AudioTimingGeometry.hpp"

#include <atomic>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace ASFW::Audio::Runtime {

enum class FatalStreamReason : uint32_t {
    None = 0,
    RxAuthorityLost,
    InvalidGeometry,
    MirrorPumpFailed,
    TxReadAhead,
    TxSourceOverwritten,
    TxPreparationMissedDeadline,
    TxSlotInvariant,
    TxPayloadMismatch,
    TxReplayUnavailable,
    TxReplayInvalidSyt,
};

enum class TxProducerFaultStage : uint32_t {
    kNone = 0,
    kPreflight,
    kExecutionAnchor,
    kReplayBegin,
    kReplayRead,
    kReplaySytValidation,
    kSlotAcquire,
    kPacketize,
    kSlotPublish,
};

[[nodiscard]] inline const char* TxProducerFaultStageName(
    TxProducerFaultStage stage) noexcept {
    switch (stage) {
        case TxProducerFaultStage::kNone: return "none";
        case TxProducerFaultStage::kPreflight: return "preflight";
        case TxProducerFaultStage::kExecutionAnchor: return "execution-anchor";
        case TxProducerFaultStage::kReplayBegin: return "replay-begin";
        case TxProducerFaultStage::kReplayRead: return "replay-read";
        case TxProducerFaultStage::kReplaySytValidation: return "replay-syt-validation";
        case TxProducerFaultStage::kSlotAcquire: return "slot-acquire";
        case TxProducerFaultStage::kPacketize: return "packetize";
        case TxProducerFaultStage::kSlotPublish: return "slot-publish";
    }
    return "unknown";
}

enum class TxProducerFaultReason : uint32_t {
    kNone = 0,
    kInvalidTransport,
    kReplayUnavailable,
    kInvalidReplaySyt,
    kSlotUnavailable,
    kPacketizerRejected,
    kSlotPublishFailed,
};

enum class TxContentFaultReason : uint32_t {
    kNone = 0,
    kNotYetPublishedAtDeadline,
    kExpired,
    kConcurrentRewriteAtDeadline,
    kInvalidSource,
    kSecondaryStreamFailure,
};

[[nodiscard]] inline const char* TxContentFaultReasonName(
    TxContentFaultReason reason) noexcept {
    switch (reason) {
        case TxContentFaultReason::kNone: return "none";
        case TxContentFaultReason::kNotYetPublishedAtDeadline:
            return "not-yet-published-at-deadline";
        case TxContentFaultReason::kExpired: return "expired";
        case TxContentFaultReason::kConcurrentRewriteAtDeadline:
            return "concurrent-rewrite-at-deadline";
        case TxContentFaultReason::kInvalidSource: return "invalid-source";
        case TxContentFaultReason::kSecondaryStreamFailure:
            return "secondary-stream-failure";
    }
    return "unknown";
}

[[nodiscard]] inline const char* TxProducerFaultReasonName(
    TxProducerFaultReason reason) noexcept {
    switch (reason) {
        case TxProducerFaultReason::kNone: return "none";
        case TxProducerFaultReason::kInvalidTransport: return "invalid-transport";
        case TxProducerFaultReason::kReplayUnavailable: return "replay-unavailable";
        case TxProducerFaultReason::kInvalidReplaySyt: return "invalid-replay-syt";
        case TxProducerFaultReason::kSlotUnavailable: return "slot-unavailable";
        case TxProducerFaultReason::kPacketizerRejected: return "packetizer-rejected";
        case TxProducerFaultReason::kSlotPublishFailed: return "slot-publish-failed";
    }
    return "unknown";
}

struct TxProducerFaultRecord final {
    uint64_t generation{0};
    TxProducerFaultStage stage{TxProducerFaultStage::kNone};
    TxProducerFaultReason reason{TxProducerFaultReason::kNone};
    uint64_t packetIndex{0};
    uint64_t rangeStart{0};
    uint64_t rangeTarget{0};
    uint32_t preparedCount{0};
    uint64_t completionCursor{0};
    uint64_t committedEnd{0};
    uint64_t replayProducerCursor{0};
    uint32_t replayEpoch{0};
};

struct TxProducerFaultSnapshot final {
    std::atomic<uint64_t> generation{0};
    std::atomic<uint32_t> stage{static_cast<uint32_t>(TxProducerFaultStage::kNone)};
    std::atomic<uint32_t> reason{static_cast<uint32_t>(TxProducerFaultReason::kNone)};
    std::atomic<uint64_t> packetIndex{0};
    std::atomic<uint64_t> rangeStart{0};
    std::atomic<uint64_t> rangeTarget{0};
    std::atomic<uint32_t> preparedCount{0};
    std::atomic<uint64_t> completionCursor{0};
    std::atomic<uint64_t> committedEnd{0};
    std::atomic<uint64_t> replayProducerCursor{0};
    std::atomic<uint32_t> replayEpoch{0};

    void Reset() noexcept {
        stage.store(static_cast<uint32_t>(TxProducerFaultStage::kNone), std::memory_order_relaxed);
        reason.store(static_cast<uint32_t>(TxProducerFaultReason::kNone), std::memory_order_relaxed);
        packetIndex.store(0, std::memory_order_relaxed);
        rangeStart.store(0, std::memory_order_relaxed);
        rangeTarget.store(0, std::memory_order_relaxed);
        preparedCount.store(0, std::memory_order_relaxed);
        completionCursor.store(0, std::memory_order_relaxed);
        committedEnd.store(0, std::memory_order_relaxed);
        replayProducerCursor.store(0, std::memory_order_relaxed);
        replayEpoch.store(0, std::memory_order_relaxed);
        generation.store(0, std::memory_order_release);
    }

    [[nodiscard]] uint64_t Publish(const TxProducerFaultRecord& record) noexcept {
        const uint64_t next = generation.load(std::memory_order_relaxed) + 1;
        stage.store(static_cast<uint32_t>(record.stage), std::memory_order_relaxed);
        reason.store(static_cast<uint32_t>(record.reason), std::memory_order_relaxed);
        packetIndex.store(record.packetIndex, std::memory_order_relaxed);
        rangeStart.store(record.rangeStart, std::memory_order_relaxed);
        rangeTarget.store(record.rangeTarget, std::memory_order_relaxed);
        preparedCount.store(record.preparedCount, std::memory_order_relaxed);
        completionCursor.store(record.completionCursor, std::memory_order_relaxed);
        committedEnd.store(record.committedEnd, std::memory_order_relaxed);
        replayProducerCursor.store(record.replayProducerCursor, std::memory_order_relaxed);
        replayEpoch.store(record.replayEpoch, std::memory_order_relaxed);
        generation.store(next, std::memory_order_release);
        return next;
    }

    [[nodiscard]] bool TryRead(TxProducerFaultRecord& out) const noexcept {
        for (uint32_t attempt = 0; attempt < 4; ++attempt) {
            const uint64_t before = generation.load(std::memory_order_acquire);
            if (before == 0) return false;
            TxProducerFaultRecord record{};
            record.generation = before;
            record.stage = static_cast<TxProducerFaultStage>(stage.load(std::memory_order_relaxed));
            record.reason = static_cast<TxProducerFaultReason>(reason.load(std::memory_order_relaxed));
            record.packetIndex = packetIndex.load(std::memory_order_relaxed);
            record.rangeStart = rangeStart.load(std::memory_order_relaxed);
            record.rangeTarget = rangeTarget.load(std::memory_order_relaxed);
            record.preparedCount = preparedCount.load(std::memory_order_relaxed);
            record.completionCursor = completionCursor.load(std::memory_order_relaxed);
            record.committedEnd = committedEnd.load(std::memory_order_relaxed);
            record.replayProducerCursor = replayProducerCursor.load(std::memory_order_relaxed);
            record.replayEpoch = replayEpoch.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (generation.load(std::memory_order_relaxed) == before) {
                out = record;
                return true;
            }
        }
        return false;
    }
};

// RX capture-ring telemetry is intentionally about frame ownership, not
// payload amplitude. Both the direct RX writer and the CoreAudio reader sample
// the same absolute frame frontiers, so the watermarks identify the buffer
// slack that can be reduced when tuning round-trip latency.
struct RxCaptureBufferTelemetry final {
    using Geometry = ASFW::Audio::Shared::AudioTimingGeometry;

    std::atomic<uint64_t> currentAvailableFrames{0};
    std::atomic<uint64_t> intervalMinimumAvailableFrames{UINT64_MAX};
    std::atomic<uint64_t> intervalMaximumAvailableFrames{0};
    std::atomic<uint64_t> intervalMinimumFreeHeadroomFrames{UINT64_MAX};
    std::array<std::atomic<uint64_t>,
               Geometry::kRxCaptureOccupancyHistogramBuckets>
        intervalOccupancyHistogram{};
    std::atomic<uint64_t> intervalOverrunEvents{0};
    std::atomic<uint64_t> intervalOverwrittenFrames{0};
    std::atomic<uint64_t> intervalStarvationEvents{0};
    std::atomic<uint64_t> intervalStarvedFrames{0};
    // A BeginRead is the only authoritative indication that CoreAudio is
    // consuming capture frames. A full ring without one is the normal
    // latest-window mailbox state, not an input overrun.
    std::atomic<uint64_t> intervalReaderBeginReadCalls{0};

    // Written by the heartbeat owner. Odd while copying, even when readers
    // may snapshot the completed interval without locking the audio path.
    std::atomic<uint64_t> completedIntervalSequence{0};
    std::atomic<uint64_t> completedMinimumAvailableFrames{UINT64_MAX};
    std::atomic<uint64_t> completedMaximumAvailableFrames{0};
    std::atomic<uint64_t> completedMinimumFreeHeadroomFrames{UINT64_MAX};
    std::array<std::atomic<uint64_t>,
               Geometry::kRxCaptureOccupancyHistogramBuckets>
        completedOccupancyHistogram{};
    std::atomic<uint64_t> completedOverrunEvents{0};
    std::atomic<uint64_t> completedOverwrittenFrames{0};
    std::atomic<uint64_t> completedStarvationEvents{0};
    std::atomic<uint64_t> completedStarvedFrames{0};
    std::atomic<uint64_t> completedReaderBeginReadCalls{0};
    std::atomic<uint64_t> totalOverwrittenFrames{0};
    std::atomic<uint64_t> totalStarvedFrames{0};

    static void UpdateMinimum(std::atomic<uint64_t>& target,
                              uint64_t candidate) noexcept {
        uint64_t previous = target.load(std::memory_order_relaxed);
        while (candidate < previous &&
               !target.compare_exchange_weak(
                   previous, candidate, std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
    }

    static void UpdateMaximum(std::atomic<uint64_t>& target,
                              uint64_t candidate) noexcept {
        uint64_t previous = target.load(std::memory_order_relaxed);
        while (candidate > previous &&
               !target.compare_exchange_weak(
                   previous, candidate, std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
    }

    void Observe(uint64_t writeFrame, uint64_t readFrame,
                 uint32_t capacityFrames) noexcept {
        if (capacityFrames == 0) {
            return;
        }
        const uint64_t available = writeFrame >= readFrame
            ? std::min(writeFrame - readFrame,
                       static_cast<uint64_t>(capacityFrames))
            : 0;
        const uint64_t freeHeadroom = capacityFrames - available;
        currentAvailableFrames.store(available, std::memory_order_relaxed);
        UpdateMinimum(intervalMinimumAvailableFrames, available);
        UpdateMaximum(intervalMaximumAvailableFrames, available);
        UpdateMinimum(intervalMinimumFreeHeadroomFrames, freeHeadroom);
        const uint32_t bucket = static_cast<uint32_t>(
            std::min<uint64_t>(
                (available * Geometry::kRxCaptureOccupancyHistogramBuckets) /
                    capacityFrames,
                Geometry::kRxCaptureOccupancyHistogramBuckets - 1));
        intervalOccupancyHistogram[bucket].fetch_add(
            1, std::memory_order_relaxed);
    }

    void RecordOverrun(uint64_t overwrittenFrames) noexcept {
        intervalOverrunEvents.fetch_add(1, std::memory_order_relaxed);
        intervalOverwrittenFrames.fetch_add(
            overwrittenFrames, std::memory_order_relaxed);
        totalOverwrittenFrames.fetch_add(
            overwrittenFrames, std::memory_order_relaxed);
    }

    void RecordStarvation(uint64_t starvedFrames) noexcept {
        intervalStarvationEvents.fetch_add(1, std::memory_order_relaxed);
        intervalStarvedFrames.fetch_add(
            starvedFrames, std::memory_order_relaxed);
        totalStarvedFrames.fetch_add(
            starvedFrames, std::memory_order_relaxed);
    }

    void RecordReaderBeginRead() noexcept {
        intervalReaderBeginReadCalls.fetch_add(1, std::memory_order_relaxed);
    }

    void CompleteInterval() noexcept {
        completedIntervalSequence.fetch_add(1, std::memory_order_relaxed);
        completedMinimumAvailableFrames.store(
            intervalMinimumAvailableFrames.exchange(
                UINT64_MAX, std::memory_order_relaxed),
            std::memory_order_relaxed);
        completedMaximumAvailableFrames.store(
            intervalMaximumAvailableFrames.exchange(
                0, std::memory_order_relaxed),
            std::memory_order_relaxed);
        completedMinimumFreeHeadroomFrames.store(
            intervalMinimumFreeHeadroomFrames.exchange(
                UINT64_MAX, std::memory_order_relaxed),
            std::memory_order_relaxed);
        for (size_t index = 0; index < intervalOccupancyHistogram.size(); ++index) {
            completedOccupancyHistogram[index].store(
                intervalOccupancyHistogram[index].exchange(
                    0, std::memory_order_relaxed),
                std::memory_order_relaxed);
        }
        completedOverrunEvents.store(intervalOverrunEvents.exchange(
            0, std::memory_order_relaxed), std::memory_order_relaxed);
        completedOverwrittenFrames.store(intervalOverwrittenFrames.exchange(
            0, std::memory_order_relaxed), std::memory_order_relaxed);
        completedStarvationEvents.store(intervalStarvationEvents.exchange(
            0, std::memory_order_relaxed), std::memory_order_relaxed);
        completedStarvedFrames.store(intervalStarvedFrames.exchange(
            0, std::memory_order_relaxed), std::memory_order_relaxed);
        completedReaderBeginReadCalls.store(intervalReaderBeginReadCalls.exchange(
            0, std::memory_order_relaxed), std::memory_order_relaxed);
        completedIntervalSequence.fetch_add(1, std::memory_order_release);
    }

    void Reset() noexcept {
        currentAvailableFrames.store(0, std::memory_order_relaxed);
        intervalMinimumAvailableFrames.store(UINT64_MAX, std::memory_order_relaxed);
        intervalMaximumAvailableFrames.store(0, std::memory_order_relaxed);
        intervalMinimumFreeHeadroomFrames.store(UINT64_MAX, std::memory_order_relaxed);
        for (auto& bucket : intervalOccupancyHistogram) {
            bucket.store(0, std::memory_order_relaxed);
        }
        intervalOverrunEvents.store(0, std::memory_order_relaxed);
        intervalOverwrittenFrames.store(0, std::memory_order_relaxed);
        intervalStarvationEvents.store(0, std::memory_order_relaxed);
        intervalStarvedFrames.store(0, std::memory_order_relaxed);
        intervalReaderBeginReadCalls.store(0, std::memory_order_relaxed);
        completedIntervalSequence.store(0, std::memory_order_relaxed);
        completedMinimumAvailableFrames.store(UINT64_MAX, std::memory_order_relaxed);
        completedMaximumAvailableFrames.store(0, std::memory_order_relaxed);
        completedMinimumFreeHeadroomFrames.store(UINT64_MAX, std::memory_order_relaxed);
        for (auto& bucket : completedOccupancyHistogram) {
            bucket.store(0, std::memory_order_relaxed);
        }
        completedOverrunEvents.store(0, std::memory_order_relaxed);
        completedOverwrittenFrames.store(0, std::memory_order_relaxed);
        completedStarvationEvents.store(0, std::memory_order_relaxed);
        completedStarvedFrames.store(0, std::memory_order_relaxed);
        completedReaderBeginReadCalls.store(0, std::memory_order_relaxed);
        totalOverwrittenFrames.store(0, std::memory_order_relaxed);
        totalStarvedFrames.store(0, std::memory_order_relaxed);
    }
};

struct TxPreparationRequestState final {
    std::atomic<uint64_t> requestedGeneration{0};
    std::atomic<uint64_t> handledGeneration{0};
    std::atomic<uint64_t> requestHostTicks{0};
    std::atomic<uint64_t> handledHostTicks{0};
    // CoreAudio can publish every IO period while TxPreparation runs on a
    // different queue. This latch makes action delivery edge-triggered and
    // coalesces those writes into one follow-up action.
    std::atomic<bool> wakeScheduled{false};

    /// Stamp only the OLDEST unhandled request. CoreAudio can publish several
    /// IO periods before the preparation queue runs, and a last-writer-wins
    /// stamp would then measure the delay of the newest request instead of the
    /// queueing delay actually incurred -- exactly the quantity that decides
    /// how late content may arrive. Zero means "no request outstanding".
    [[nodiscard]] uint64_t PublishRequest(uint64_t hostTicks) noexcept {
        uint64_t expected = 0;
        const uint64_t stamp = hostTicks != 0 ? hostTicks : 1;
        requestHostTicks.compare_exchange_strong(
            expected, stamp, std::memory_order_relaxed,
            std::memory_order_relaxed);
        return requestedGeneration.fetch_add(1, std::memory_order_release) + 1;
    }

    [[nodiscard]] uint64_t RequestedGeneration() const noexcept {
        return requestedGeneration.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool NeedsHandling() const noexcept {
        return handledGeneration.load(std::memory_order_acquire) <
               requestedGeneration.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool TryScheduleWake() noexcept {
        return !wakeScheduled.exchange(true, std::memory_order_acq_rel);
    }

    void FinishWake() noexcept {
        wakeScheduled.store(false, std::memory_order_release);
    }

    /// Returns the host-tick stamp of the oldest request this pass served, or
    /// 0 when the wake was not request-driven. The caller turns it into a
    /// latency sample; this type owns the handshake, not the statistics.
    [[nodiscard]] uint64_t MarkHandled(uint64_t generation,
                                       uint64_t hostTicks) noexcept {
        uint64_t handled = handledGeneration.load(std::memory_order_relaxed);
        while (handled < generation &&
               !handledGeneration.compare_exchange_weak(
                   handled, generation, std::memory_order_release,
                   std::memory_order_relaxed)) {
        }
        handledHostTicks.store(hostTicks, std::memory_order_relaxed);
        return requestHostTicks.exchange(0, std::memory_order_relaxed);
    }

    void Reset() noexcept {
        requestedGeneration.store(0, std::memory_order_relaxed);
        handledGeneration.store(0, std::memory_order_relaxed);
        requestHostTicks.store(0, std::memory_order_relaxed);
        handledHostTicks.store(0, std::memory_order_relaxed);
        wakeScheduled.store(false, std::memory_order_relaxed);
    }
};

struct TxFatalSnapshot final {
    std::atomic<uint64_t> audioFrame{0};
    std::atomic<int64_t> outputPhaseTicks{-1};
    std::atomic<uint64_t> oldestValidFrame{0};
    std::atomic<uint64_t> writtenEndFrame{0};
    std::atomic<uint32_t> packetIndex{0};
    std::atomic<uint32_t> distanceToHardware{0};
    std::atomic<uint32_t> slotState{0};
    std::atomic<uint32_t> dbc{0};
    std::atomic<uint32_t> syt{0};
    std::atomic<uint64_t> preparedPayloadHash{0};
    std::atomic<uint64_t> completedPayloadHash{0};

    void Reset() noexcept {
        audioFrame.store(0, std::memory_order_relaxed);
        outputPhaseTicks.store(-1, std::memory_order_relaxed);
        oldestValidFrame.store(0, std::memory_order_relaxed);
        writtenEndFrame.store(0, std::memory_order_relaxed);
        packetIndex.store(0, std::memory_order_relaxed);
        distanceToHardware.store(0, std::memory_order_relaxed);
        slotState.store(0, std::memory_order_relaxed);
        dbc.store(0, std::memory_order_relaxed);
        syt.store(0, std::memory_order_relaxed);
        preparedPayloadHash.store(0, std::memory_order_relaxed);
        completedPayloadHash.store(0, std::memory_order_relaxed);
    }
};

struct AudioTransportControlBlock final {
    std::atomic<uint64_t> generation{0};

    AudioClientCursor client{};
    DeviceTimeline device{};
    AudioRtCounters counters{};
    HostClockAnchorState hostClockAnchor{};
    std::atomic<uint64_t> discontinuities{0};

    // Latest ADK IO callback, successful or not. The real-time callback only
    // stores atomics; the watchdog formats the state off the RT thread.
    std::atomic<uint64_t> ioCallbackGeneration{0};
    std::atomic<uint32_t> ioLastOperation{0};
    std::atomic<uint32_t> ioLastFrameCount{0};
    std::atomic<uint32_t> ioLastObjectId{0};
    std::atomic<uint64_t> ioLastSampleTime{0};
    std::atomic<uint64_t> ioLastHostTime{0};

    // Real-time IO callback failures are captured atomically and formatted by
    // the watchdog. Do not call os_log from the ADK real-time callback.
    std::atomic<uint64_t> ioCallbackErrorGeneration{0};
    std::atomic<uint64_t> ioCallbackErrorReportedGeneration{0};
    std::atomic<uint32_t> ioLastError{0};
    std::atomic<uint32_t> ioLastErrorOperation{0};
    std::atomic<uint32_t> ioLastErrorFrameCount{0};
    std::atomic<uint32_t> ioLastErrorObjectId{0};
    std::atomic<uint64_t> ioLastErrorSampleTime{0};
    std::atomic<uint64_t> ioLastErrorHostTime{0};

    std::atomic<FatalStreamReason> fatalReason{FatalStreamReason::None};
    std::atomic<uint64_t> fatalGeneration{0};

    // TX control block members
    TxWirePayloadTelemetry txWirePayloadTelemetry{};
    HardwareSampleTimeline hardwareTimeline{};
    // Zero means no request; otherwise stores discontinuity enum + 1 so
    // StartIO (enum value zero) remains representable. The RX service requests
    // and the serialized TX preparation queue consumes the transition.
    std::atomic<uint32_t> timelineEpochRequest{0};
    PcmPublicationTelemetry pcmPublicationTelemetry{};

    // Latest-value trace of the live replay TX SYT decision (diagnostics).
    TxSytTraceLatest txSytTrace{};
    TxPreparationRequestState txPreparationRequests{};
    TxFatalSnapshot txFatalSnapshot{};
    TxProducerFaultSnapshot txProducerFault{};
    TxCycleTraceRing txCycleTrace{};

    std::atomic<uint64_t> outputConsumedEndFrame{0};
    std::atomic<uint64_t> outputUnderruns{0};

    std::atomic<uint64_t> playbackRingWriteFrame{0};
    std::atomic<uint64_t> playbackRingReadFrame{0};
    std::atomic<uint64_t> playbackRingOldestValidFrame{0};
    std::atomic<uint64_t> playbackRingDiscontinuityGeneration{0};
    std::atomic<uint64_t> playbackRingUnderruns{0};
    std::atomic<uint64_t> playbackRingOverruns{0};
    std::atomic<uint64_t> txScheduledSampleFrame{0};
    std::atomic<uint64_t> txCompletedSampleFrame{0};
    std::atomic<uint64_t> txContentDeferrals{0};
    std::atomic<uint64_t> txContentDeadlineNoData{0};
    std::atomic<uint64_t> txMissedFrames{0};
    // Value-owned mirrors of the neutral transport queue. The preparation
    // queue refreshes these while the direct binding is alive so diagnostics
    // never need to retain or dereference the transport-owned queue mapping.
    std::atomic<uint64_t> txTransportCompletionCursor{0};
    std::atomic<uint64_t> txTransportCommittedEnd{0};
    std::atomic<uint32_t> txTransportStatus{0};
    // First content failure is latched for post-mortem attribution. A separate
    // counter records later events without erasing the causal snapshot.
    std::atomic<uint64_t> txContentFaultEvents{0};
    std::atomic<uint32_t> txContentFirstFaultReason{
        static_cast<uint32_t>(TxContentFaultReason::kNone)};
    std::atomic<uint64_t> txContentFirstFaultPacket{0};
    std::atomic<uint64_t> txContentFirstFaultAudioFrame{0};
    std::atomic<uint64_t> txContentFirstFaultOldestFrame{0};
    std::atomic<uint64_t> txContentFirstFaultWrittenEndFrame{0};
    std::atomic<uint64_t> txContentFirstFaultCompletionCursor{0};
    std::atomic<uint64_t> txContentFirstFaultCommittedEnd{0};
    std::atomic<uint32_t> txCurrentCommittedMarginPackets{0};
    std::atomic<uint32_t> txMinimumPreparationDistance{UINT32_MAX};
    std::atomic<uint32_t> txMinimumCommittedMarginPackets{UINT32_MAX};
    std::atomic<uint64_t> txLastPreparationLatencyTicks{0};
    std::atomic<uint64_t> txMaxPreparationLatencyTicks{0};
    std::atomic<uint64_t> txPreparationLatencySamples{0};
    std::atomic<uint64_t> txPreparationAtMost750Us{0};
    std::atomic<uint64_t> txPreparationAtLeast1500Us{0};
    // Snapshot-and-reset [TxPrep] telemetry.  These characterize the interval
    // since the previous emitted line; the adjacent fields remain since-start
    // watermarks/counters for long-run fault evidence.
    std::atomic<uint32_t> txIntervalCommittedMarginMinPackets{UINT32_MAX};
    std::atomic<uint32_t> txIntervalCommittedMarginMaxPackets{0};
    /// Producer runway: frames the CoreAudio writer has staged beyond the next
    /// frame packetization will consume, sampled after each prepared packet.
    ///
    /// The committed-margin fields above measure *our* headroom against *our*
    /// transmit deadline; they have no term for how far ahead the writer is.
    /// That is why a deadline xrun can occur while margin reads healthy: the
    /// two are independent, and only this one can go to zero when the host
    /// callback is late. A `kNotYetPublishedAtDeadline` fault is the matching
    /// immutable-cache outcome.
    std::atomic<uint32_t> txIntervalProducerHeadroomMinFrames{UINT32_MAX};
    std::atomic<uint32_t> txMinimumProducerHeadroomFrames{UINT32_MAX};
    std::atomic<uint64_t> txIntervalPreparationLatencyMaxTicks{0};
    std::array<std::atomic<uint64_t>,
               ASFW::Audio::Shared::AudioTimingGeometry::
                   kTxPreparationLatencyHistogramBuckets>
        txIntervalPreparationLatencyHistogram{};
    std::array<std::atomic<uint64_t>,
               ASFW::Audio::Shared::AudioTimingGeometry::
                   kTxCommittedMarginHistogramBuckets>
        txIntervalCommittedMarginHistogram{};
    // Last complete interval, copied by the control plane.  The audio thread
    // writes it under txCompletedIntervalSequence (odd while mutating, even
    // when stable); readers only ever receive a value-owned snapshot.
    std::atomic<uint64_t> txCompletedIntervalSequence{0};
    std::atomic<uint32_t> txCompletedIntervalMarginMinPackets{UINT32_MAX};
    std::atomic<uint32_t> txCompletedIntervalMarginMaxPackets{0};
    std::atomic<uint32_t> txCompletedIntervalProducerHeadroomMinFrames{
        UINT32_MAX};
    std::atomic<uint64_t> txCompletedIntervalPreparationLatencyMaxTicks{0};
    std::array<std::atomic<uint64_t>,
               ASFW::Audio::Shared::AudioTimingGeometry::
                   kTxPreparationLatencyHistogramBuckets>
        txCompletedIntervalPreparationLatencyHistogram{};
    std::array<std::atomic<uint64_t>,
               ASFW::Audio::Shared::AudioTimingGeometry::
                   kTxCommittedMarginHistogramBuckets>
        txCompletedIntervalCommittedMarginHistogram{};
    /// Host-tick stamp of the last emitted [TxPrep] line. The heartbeat is
    /// wall-clock paced rather than wake-count paced so its rate does not scale
    /// with sample rate (a %N-of-wakes trigger fires 2-4x faster at 96/192 kHz,
    /// flooding the log ring exactly when retention matters most).
    std::atomic<uint64_t> txHeartbeatLastHostTicks{0};
    std::atomic<int64_t> txLastLeadTicks{0};
    std::atomic<int64_t> txMinimumLeadTicks{INT64_MAX};
    std::atomic<int64_t> txMaximumLeadTicks{INT64_MIN};
    std::atomic<uint64_t> txPacketStoreHighWaterPackets{0};
    std::atomic<uint64_t> txCompletionLatencyMaxCycles{0};
    std::array<std::atomic<uint64_t>,
               ASFW::Audio::Shared::AudioTimingGeometry::
                   kTxDeadlineHeadroomHistogramBuckets>
        txDeadlineHeadroomHistogram{};
    std::array<std::atomic<uint64_t>,
               ASFW::Audio::Shared::AudioTimingGeometry::
                   kTxCompletionLatencyHistogramBuckets>
        txCompletionLatencyHistogram{};
    std::atomic<uint64_t> backendDbcDiscontinuities{0};
    std::atomic<uint64_t> backendSytDiscontinuities{0};
    std::atomic<uint64_t> backendObservationConversions{0};
    std::atomic<uint64_t> backendObservationConversionFailures{0};
    std::atomic<uint64_t> mAudioWarmupGroups{0};
    std::atomic<uint64_t> mAudioTxDerivedObservations{0};
    std::atomic<uint64_t> mAudioCaptureTransitions{0};
    std::atomic<uint64_t> mAudioPostStartConfirmations{0};

    // -------------------------------------------------------------------
    // [TxPrep] interval accumulators.
    //
    // These fields, their completed-interval mirrors, and the whole export
    // path already existed; the V3 preparation rewrite dropped the writers, so
    // every reader has been showing zeros. Restoring them is a prerequisite
    // for deriving output safety from measured scheduling behaviour instead of
    // from the plan horizon (see RTL.md, "Instrumentation required before
    // tuning"). Nothing here changes TX behaviour.
    // -------------------------------------------------------------------

    /// One preparation wake that served a pending CoreAudio request.
    /// `latencyMicros` is the caller's timebase conversion of `latencyTicks`;
    /// this header stays free of mach_timebase so it remains host-testable.
    void RecordPreparationLatency(uint64_t latencyTicks,
                                  uint64_t latencyMicros) noexcept {
        using Geometry = ASFW::Audio::Shared::AudioTimingGeometry;
        txLastPreparationLatencyTicks.store(latencyTicks,
                                            std::memory_order_relaxed);
        RaiseMaximum64(txMaxPreparationLatencyTicks, latencyTicks);
        RaiseMaximum64(txIntervalPreparationLatencyMaxTicks, latencyTicks);
        txPreparationLatencySamples.fetch_add(1, std::memory_order_relaxed);
        if (latencyMicros <= Geometry::kTxPreparationLatency750Us) {
            txPreparationAtMost750Us.fetch_add(1, std::memory_order_relaxed);
        }
        if (latencyMicros >= Geometry::kTxPreparationLatency1500Us) {
            txPreparationAtLeast1500Us.fetch_add(1, std::memory_order_relaxed);
        }
        txIntervalPreparationLatencyHistogram[
            PreparationLatencyBucket(latencyMicros)]
            .fetch_add(1, std::memory_order_relaxed);
    }

    /// Committed packets ahead of the transport completion cursor, sampled
    /// once per preparation pass.
    void RecordCommittedMargin(uint64_t marginPackets) noexcept {
        const uint32_t margin = marginPackets > UINT32_MAX
            ? UINT32_MAX : static_cast<uint32_t>(marginPackets);
        LowerMinimum32(txIntervalCommittedMarginMinPackets, margin);
        RaiseMaximum32(txIntervalCommittedMarginMaxPackets, margin);
        LowerMinimum32(txMinimumCommittedMarginPackets, margin);
        txIntervalCommittedMarginHistogram[CommittedMarginBucket(margin)]
            .fetch_add(1, std::memory_order_relaxed);
    }

    /// Frames the CoreAudio writer has staged beyond the frame this packet
    /// consumes. This is the only field that can go to zero when the host
    /// callback is late; committed margin measures our headroom against our
    /// own deadline and has no term for the writer at all.
    void RecordProducerHeadroom(uint64_t headroomFrames) noexcept {
        const uint32_t headroom = headroomFrames > UINT32_MAX
            ? UINT32_MAX : static_cast<uint32_t>(headroomFrames);
        LowerMinimum32(txIntervalProducerHeadroomMinFrames, headroom);
        LowerMinimum32(txMinimumProducerHeadroomFrames, headroom);
    }

    /// Publish the interval just closed and re-arm the accumulators. Readers
    /// only ever observe a value-owned snapshot: the sequence is odd while the
    /// completed fields are being replaced, even when they are stable.
    void CompleteTxInterval() noexcept {
        const uint64_t sequence =
            txCompletedIntervalSequence.load(std::memory_order_relaxed);
        txCompletedIntervalSequence.store(sequence + 1,
                                          std::memory_order_release);

        txCompletedIntervalMarginMinPackets.store(
            txIntervalCommittedMarginMinPackets.exchange(
                UINT32_MAX, std::memory_order_relaxed),
            std::memory_order_relaxed);
        txCompletedIntervalMarginMaxPackets.store(
            txIntervalCommittedMarginMaxPackets.exchange(
                0, std::memory_order_relaxed),
            std::memory_order_relaxed);
        txCompletedIntervalPreparationLatencyMaxTicks.store(
            txIntervalPreparationLatencyMaxTicks.exchange(
                0, std::memory_order_relaxed),
            std::memory_order_relaxed);
        for (size_t index = 0;
             index < txIntervalPreparationLatencyHistogram.size(); ++index) {
            txCompletedIntervalPreparationLatencyHistogram[index].store(
                txIntervalPreparationLatencyHistogram[index].exchange(
                    0, std::memory_order_relaxed),
                std::memory_order_relaxed);
        }
        for (size_t index = 0;
             index < txIntervalCommittedMarginHistogram.size(); ++index) {
            txCompletedIntervalCommittedMarginHistogram[index].store(
                txIntervalCommittedMarginHistogram[index].exchange(
                    0, std::memory_order_relaxed),
                std::memory_order_relaxed);
        }
        txCompletedIntervalProducerHeadroomMinFrames.store(
            txIntervalProducerHeadroomMinFrames.exchange(
                UINT32_MAX, std::memory_order_relaxed),
            std::memory_order_relaxed);

        txCompletedIntervalSequence.store(sequence + 2,
                                          std::memory_order_release);
    }

    static void RaiseMaximum64(std::atomic<uint64_t>& target,
                               uint64_t candidate) noexcept {
        uint64_t seen = target.load(std::memory_order_relaxed);
        while (candidate > seen &&
               !target.compare_exchange_weak(seen, candidate,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
        }
    }

    static void RaiseMaximum32(std::atomic<uint32_t>& target,
                               uint32_t candidate) noexcept {
        uint32_t seen = target.load(std::memory_order_relaxed);
        while (candidate > seen &&
               !target.compare_exchange_weak(seen, candidate,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
        }
    }

    static void LowerMinimum32(std::atomic<uint32_t>& target,
                               uint32_t candidate) noexcept {
        uint32_t seen = target.load(std::memory_order_relaxed);
        while (candidate < seen &&
               !target.compare_exchange_weak(seen, candidate,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
        }
    }

    [[nodiscard]] static size_t PreparationLatencyBucket(
        uint64_t micros) noexcept {
        using Geometry = ASFW::Audio::Shared::AudioTimingGeometry;
        if (micros <= Geometry::kTxPreparationLatency250Us)  return 0;
        if (micros <= Geometry::kTxPreparationLatency500Us)  return 1;
        if (micros <= Geometry::kTxPreparationLatency750Us)  return 2;
        if (micros <= Geometry::kTxPreparationLatency1000Us) return 3;
        if (micros <= Geometry::kTxPreparationLatency1500Us) return 4;
        return 5;
    }

    [[nodiscard]] static size_t CommittedMarginBucket(
        uint32_t marginPackets) noexcept {
        using Geometry = ASFW::Audio::Shared::AudioTimingGeometry;
        if (marginPackets <= Geometry::kTxCommittedMarginQuarterRingPackets) {
            return 0;
        }
        if (marginPackets <= Geometry::kTxCommittedMarginHalfRingPackets) {
            return 1;
        }
        if (marginPackets <=
            Geometry::kTxCommittedMarginThreeQuarterRingPackets) {
            return 2;
        }
        if (marginPackets <= Geometry::kTxCommittedMarginOneRingPackets) {
            return 3;
        }
        return 4;
    }

    void RequestTimelineEpoch(
        HardwareTimelineDiscontinuity reason) noexcept {
        timelineEpochRequest.store(
            static_cast<uint32_t>(reason) + 1U,
            std::memory_order_release);
    }

    [[nodiscard]] bool ConsumeTimelineEpochRequest(
        HardwareTimelineDiscontinuity& reason) noexcept {
        const uint32_t encoded = timelineEpochRequest.exchange(
            0, std::memory_order_acq_rel);
        if (encoded == 0) return false;
        reason = static_cast<HardwareTimelineDiscontinuity>(encoded - 1U);
        return true;
    }

    // RX control block members
    ASFW::Driver::RxSytCadence rxSytCadence{};
    RxSequenceReplayState rxSequenceReplay{};
    std::atomic<uint32_t> rxTransferDelayTicks{12800};
    std::atomic<uint32_t> txTransferDelayTicks{12800};
    std::atomic<uint64_t> rxReplayEntries{0};
    std::atomic<uint64_t> rxReplayEpochResets{0};

    // Bring-up attribution. Every packet the master stream decodes bumps
    // rxPacketsSeen plus at most one outcome counter, so a stream that never
    // establishes can be explained without a packet analyser:
    //
    //   all zero                        -> nothing arrived; the IR context is
    //                                      not delivering (wrong channel, never
    //                                      started, device not enabled).
    //   seen == noData                  -> the device really is sending only
    //                                      CIP NO-DATA (SYT 0xFFFF).
    //   any reject counter non-zero     -> WE rejected its packets; the device
    //                                      may be streaming fine. geometry in
    //                                      particular means our profile and the
    //                                      device disagree on channels/DBS.
    //   data > 0 but never established  -> valid SYTs arrive but the cadence
    //                                      detector refuses them; see
    //                                      RxSytCadence::Observe.
    //
    // Without the split these are one indistinguishable silence.
    std::atomic<uint64_t> rxPacketsSeen{0};
    std::atomic<uint64_t> rxDataPackets{0};
    std::atomic<uint64_t> rxNoDataPackets{0};
    std::atomic<uint64_t> rxEmptyCompletions{0};
    std::atomic<uint64_t> rxShortPackets{0};
    std::atomic<uint64_t> rxInvalidCipHeaders{0};
    std::atomic<uint64_t> rxZeroDataBlockSize{0};
    std::atomic<uint64_t> rxGeometryMismatch{0};
    std::atomic<uint64_t> txReplayEntries{0};
    std::atomic<uint64_t> txReplayUnderflows{0};
    std::atomic<uint64_t> txReplayInvalidSyt{0};

    std::atomic<uint64_t> inputProducedEndFrame{0};
    std::atomic<uint64_t> inputOverruns{0};
    // Device-domain frame count from CIP DBC (Data Block Counter).
    // Updated by RX interrupt path, read by TX preparation path.
    std::atomic<uint64_t> rxDbcFrameCount{0};

    std::atomic<uint64_t> captureRingWriteFrame{0};
    std::atomic<uint64_t> captureRingReadFrame{0};
    std::atomic<uint64_t> captureRingOverruns{0};
    std::atomic<uint64_t> captureRingStarvations{0};
    RxCaptureBufferTelemetry rxCaptureBufferTelemetry{};

    [[nodiscard]] HostClockAnchorPublishResult PublishHostClockAnchor(
        uint64_t sampleFrame,
        uint64_t hostTicks,
        uint32_t hostNanosPerSampleQ8) noexcept {
        return hostClockAnchor.Publish(
            sampleFrame, hostTicks, hostNanosPerSampleQ8);
    }

    void ResetForStart() noexcept {
        client.Reset();
        device.Reset();
        counters.Reset();
        hostClockAnchor.Reset();

        ioCallbackGeneration.store(0, std::memory_order_relaxed);
        ioLastOperation.store(0, std::memory_order_relaxed);
        ioLastFrameCount.store(0, std::memory_order_relaxed);
        ioLastObjectId.store(0, std::memory_order_relaxed);
        ioLastSampleTime.store(0, std::memory_order_relaxed);
        ioLastHostTime.store(0, std::memory_order_relaxed);

        ioCallbackErrorGeneration.store(0, std::memory_order_relaxed);
        ioCallbackErrorReportedGeneration.store(0, std::memory_order_relaxed);
        ioLastError.store(0, std::memory_order_relaxed);
        ioLastErrorOperation.store(0, std::memory_order_relaxed);
        ioLastErrorFrameCount.store(0, std::memory_order_relaxed);
        ioLastErrorObjectId.store(0, std::memory_order_relaxed);
        ioLastErrorSampleTime.store(0, std::memory_order_relaxed);
        ioLastErrorHostTime.store(0, std::memory_order_relaxed);

        fatalReason.store(FatalStreamReason::None, std::memory_order_release);
        fatalGeneration.store(0, std::memory_order_release);

        discontinuities.store(0, std::memory_order_release);

        // Reset TX members
        txWirePayloadTelemetry.Reset();
        // Preserve the epoch counter across StartIO cycles. BeginEpoch() below
        // clears all per-epoch state while monotonically changing the identity
        // seen by the cache, packet plans, and telemetry.
        pcmPublicationTelemetry.Reset();
        txSytTrace.Reset();
        txPreparationRequests.Reset();
        txFatalSnapshot.Reset();
        txProducerFault.Reset();
        txCycleTrace.Reset();
        timelineEpochRequest.store(0, std::memory_order_relaxed);

        outputConsumedEndFrame.store(0, std::memory_order_relaxed);
        outputUnderruns.store(0, std::memory_order_relaxed);

        playbackRingWriteFrame.store(0, std::memory_order_relaxed);
        playbackRingReadFrame.store(0, std::memory_order_relaxed);
        playbackRingOldestValidFrame.store(0, std::memory_order_relaxed);
        playbackRingDiscontinuityGeneration.store(0, std::memory_order_relaxed);
        playbackRingUnderruns.store(0, std::memory_order_relaxed);
        playbackRingOverruns.store(0, std::memory_order_relaxed);
        txScheduledSampleFrame.store(0, std::memory_order_relaxed);
        txCompletedSampleFrame.store(0, std::memory_order_relaxed);
        txContentDeferrals.store(0, std::memory_order_relaxed);
        txContentDeadlineNoData.store(0, std::memory_order_relaxed);
        txMissedFrames.store(0, std::memory_order_relaxed);
        txTransportCompletionCursor.store(0, std::memory_order_relaxed);
        txTransportCommittedEnd.store(0, std::memory_order_relaxed);
        txTransportStatus.store(0, std::memory_order_relaxed);
        txContentFaultEvents.store(0, std::memory_order_relaxed);
        txContentFirstFaultReason.store(
            static_cast<uint32_t>(TxContentFaultReason::kNone),
            std::memory_order_relaxed);
        txContentFirstFaultPacket.store(0, std::memory_order_relaxed);
        txContentFirstFaultAudioFrame.store(0, std::memory_order_relaxed);
        txContentFirstFaultOldestFrame.store(0, std::memory_order_relaxed);
        txContentFirstFaultWrittenEndFrame.store(0, std::memory_order_relaxed);
        txContentFirstFaultCompletionCursor.store(0, std::memory_order_relaxed);
        txContentFirstFaultCommittedEnd.store(0, std::memory_order_relaxed);
        txCurrentCommittedMarginPackets.store(0, std::memory_order_relaxed);
        txMinimumPreparationDistance.store(UINT32_MAX, std::memory_order_relaxed);
        txMinimumCommittedMarginPackets.store(
            UINT32_MAX, std::memory_order_relaxed);
        txLastPreparationLatencyTicks.store(0, std::memory_order_relaxed);
        txMaxPreparationLatencyTicks.store(0, std::memory_order_relaxed);
        txPreparationLatencySamples.store(0, std::memory_order_relaxed);
        txPreparationAtMost750Us.store(0, std::memory_order_relaxed);
        txPreparationAtLeast1500Us.store(0, std::memory_order_relaxed);
        txIntervalCommittedMarginMinPackets.store(
            UINT32_MAX, std::memory_order_relaxed);
        txIntervalCommittedMarginMaxPackets.store(0, std::memory_order_relaxed);
        txIntervalProducerHeadroomMinFrames.store(
            UINT32_MAX, std::memory_order_relaxed);
        txMinimumProducerHeadroomFrames.store(
            UINT32_MAX, std::memory_order_relaxed);
        txIntervalPreparationLatencyMaxTicks.store(
            0, std::memory_order_relaxed);
        for (auto& bucket : txIntervalPreparationLatencyHistogram) {
            bucket.store(0, std::memory_order_relaxed);
        }
        for (auto& bucket : txIntervalCommittedMarginHistogram) {
            bucket.store(0, std::memory_order_relaxed);
        }
        txCompletedIntervalSequence.store(0, std::memory_order_relaxed);
        txCompletedIntervalMarginMinPackets.store(
            UINT32_MAX, std::memory_order_relaxed);
        txCompletedIntervalMarginMaxPackets.store(0, std::memory_order_relaxed);
        txCompletedIntervalProducerHeadroomMinFrames.store(
            UINT32_MAX, std::memory_order_relaxed);
        txCompletedIntervalPreparationLatencyMaxTicks.store(
            0, std::memory_order_relaxed);
        for (auto& bucket : txCompletedIntervalPreparationLatencyHistogram) {
            bucket.store(0, std::memory_order_relaxed);
        }
        for (auto& bucket : txCompletedIntervalCommittedMarginHistogram) {
            bucket.store(0, std::memory_order_relaxed);
        }
        txHeartbeatLastHostTicks.store(0, std::memory_order_relaxed);
        txLastLeadTicks.store(0, std::memory_order_relaxed);
        txMinimumLeadTicks.store(INT64_MAX, std::memory_order_relaxed);
        txMaximumLeadTicks.store(INT64_MIN, std::memory_order_relaxed);
        txPacketStoreHighWaterPackets.store(0, std::memory_order_relaxed);
        txCompletionLatencyMaxCycles.store(0, std::memory_order_relaxed);
        for (auto& bucket : txDeadlineHeadroomHistogram) {
            bucket.store(0, std::memory_order_relaxed);
        }
        for (auto& bucket : txCompletionLatencyHistogram) {
            bucket.store(0, std::memory_order_relaxed);
        }
        backendDbcDiscontinuities.store(0, std::memory_order_relaxed);
        backendSytDiscontinuities.store(0, std::memory_order_relaxed);
        backendObservationConversions.store(0, std::memory_order_relaxed);
        backendObservationConversionFailures.store(
            0, std::memory_order_relaxed);
        mAudioWarmupGroups.store(0, std::memory_order_relaxed);
        mAudioTxDerivedObservations.store(0, std::memory_order_relaxed);
        mAudioCaptureTransitions.store(0, std::memory_order_relaxed);
        mAudioPostStartConfirmations.store(0, std::memory_order_relaxed);

        // Reset RX members
        rxSytCadence.Reset();
        rxSequenceReplay.Reset();
        rxReplayEntries.store(0, std::memory_order_relaxed);
        rxReplayEpochResets.store(0, std::memory_order_relaxed);
        rxPacketsSeen.store(0, std::memory_order_relaxed);
        rxDataPackets.store(0, std::memory_order_relaxed);
        rxNoDataPackets.store(0, std::memory_order_relaxed);
        rxEmptyCompletions.store(0, std::memory_order_relaxed);
        rxShortPackets.store(0, std::memory_order_relaxed);
        rxInvalidCipHeaders.store(0, std::memory_order_relaxed);
        rxZeroDataBlockSize.store(0, std::memory_order_relaxed);
        rxGeometryMismatch.store(0, std::memory_order_relaxed);
        txReplayEntries.store(0, std::memory_order_relaxed);
        txReplayUnderflows.store(0, std::memory_order_relaxed);
        txReplayInvalidSyt.store(0, std::memory_order_relaxed);

        inputProducedEndFrame.store(0, std::memory_order_relaxed);
        inputOverruns.store(0, std::memory_order_relaxed);
        rxDbcFrameCount.store(0, std::memory_order_relaxed);

        captureRingWriteFrame.store(0, std::memory_order_relaxed);
        captureRingReadFrame.store(0, std::memory_order_relaxed);
        captureRingOverruns.store(0, std::memory_order_relaxed);
        captureRingStarvations.store(0, std::memory_order_relaxed);
        rxCaptureBufferTelemetry.Reset();

        generation.fetch_add(1, std::memory_order_acq_rel);
    }
};

} // namespace ASFW::Audio::Runtime
