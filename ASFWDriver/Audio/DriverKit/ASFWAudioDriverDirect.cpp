//
// ASFWAudioDriverDirect.cpp
// ASFWDriver
//
// Direct endpoint-memory binding helpers for ASFWAudioDriver.
//

#include <new>

#include "ASFWAudioDevice.h"
#include "ASFWAudioDriverPrivate.hpp"
#include "../../Logging/LogConfig.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/DriverKit.h>

#include <limits>
#include <type_traits>
#include <utility>

namespace ASFW::Audio::DriverKit::DirectDiagnostics {

void MaybeLogDirectAudioDebugSnapshot(AudioDriverRuntimeState& runtime) noexcept {
    if (!ASFW::LogConfig::Shared().IsStatisticsEnabled() ||
        ASFW::LogConfig::Shared().GetDirectAudioVerbosity() < 1) {
        return;
    }

    const bool bound = runtime.directAudioSkeletonBound.load(std::memory_order_acquire) &&
                       runtime.directAudioEngine.IsBound();
    const auto snapshot = ASFW::Audio::Runtime::CaptureDirectAudioDebugSnapshot(
        runtime.directAudioGraph,
        bound,
        0,
        ASFW::Audio::Config::kAudioIoPeriodFrames,
        0,
        0,
        0,
        true);

    if (!ASFW::Audio::Runtime::ShouldLogDirectAudioDebugSnapshot(
            runtime.directAudioDebugLog,
            snapshot,
            ASFW::LogDetail::NowNs())) {
        return;
    }

    // NOTE: this snapshot is split across three os_log lines on purpose. os_log
    // truncates a single composed message (~1KB / limited arg count); the
    // previous one-line form was cut off at "writeFrames=", dropping every
    // decisive TX counter (pcmNZ/pcmZero/disc/pending/retired/...). Keep each
    // line well under the limit. All three share the "ADK snapshot" prefix.
    ASFW_LOG(DirectAudio,
             "ADK snapshot/io bound=%d inBase=0x%llx outBase=0x%llx inCap=%u outCap=%u inCh=%u outCh=%u beginRead=%llu writeEnd=%llu beginSample=%llu readEndFrame=%llu writeSample=%llu writeEndFrame=%llu beginFrames=%u writeFrames=%u ioFrames=%u expectedIoFrames=%u outputAvailable=%d",
             snapshot.bound,
             snapshot.inputBufferAddress,
             snapshot.outputBufferAddress,
             snapshot.inputFrameCapacity,
             snapshot.outputFrameCapacity,
             snapshot.inputChannels,
             snapshot.outputChannels,
             snapshot.ioBeginReadCount,
             snapshot.ioWriteEndCount,
             snapshot.inputBeginReadSampleFrame,
             snapshot.inputClientReadEndFrame,
             snapshot.outputWriteEndSampleFrame,
             snapshot.outputClientWriteEndFrame,
             snapshot.inputBeginReadFrameCount,
             snapshot.outputWriteEndFrameCount,
             snapshot.ioBufferFrameSize,
             snapshot.expectedIoBufferFrameSize,
             snapshot.outputReaderAvailableAtWriteEnd);
    ASFW_LOG(DirectAudio,
             "ADK snapshot/ring playback(wr=%llu rd=%llu oldest=%llu avail=%llu underrun=%llu overrun=%llu) timeline(sched=%llu done=%llu targetShort=%llu)",
             snapshot.playbackRingWriteFrame,
             snapshot.playbackRingReadFrame,
             snapshot.playbackRingOldestValidFrame,
             snapshot.playbackRingAvailableFrames,
             snapshot.playbackRingUnderruns,
             snapshot.playbackRingOverruns,
             snapshot.txScheduledSampleFrame,
             snapshot.txCompletedSampleFrame,
             snapshot.txPreparedTargetShortfalls);
    ASFW_LOG(DirectAudio,
             "ADK snapshot/tx v3(deadline=%llu fatal=%u/%llu pkt=%u dist=%u audioFrame=%llu range=[%llu,%llu)) capture(wr=%llu rd=%llu avail=%llu overrun=%llu starve=%llu rxFrames=%llu) packets=%llu data=%llu noData=%llu underruns=%llu",
             snapshot.txPreparationDeadlineFaults,
             static_cast<uint32_t>(snapshot.fatalReason),
             snapshot.fatalGeneration,
             snapshot.fatalPacketIndex,
             snapshot.fatalDistanceToHardware,
             snapshot.fatalAudioFrame,
             snapshot.fatalOldestValidFrame,
             snapshot.fatalWrittenEndFrame,
             snapshot.captureRingWriteFrame,
             snapshot.captureRingReadFrame,
             snapshot.captureRingAvailableFrames,
             snapshot.captureRingOverruns,
             snapshot.captureRingStarvations,
             snapshot.rxDecodedFrames,
             snapshot.directTxPackets,
             snapshot.txDataPackets,
             snapshot.txNoDataPackets,
             snapshot.directTxUnderruns);
    ASFW_LOG(DirectAudio,
             "ADK TX PREP WAKE requested=%llu handled=%llu pending=%llu requestTicks=%llu handledTicks=%llu requests=%llu dispatches=%llu coalesced=%llu drainPasses=%llu",
             snapshot.txPreparationRequestedGeneration,
             snapshot.txPreparationHandledGeneration,
             snapshot.txPreparationRequestedGeneration >=
                     snapshot.txPreparationHandledGeneration
                 ? snapshot.txPreparationRequestedGeneration -
                       snapshot.txPreparationHandledGeneration
                 : 0,
             snapshot.txPreparationRequestHostTicks,
             snapshot.txPreparationHandledHostTicks,
             snapshot.txPreparationWakeRequests,
             snapshot.txPreparationWakeDispatches,
             snapshot.txPreparationWakeCoalesced,
             snapshot.txPreparationDrainPasses);
    auto* directControl = runtime.directAudioGraph.control;
    const int64_t transferDelayTicks =
        directControl
            ? static_cast<int64_t>(directControl->txTransferDelayTicks.load(
                  std::memory_order_relaxed))
            : 0;
    const int64_t wireLeadMinimum =
        snapshot.txMinimumLeadTicks == std::numeric_limits<int64_t>::max()
            ? std::numeric_limits<int64_t>::max()
            : snapshot.txMinimumLeadTicks + transferDelayTicks;
    const int64_t wireLeadMaximum =
        snapshot.txMaximumLeadTicks == std::numeric_limits<int64_t>::min()
            ? std::numeric_limits<int64_t>::min()
            : snapshot.txMaximumLeadTicks + transferDelayTicks;
    ASFW_LOG(
        DirectAudio,
        "ADK timing anchor(generation=%llu frame=%llu updates=%llu mirrors=%llu invalid=%llu) txLead(last=%lld min=%lld max=%lld) wireLead(last=%lld min=%lld max=%lld) packets(data=%llu noData=%llu empty=%llu postLockNoData=%llu) refillLatency(last=%llu max=%llu samples=%llu le750us=%llu ge1500us=%llu) minCommittedMargin=%llu",
        snapshot.hostAnchorGeneration,
        snapshot.hostAnchorFrame,
        snapshot.hostAnchorUpdates,
        snapshot.hostAnchorMirrorPublications,
        snapshot.hostAnchorInvalidUpdates,
        snapshot.txLastLeadTicks,
        snapshot.txMinimumLeadTicks,
        snapshot.txMaximumLeadTicks,
        snapshot.txLastLeadTicks + transferDelayTicks,
        wireLeadMinimum,
        wireLeadMaximum,
        snapshot.txDataPackets,
        snapshot.txNoDataPackets,
        snapshot.txEmptyPackets,
        snapshot.txPostLockNoDataPackets,
        snapshot.txLastPreparationLatencyTicks,
        snapshot.txMaxPreparationLatencyTicks,
        snapshot.txPreparationLatencySamples,
        snapshot.txPreparationAtMost750Us,
        snapshot.txPreparationAtLeast1500Us,
        snapshot.txMinimumCommittedMarginPackets);

    if (directControl) {
        const auto& cache = directControl->pcmPublicationTelemetry;
        ASFW_LOG(
            DirectAudio,
            "ADK tx-content cache=[%llu,%llu) scheduled=%llu publications=%llu frames=%llu copies=%llu/%llu/%llu/%llu defers=%llu deadlineNoData=%llu wrongEpoch=%llu missedFrames=%llu firstFault=%u packet=%llu frame=%llu",
            cache.oldestValidFrame.load(std::memory_order_relaxed),
            cache.publishedEndFrame.load(std::memory_order_relaxed),
            directControl->txScheduledSampleFrame.load(
                std::memory_order_relaxed),
            cache.publications.load(std::memory_order_relaxed),
            cache.framesPublished.load(std::memory_order_relaxed),
            cache.copiesReady.load(std::memory_order_relaxed),
            cache.copiesNotYetPublished.load(std::memory_order_relaxed),
            cache.copiesExpired.load(std::memory_order_relaxed),
            cache.copiesConcurrentRewrite.load(std::memory_order_relaxed),
            directControl->txContentDeferrals.load(std::memory_order_relaxed),
            directControl->txContentDeadlineNoData.load(
                std::memory_order_relaxed),
            cache.copiesWrongEpoch.load(std::memory_order_relaxed),
            directControl->txMissedFrames.load(std::memory_order_relaxed),
            directControl->txContentFirstFaultReason.load(
                std::memory_order_acquire),
            directControl->txContentFirstFaultPacket.load(
                std::memory_order_relaxed),
            directControl->txContentFirstFaultAudioFrame.load(
                std::memory_order_relaxed));
    }
}

void ForceLogDirectAudioDebugSnapshot(AudioDriverRuntimeState& runtime, const char* context) noexcept {
    const bool bound = runtime.directAudioSkeletonBound.load(std::memory_order_acquire);
    const auto snapshot = ASFW::Audio::Runtime::CaptureDirectAudioDebugSnapshot(
        runtime.directAudioGraph,
        bound,
        0,
        ASFW::Audio::Config::kAudioIoPeriodFrames,
        0,
        0,
        0,
        true);

    ASFW_LOG(
        DirectAudio,
        "ADK FORCED CORE (%{public}s) bound=%d writeEnd=%llu playback=[%llu,%llu) oldest=%llu avail=%llu",
        context ? context : "unknown",
        snapshot.bound,
        snapshot.outputClientWriteEndFrame,
        snapshot.playbackRingReadFrame,
        snapshot.playbackRingWriteFrame,
        snapshot.playbackRingOldestValidFrame,
        snapshot.playbackRingAvailableFrames);
    ASFW_LOG(
        DirectAudio,
        "ADK FORCED FATAL reason=%u generation=%llu pkt=%u distance=%u audioFrame=%llu phase=%lld valid=[%llu,%llu)",
        static_cast<uint32_t>(snapshot.fatalReason),
        snapshot.fatalGeneration,
        snapshot.fatalPacketIndex,
        snapshot.fatalDistanceToHardware,
        snapshot.fatalAudioFrame,
        snapshot.fatalOutputPhaseTicks,
        snapshot.fatalOldestValidFrame,
        snapshot.fatalWrittenEndFrame);
}

} // namespace ASFW::Audio::DriverKit::DirectDiagnostics

