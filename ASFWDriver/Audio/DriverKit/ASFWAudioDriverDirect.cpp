//
// ASFWAudioDriverDirect.cpp
// ASFWDriver
//
// Direct endpoint-memory binding helpers for ASFWAudioDriver.
//

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

    ASFW_LOG(DirectAudio,
             "ADK snapshot bound=%d inBase=0x%llx outBase=0x%llx inCap=%u outCap=%u inCh=%u outCh=%u beginRead=%llu writeEnd=%llu beginSample=%llu readEndFrame=%llu writeSample=%llu writeEndFrame=%llu beginFrames=%u writeFrames=%u ioFrames=%u expectedIoFrames=%u outputAvailable=%d playback(wr=%llu rd=%llu avail=%llu underrun=%llu overrun=%llu) capture(wr=%llu rd=%llu avail=%llu overrun=%llu starve=%llu rxFrames=%llu) txPackets=%llu txUnderruns=%llu txSilence=%llu txValidPcm=%llu txValidSilence=%llu txNoPhaseSilence=%llu txUnderrunSilence=%llu txStaleSync=%llu txInvalidGeom=%llu",
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
             snapshot.outputReaderAvailableAtWriteEnd,
             snapshot.playbackRingWriteFrame,
             snapshot.playbackRingReadFrame,
             snapshot.playbackRingAvailableFrames,
             snapshot.playbackRingUnderruns,
             snapshot.playbackRingOverruns,
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
}

void ForceLogDirectAudioDebugSnapshot(AudioDriverRuntimeState& runtime, const char* context) noexcept {
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

    ASFW_LOG(DirectAudio,
             "ADK FORCED SNAPSHOT (%{public}s) bound=%d inBase=0x%llx outBase=0x%llx inCap=%u outCap=%u inCh=%u outCh=%u beginRead=%llu writeEnd=%llu beginSample=%llu readEndFrame=%llu writeSample=%llu writeEndFrame=%llu beginFrames=%u writeFrames=%u ioFrames=%u expectedIoFrames=%u outputAvailable=%d playback(wr=%llu rd=%llu avail=%llu underrun=%llu overrun=%llu) capture(wr=%llu rd=%llu avail=%llu overrun=%llu starve=%llu rxFrames=%llu) txPackets=%llu txUnderruns=%llu txSilence=%llu txValidPcm=%llu txValidSilence=%llu txNoPhaseSilence=%llu txUnderrunSilence=%llu txStaleSync=%llu txInvalidGeom=%llu",
             context ? context : "unknown",
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
             snapshot.outputReaderAvailableAtWriteEnd,
             snapshot.playbackRingWriteFrame,
             snapshot.playbackRingReadFrame,
             snapshot.playbackRingAvailableFrames,
             snapshot.playbackRingUnderruns,
             snapshot.playbackRingOverruns,
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

    ivars.runtime.directAudioGraph = ASFW::Audio::Runtime::AudioGraphBinding{
        .guid = ivars.device.guid,
        .sampleRateHz = static_cast<uint32_t>(ivars.device.currentSampleRate),
        .memory = ASFW::Audio::Runtime::AudioStreamMemory{
            .inputBase = reinterpret_cast<int32_t*>(static_cast<uintptr_t>(ivars.inputMap->GetAddress())),
            .outputBase = reinterpret_cast<const int32_t*>(static_cast<uintptr_t>(ivars.outputMap->GetAddress())),
            .inputFrameCapacity = inputFrameCapacity,
            .outputFrameCapacity = outputFrameCapacity,
            .inputChannels = ivars.device.inputChannelCount,
            .outputChannels = ivars.device.outputChannelCount,
            .storage = ASFW::Audio::Runtime::AudioSampleStorage::kInt32Native,
        },
        .control = control,
        .deviceToHostAm824Slots = ivars.device.inputChannelCount,
        .hostToDeviceAm824Slots = ivars.device.outputChannelCount,
        .streamMode = DirectStreamModeFromRaw(ivars.device.streamModeRaw),
        .hostToDeviceWireFormat = ASFW::Audio::Runtime::AudioWireFormat::kAM824,
        .audioDevice = ivars.audioDevice.get(),
    };

    const bool bound = ivars.runtime.directAudioEngine.Bind(ivars.runtime.directAudioGraph);
    ivars.runtime.directAudioDebugLog.Reset();
    ivars.runtime.lastHalZeroTimestampGeneration.store(0, std::memory_order_release);
    ivars.runtime.directAudioSkeletonBound.store(bound, std::memory_order_release);
    ASFW_LOG(DirectAudio,
             "ADK DBG BIND skeleton %s outBase=%p outFrames=%u outCh=%u inBase=%p inFrames=%u inCh=%u control=%p audioDevice=%p rate=%u",
             bound ? "bound" : "inactive",
             static_cast<const void*>(ivars.runtime.directAudioGraph.memory.outputBase),
             ivars.runtime.directAudioGraph.memory.outputFrameCapacity,
             ivars.runtime.directAudioGraph.memory.outputChannels,
             static_cast<void*>(ivars.runtime.directAudioGraph.memory.inputBase),
             ivars.runtime.directAudioGraph.memory.inputFrameCapacity,
             ivars.runtime.directAudioGraph.memory.inputChannels,
             static_cast<void*>(ivars.runtime.directAudioGraph.control),
             static_cast<void*>(ivars.runtime.directAudioGraph.audioDevice),
             ivars.runtime.directAudioGraph.sampleRateHz);
    return bound;
}

void UnbindDirectAudioSkeleton(ASFWAudioDriver_IVars& ivars) noexcept {
    ivars.runtime.directAudioSkeletonBound.store(false, std::memory_order_release);
    ivars.runtime.directAudioEngine.Unbind();
    ivars.runtime.directAudioGraph = {};
    ivars.runtime.ztsTimelineInitialized.store(false, std::memory_order_release);
    ivars.runtime.lastHalZeroTimestampGeneration.store(0, std::memory_order_release);
}

} // namespace ASFW::Audio::DriverKit
