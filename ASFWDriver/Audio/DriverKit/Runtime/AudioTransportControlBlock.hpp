#pragma once

#include "AudioClientCursor.hpp"
#include "AudioRtCounters.hpp"
#include "DeviceTimeline.hpp"
#include "TxSytTrace.hpp"
#include "PayloadWriterTelemetry.hpp"
#include "TxWirePayloadTelemetry.hpp"
#include "../../Runtime/HostClockAnchor.hpp"
#include "../../Wire/AMDTP/RxSequenceReplay.hpp"
#include "../../Wire/AMDTP/RxSytCadence.hpp"
#include "../../../Shared/Isoch/AudioTimingGeometry.hpp"

#include <atomic>
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

struct TxPreparationRequestState final {
    std::atomic<uint64_t> requestedGeneration{0};
    std::atomic<uint64_t> handledGeneration{0};
    std::atomic<uint64_t> requestHostTicks{0};
    std::atomic<uint64_t> handledHostTicks{0};

    [[nodiscard]] uint64_t PublishRequest(uint64_t hostTicks) noexcept {
        requestHostTicks.store(hostTicks, std::memory_order_relaxed);
        return requestedGeneration.fetch_add(1, std::memory_order_release) + 1;
    }

    [[nodiscard]] uint64_t RequestedGeneration() const noexcept {
        return requestedGeneration.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool NeedsHandling() const noexcept {
        return handledGeneration.load(std::memory_order_acquire) <
               requestedGeneration.load(std::memory_order_acquire);
    }

    void MarkHandled(uint64_t generation, uint64_t hostTicks) noexcept {
        handledHostTicks.store(hostTicks, std::memory_order_relaxed);
        handledGeneration.store(generation, std::memory_order_release);
    }

    void Reset() noexcept {
        requestedGeneration.store(0, std::memory_order_relaxed);
        handledGeneration.store(0, std::memory_order_relaxed);
        requestHostTicks.store(0, std::memory_order_relaxed);
        handledHostTicks.store(0, std::memory_order_relaxed);
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
    PayloadWriterTelemetryRing payloadWriterTelemetry{};
    TxWirePayloadTelemetry txWirePayloadTelemetry{};

    // Latest-value trace of the live replay TX SYT decision (diagnostics).
    TxSytTraceLatest txSytTrace{};
    TxPreparationRequestState txPreparationRequests{};
    TxFatalSnapshot txFatalSnapshot{};
    TxProducerFaultSnapshot txProducerFault{};

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
    std::atomic<uint32_t> txMinimumPreparationDistance{UINT32_MAX};
    std::atomic<uint32_t> txMinimumCommittedMarginPackets{UINT32_MAX};
    std::atomic<uint64_t> txLastPreparationLatencyTicks{0};
    std::atomic<uint64_t> txMaxPreparationLatencyTicks{0};
    std::atomic<uint64_t> txPreparationLatencySamples{0};
    std::atomic<uint64_t> txPreparationAtMost750Us{0};
    std::atomic<uint64_t> txPreparationAtLeast1500Us{0};
    std::atomic<int64_t> txLastLeadTicks{0};
    std::atomic<int64_t> txMinimumLeadTicks{INT64_MAX};
    std::atomic<int64_t> txMaximumLeadTicks{INT64_MIN};

    // RX control block members
    ASFW::Driver::RxSytCadence rxSytCadence{};
    RxSequenceReplayState rxSequenceReplay{};
    std::atomic<uint32_t> rxTransferDelayTicks{12800};
    std::atomic<uint32_t> txTransferDelayTicks{12800};
    std::atomic<uint64_t> rxReplayEntries{0};
    std::atomic<uint64_t> rxReplayEpochResets{0};
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
        payloadWriterTelemetry.Reset();
        txWirePayloadTelemetry.Reset();
        txSytTrace.Reset();
        txPreparationRequests.Reset();
        txFatalSnapshot.Reset();
        txProducerFault.Reset();

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
        txMinimumPreparationDistance.store(UINT32_MAX, std::memory_order_relaxed);
        txMinimumCommittedMarginPackets.store(
            UINT32_MAX, std::memory_order_relaxed);
        txLastPreparationLatencyTicks.store(0, std::memory_order_relaxed);
        txMaxPreparationLatencyTicks.store(0, std::memory_order_relaxed);
        txPreparationLatencySamples.store(0, std::memory_order_relaxed);
        txPreparationAtMost750Us.store(0, std::memory_order_relaxed);
        txPreparationAtLeast1500Us.store(0, std::memory_order_relaxed);
        txLastLeadTicks.store(0, std::memory_order_relaxed);
        txMinimumLeadTicks.store(INT64_MAX, std::memory_order_relaxed);
        txMaximumLeadTicks.store(INT64_MIN, std::memory_order_relaxed);

        // Reset RX members
        rxSytCadence.Reset();
        rxSequenceReplay.Reset();
        rxReplayEntries.store(0, std::memory_order_relaxed);
        rxReplayEpochResets.store(0, std::memory_order_relaxed);
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

        generation.fetch_add(1, std::memory_order_acq_rel);
    }
};

} // namespace ASFW::Audio::Runtime
