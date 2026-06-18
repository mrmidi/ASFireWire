//
// ASFWAudioDriverDirect.cpp
// ASFWDriver
//
// Direct endpoint-memory binding helpers for ASFWAudioDriver.
//

#include <new>

#include "ASFWAudioDevice.h"
#include "ASFWAudioDriverPrivate.hpp"
#include "Config/AudioProfileRegistry.hpp"
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
        ASFW::Isoch::Config::kAudioIoPeriodFrames,
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
             "ADK snapshot/ring playback(wr=%llu rd=%llu oldest=%llu avail=%llu underrun=%llu overrun=%llu) timeline(sched=%llu done=%llu rebase=%llu fallback=%llu stale=%llu ahead=%llu pcmNZ=%llu pcmZero=%llu prep=%llu startup=%llu)",
             snapshot.playbackRingWriteFrame,
             snapshot.playbackRingReadFrame,
             snapshot.playbackRingOldestValidFrame,
             snapshot.playbackRingAvailableFrames,
             snapshot.playbackRingUnderruns,
             snapshot.playbackRingOverruns,
             snapshot.txScheduledSampleFrame,
             snapshot.txCompletedSampleFrame,
             snapshot.txPhaseRebases,
             snapshot.txSilenceFallback,
             snapshot.txStaleOverwrittenReads,
             snapshot.txProducerAheadUnderruns,
             snapshot.txPcmNonzeroPackets,
             snapshot.txPcmAllZeroPackets,
             snapshot.txPreparedPcmSlots,
             snapshot.txStartupSilenceSlots);
    ASFW_LOG(DirectAudio,
             "ADK snapshot/tx faults(readAhead=%llu overwritten=%llu deadline=%llu ownership=%llu stops=%llu fatal=%u/%llu pkt=%u dist=%u audioFrame=%llu phase=%lld valid=[%llu,%llu)) capture(wr=%llu rd=%llu avail=%llu overrun=%llu starve=%llu rxFrames=%llu) txPackets=%llu txUnderruns=%llu txSilence=%llu txValidPcm=%llu txValidSilence=%llu txNoPhaseSilence=%llu txUnderrunSilence=%llu txStaleSync=%llu txInvalidGeom=%llu",
             snapshot.txReadAheadFaults,
             snapshot.txSourceOverwrittenFaults,
             snapshot.txPreparationDeadlineFaults,
             snapshot.txSlotOwnershipFaults,
             snapshot.txImmediateStops,
             static_cast<uint32_t>(snapshot.fatalReason),
             snapshot.fatalGeneration,
             snapshot.fatalPacketIndex,
             snapshot.fatalDistanceToHardware,
             snapshot.fatalAudioFrame,
             snapshot.fatalOutputPhaseTicks,
             snapshot.fatalOldestValidFrame,
             snapshot.fatalWrittenEndFrame,
             snapshot.captureRingWriteFrame,
             snapshot.captureRingReadFrame,
             snapshot.captureRingAvailableFrames,
             snapshot.captureRingOverruns,
             snapshot.captureRingStarvations,
             snapshot.rxDecodedFrames,
             snapshot.directTxPackets,
             snapshot.directTxUnderruns,
             snapshot.directTxSilenceSubstitutions,
             snapshot.txValidPhasePcmPackets,
             snapshot.txValidPhaseSilencePackets,
             snapshot.txNoPhaseSilencePackets,
             snapshot.txUnderrunSilencePackets,
             snapshot.txStaleSyncPackets,
             snapshot.txInvalidGeometryPackets);
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
        "ADK timing anchor(generation=%llu frame=%llu updates=%llu mirrors=%llu invalid=%llu) txLead(last=%lld min=%lld max=%lld) wireLead(last=%lld min=%lld max=%lld) packets(data=%llu noData=%llu postLockNoData=%llu) refillLatency(last=%llu max=%llu samples=%llu le750us=%llu ge1500us=%llu) minCommittedMargin=%llu",
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
        snapshot.txPostLockNoDataPackets,
        snapshot.txLastPreparationLatencyTicks,
        snapshot.txMaxPreparationLatencyTicks,
        snapshot.txPreparationLatencySamples,
        snapshot.txPreparationAtMost750Us,
        snapshot.txPreparationAtLeast1500Us,
        snapshot.txMinimumCommittedMarginPackets);

    const auto& pw = runtime.txStreamEngine.PayloadWriterCounters();
    ASFW_LOG(
        DirectAudio,
        "ADK writer/visited=%llu written=%llu withoutPkt=%llu outsidePkt=%llu racedReuse=%llu wroteIntoTx=%llu nonZero=%llu slotsNZ=%llu maxAbs=%u",
        pw.framesVisited.load(std::memory_order_relaxed),
        pw.framesWritten.load(std::memory_order_relaxed),
        pw.framesWithoutPacket.load(std::memory_order_relaxed),
        pw.framesOutsidePacket.load(std::memory_order_relaxed),
        pw.framesRacedReuse.load(std::memory_order_relaxed),
        pw.framesWroteIntoTransmitted.load(std::memory_order_relaxed),
        pw.framesNonZero.load(std::memory_order_relaxed),
        pw.slotsNonZero.load(std::memory_order_relaxed),
        pw.maxAbsSampleBits.load(std::memory_order_relaxed));
}

