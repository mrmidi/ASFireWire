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

bool BindDirectAudioSkeleton(ASFWAudioDriver_IVars& ivars,
                             DirectAudioMemoryGeometry physicalGeometry) noexcept {
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
        FrameCapacityFromSegment(inputSegment, physicalGeometry.inputChannels);
    const uint32_t outputFrameCapacity =
        FrameCapacityFromSegment(outputSegment, physicalGeometry.outputChannels);

    // Resolve the device profile to determine the host-to-device wire format.
    // Standard DICE fallback profiles use AM824 sub-frame formatting, while
    // Focusrite Saffire playback uses sign-extended 24-in-32 big-endian formatting.
    ASFW::Audio::Runtime::AudioWireFormat wireFormat = ASFW::Audio::Runtime::AudioWireFormat::kAM824;
    if (const auto* profile = ASFW::Isoch::Audio::AudioProfileRegistry::FindProfile(
            ivars.device.vendorId, ivars.device.modelId, ivars.device.guid,
            ivars.device.profileBuilderId)) {
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

    ivars.runtime.lastHalZeroTimestampGeneration.store(0, std::memory_order_release);
    ivars.runtime.lastHalZeroTimestampSampleFrame.store(0, std::memory_order_release);
    ivars.runtime.lastHalZeroTimestampHostTicks.store(0, std::memory_order_release);
    ivars.runtime.directAudioSkeletonBound.store(true, std::memory_order_release);
    ASFW_LOG(DirectAudio,
             "ADK DBG BIND skeleton %{public}s outBase=%p outFrames=%u outCh=%u inBase=%p inFrames=%u inCh=%u control=%p audioDevice=%p rate=%u",
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