namespace ASFW::Audio::DriverKit {
namespace {

[[nodiscard]] ASFW::Audio::Runtime::AudioStreamMode DirectStreamModeFromRaw(uint32_t streamModeRaw) noexcept {
    return streamModeRaw == std::to_underlying(ASFW::Isoch::Audio::StreamMode::kBlocking)
         ? ASFW::Audio::Runtime::AudioStreamMode::kBlocking
         : ASFW::Audio::Runtime::AudioStreamMode::kNonBlocking;
}

} // namespace

bool BindDirectAudioSkeleton(ASFWAudioDriver_IVars& ivars,
                             DirectAudioMemoryGeometry physicalGeometry) noexcept {
    if (!ivars.audioDevice) {
        ASFW_LOG(DirectAudio,
                 "ADK FATAL BIND skeleton failed null_audioDevice endpoint=%llu",
                 ivars.device.endpointId);
        return false;
    }
    if (!ivars.inputMap || !ivars.outputMap || !ivars.controlMap) {
        ASFW_LOG(DirectAudio,
                 "ADK DBG BIND skeleton failed missing_maps inMap=%p outMap=%p controlMap=%p",
                 static_cast<void*>(ivars.inputMap.get()),
                 static_cast<void*>(ivars.outputMap.get()),
                 static_cast<void*>(ivars.controlMap.get()));
        return false;
    }

    auto* control = reinterpret_cast<ASFW::Audio::Runtime::AudioTransportControlBlock*>(
        static_cast<uintptr_t>(ivars.controlMap->GetAddress()));
    if (!control) {
        ASFW_LOG(DirectAudio, "ADK DBG BIND skeleton failed null_control");
        return false;
    }
    control->ResetForStart();

    IOAddressSegment inputSegment{};
    inputSegment.address = ivars.inputMap->GetAddress();
    inputSegment.length = ivars.inputMap->GetLength();
    IOAddressSegment outputSegment{};
    outputSegment.address = ivars.outputMap->GetAddress();
    outputSegment.length = ivars.outputMap->GetLength();

    const auto hasRequiredBytes = [](const IOAddressSegment& segment,
                                     uint32_t frames,
                                     uint32_t channels) noexcept {
        if (segment.address == 0 || frames == 0 || channels == 0) {
            return false;
        }
        const uint64_t bytes = static_cast<uint64_t>(frames) *
            static_cast<uint64_t>(channels) * sizeof(float);
        return bytes <= segment.length;
    };
    if (!hasRequiredBytes(inputSegment, physicalGeometry.inputFrames,
                          physicalGeometry.inputChannels) ||
        !hasRequiredBytes(outputSegment, physicalGeometry.outputFrames,
                          physicalGeometry.outputChannels)) {
        ASFW_LOG_ERROR(DirectAudio,
                       "ADK FATAL BIND skeleton logical geometry exceeds mapping "
                       "in=%u*%u/%llu out=%u*%u/%llu",
                       physicalGeometry.inputFrames, physicalGeometry.inputChannels,
                       inputSegment.length, physicalGeometry.outputFrames,
                       physicalGeometry.outputChannels, outputSegment.length);
        return false;
    }

    // The copied endpoint snapshot is the sole wire-format source on the ADK side.
    ASFW::Audio::Runtime::AudioWireFormat wireFormat = ASFW::Audio::Runtime::AudioWireFormat::kAM824;
    if (ivars.resolvedProfile.TxWireFormat() ==
        ASFW::Encoding::AudioWireFormat::kRawPcm24In32) {
        wireFormat = ASFW::Audio::Runtime::AudioWireFormat::kRawPcm24In32;
    }

    ivars.runtime.directAudioGraph = ASFW::Audio::Runtime::AudioGraphBinding{
        .endpointId = ASFW::Audio::Devices::AudioEndpointId{ivars.device.endpointId},
        .sampleRateHz = static_cast<uint32_t>(ivars.device.currentSampleRate),
        .memory = ASFW::Audio::Runtime::AudioStreamMemory{
            .inputBase = reinterpret_cast<float*>(static_cast<uintptr_t>(ivars.inputMap->GetAddress())),
            .outputBase = reinterpret_cast<const float*>(static_cast<uintptr_t>(ivars.outputMap->GetAddress())),
            .activeInputRingFrames = physicalGeometry.inputFrames,
            .activeOutputRingFrames = physicalGeometry.outputFrames,
            .inputChannels = physicalGeometry.inputChannels,
            .outputChannels = physicalGeometry.outputChannels,
            .storage = ASFW::Audio::Runtime::AudioSampleStorage::kFloat32Native,
        },
        .control = control,
        .deviceToHostAm824Slots = physicalGeometry.inputChannels,
        .hostToDeviceAm824Slots = physicalGeometry.outputChannels,
        .streamMode = DirectStreamModeFromRaw(ivars.device.streamModeRaw),
        .hostToDeviceWireFormat = wireFormat,
        .audioDevice = ivars.audioDevice.get(),
    };

    ivars.runtime.directAudioDebugLog.Reset();
    ivars.runtime.lastHalZeroTimestampGeneration.store(0, std::memory_order_release);
    ivars.runtime.lastHalZeroTimestampSampleFrame.store(0, std::memory_order_release);
    ivars.runtime.lastHalZeroTimestampHostTicks.store(0, std::memory_order_release);
    ivars.runtime.directAudioSkeletonBound.store(true, std::memory_order_release);
    ASFW_LOG(DirectAudio,
             "ADK DBG BIND skeleton %{public}s outBase=%p outFrames=%u outCh=%u inBase=%p inFrames=%u inCh=%u control=%p audioDevice=%p rate=%u",
             "bound",
             static_cast<const void*>(ivars.runtime.directAudioGraph.memory.outputBase),
             ivars.runtime.directAudioGraph.memory.activeOutputRingFrames,
             ivars.runtime.directAudioGraph.memory.outputChannels,
             static_cast<void*>(ivars.runtime.directAudioGraph.memory.inputBase),
             ivars.runtime.directAudioGraph.memory.activeInputRingFrames,
             ivars.runtime.directAudioGraph.memory.inputChannels,
             static_cast<void*>(ivars.runtime.directAudioGraph.control),
             static_cast<void*>(ivars.runtime.directAudioGraph.audioDevice),
             ivars.runtime.directAudioGraph.sampleRateHz);
    return true;
}

bool UpdateDirectAudioGeometry(ASFWAudioDriver_IVars& ivars,
                               DirectAudioMemoryGeometry physicalGeometry) noexcept {
    if (!ivars.runtime.directAudioSkeletonBound.load(std::memory_order_acquire) ||
        !ivars.runtime.directAudioGraph.control || physicalGeometry.inputFrames == 0 ||
        physicalGeometry.outputFrames == 0 || physicalGeometry.inputChannels == 0 ||
        physicalGeometry.outputChannels == 0 || !ivars.inputMap || !ivars.outputMap) {
        return false;
    }
    const uint64_t requiredInputBytes = static_cast<uint64_t>(physicalGeometry.inputFrames) *
        physicalGeometry.inputChannels * sizeof(float);
    const uint64_t requiredOutputBytes = static_cast<uint64_t>(physicalGeometry.outputFrames) *
        physicalGeometry.outputChannels * sizeof(float);
    if (requiredInputBytes > ivars.inputMap->GetLength() ||
        requiredOutputBytes > ivars.outputMap->GetLength()) {
        ASFW_LOG_ERROR(DirectAudio,
                       "[AudioConfig] ADK geometry exceeds retained mapping in=%u*%u out=%u*%u",
                       physicalGeometry.inputFrames, physicalGeometry.inputChannels,
                       physicalGeometry.outputFrames, physicalGeometry.outputChannels);
        return false;
    }

    auto& graph = ivars.runtime.directAudioGraph;
    graph.sampleRateHz = static_cast<uint32_t>(ivars.device.currentSampleRate);
    graph.memory.activeInputRingFrames = physicalGeometry.inputFrames;
    graph.memory.activeOutputRingFrames = physicalGeometry.outputFrames;
    graph.memory.inputChannels = physicalGeometry.inputChannels;
    graph.memory.outputChannels = physicalGeometry.outputChannels;
    graph.deviceToHostAm824Slots = physicalGeometry.inputChannels;
    graph.hostToDeviceAm824Slots = physicalGeometry.outputChannels;
    ASFW_LOG(DirectAudio,
             "[AudioConfig] ADK direct view updated rate=%u in=%u/%u out=%u/%u",
             graph.sampleRateHz, graph.memory.activeInputRingFrames,
             graph.memory.inputChannels, graph.memory.activeOutputRingFrames,
             graph.memory.outputChannels);
    return graph.IsValid();
}

void UnbindDirectAudioSkeleton(ASFWAudioDriver_IVars& ivars) noexcept {
    ivars.runtime.directAudioSkeletonBound.store(false, std::memory_order_release);
    ivars.runtime.directAudioGraph = {};
    ivars.runtime.lastHalZeroTimestampGeneration.store(0, std::memory_order_release);
    ivars.runtime.lastHalZeroTimestampSampleFrame.store(0, std::memory_order_release);
    ivars.runtime.lastHalZeroTimestampHostTicks.store(0, std::memory_order_release);
}

} // namespace ASFW::Audio::DriverKit