void ForceLogDirectAudioDebugSnapshot(AudioDriverRuntimeState& runtime, const char* context) noexcept {
    const bool bound = runtime.directAudioSkeletonBound.load(std::memory_order_acquire);
    const auto snapshot = ASFW::Audio::Runtime::CaptureDirectAudioDebugSnapshot(
        runtime.directAudioGraph,
        bound,
        0,
        ASFW::Isoch::Config::kAudioIoPeriodFrames,
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

uint32_t FrameCapacityFromSegment(const IOAddressSegment& segment,
                                  uint32_t channels) noexcept {
    if (segment.address == 0 || segment.length == 0 || channels == 0) {
        return 0;
    }

    const uint64_t bytesPerFrame = uint64_t{sizeof(int32_t)} * channels;
    if (bytesPerFrame == 0) {
        return 0;
    }

    const uint64_t frameCapacity = segment.length / bytesPerFrame;
    constexpr uint32_t kMaxFrameCapacity = std::numeric_limits<uint32_t>::max();
    return frameCapacity > kMaxFrameCapacity
         ? kMaxFrameCapacity
         : static_cast<uint32_t>(frameCapacity);
}

bool BindDirectAudioSkeleton(ASFWAudioDriver_IVars& ivars) noexcept {
    if (!ivars.audioDevice) {
        ASFW_LOG(DirectAudio,
                 "ADK FATAL BIND skeleton failed null_audioDevice guid=0x%016llx",
                 ivars.device.guid);
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

    const uint32_t inputFrameCapacity =
        FrameCapacityFromSegment(inputSegment, ivars.device.inputChannelCount);
    const uint32_t outputFrameCapacity =
        FrameCapacityFromSegment(outputSegment, ivars.device.outputChannelCount);

    // Resolve the device profile to determine the host-to-device wire format.
    // Standard DICE fallback profiles use AM824 sub-frame formatting, while
    // Focusrite Saffire playback uses sign-extended 24-in-32 big-endian formatting.
    ASFW::Audio::Runtime::AudioWireFormat wireFormat = ASFW::Audio::Runtime::AudioWireFormat::kAM824;
    if (const auto* profile = ASFW::Isoch::Audio::AudioProfileRegistry::FindProfile(
            ivars.device.vendorId, ivars.device.modelId, ivars.device.guid)) {
        if (profile->TxWireFormat() == ASFW::Encoding::AudioWireFormat::kRawPcm24In32) {
            wireFormat = ASFW::Audio::Runtime::AudioWireFormat::kRawPcm24In32;
        }
    }

    ivars.runtime.directAudioGraph = ASFW::Audio::Runtime::AudioGraphBinding{
        .guid = ivars.device.guid,
        .sampleRateHz = static_cast<uint32_t>(ivars.device.currentSampleRate),
        .memory = ASFW::Audio::Runtime::AudioStreamMemory{
            .inputBase = reinterpret_cast<float*>(static_cast<uintptr_t>(ivars.inputMap->GetAddress())),
            .outputBase = reinterpret_cast<const float*>(static_cast<uintptr_t>(ivars.outputMap->GetAddress())),
            .inputFrameCapacity = inputFrameCapacity,
            .outputFrameCapacity = outputFrameCapacity,
            .inputChannels = ivars.device.inputChannelCount,
            .outputChannels = ivars.device.outputChannelCount,
            .storage = ASFW::Audio::Runtime::AudioSampleStorage::kFloat32Native,
        },
        .control = control,
        .deviceToHostAm824Slots = ivars.device.inputChannelCount,
        .hostToDeviceAm824Slots = ivars.device.outputChannelCount,
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
             "ADK DBG BIND skeleton %s outBase=%p outFrames=%u outCh=%u inBase=%p inFrames=%u inCh=%u control=%p audioDevice=%p rate=%u",
             "bound",
             static_cast<const void*>(ivars.runtime.directAudioGraph.memory.outputBase),
             ivars.runtime.directAudioGraph.memory.outputFrameCapacity,
             ivars.runtime.directAudioGraph.memory.outputChannels,
             static_cast<void*>(ivars.runtime.directAudioGraph.memory.inputBase),
             ivars.runtime.directAudioGraph.memory.inputFrameCapacity,
             ivars.runtime.directAudioGraph.memory.inputChannels,
             static_cast<void*>(ivars.runtime.directAudioGraph.control),
             static_cast<void*>(ivars.runtime.directAudioGraph.audioDevice),
             ivars.runtime.directAudioGraph.sampleRateHz);
    return true;
}

void UnbindDirectAudioSkeleton(ASFWAudioDriver_IVars& ivars) noexcept {
    ivars.runtime.directAudioSkeletonBound.store(false, std::memory_order_release);
    ivars.runtime.directAudioGraph = {};
    ivars.runtime.lastHalZeroTimestampGeneration.store(0, std::memory_order_release);
    ivars.runtime.lastHalZeroTimestampSampleFrame.store(0, std::memory_order_release);
    ivars.runtime.lastHalZeroTimestampHostTicks.store(0, std::memory_order_release);
}

} // namespace ASFW::Audio::DriverKit
