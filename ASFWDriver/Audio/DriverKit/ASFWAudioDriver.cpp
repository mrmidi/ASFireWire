//
// ASFWAudioDriver.cpp
// ASFWDriver
//
// AudioDriverKit driver implementation
// Uses shared memory queue for cross-process audio streaming to IT context
//

#include "ASFWAudioDriver.h"
#include "ASFWAudioNub.h"
#include "ASFWProtocolBooleanControl.h"
#include "Controls/AudioControlBuilder.hpp"
#include "Config/AudioDriverConfig.hpp"
#include "Runtime/AudioGraphBinding.hpp"
#include "Runtime/DirectAudioDebugSnapshot.hpp"
#include "Runtime/AudioTransportControlBlock.hpp"
#include "../../AudioEngine/Direct/FireWireAudioEngine.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Logging/LogConfig.hpp"
#include "../../AudioWire/AMDTP/PacketAssembler.hpp"
#include "../../AudioWire/AMDTP/TimingUtils.hpp"
#include "../../Isoch/Config/AudioTxProfiles.hpp"
#include "../../Common/DriverKitOwnership.hpp"

#include <DriverKit/DriverKit.h>
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/OSBoolean.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <AudioDriverKit/AudioDriverKit.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

// Report only hardware/presentation pipeline latency to HAL.
// Software queue/ring buffering should not be baked into device latency fields.
static constexpr uint32_t kReportedDeviceLatencyFrames = 24;  // ~0.5ms @ 48kHz
// 2A: Safety offset driven by TX buffer profile (data-driven from Phase 1 diagnostics)
static constexpr uint32_t kReportedSafetyOffsetFrames =
    ASFW::Isoch::Config::kTxBufferProfile.safetyOffsetFrames;

struct AudioDriverDeviceState {
    ASFWAudioNub* audioNub{nullptr};
    uint64_t guid{0};
    uint32_t vendorId{0};
    uint32_t modelId{0};
    char deviceName[128]{};
    uint32_t channelCount{0};
    uint32_t inputChannelCount{0};
    uint32_t outputChannelCount{0};
    double sampleRates[8]{};
    uint32_t sampleRateCount{0};
    double currentSampleRate{0};
    uint32_t streamModeRaw{0};
    bool hasPhantomOverride{false};
    uint32_t phantomSupportedMask{0};
    uint32_t phantomInitialMask{0};
    uint32_t boolControlCount{0};
    ASFW::Isoch::Audio::BoolControlSlot boolControls[ASFW::Isoch::Audio::kMaxBoolControls]{};

    char inputPlugName[64]{};
    char outputPlugName[64]{};
    char inputChannelNames[8][64]{};
    char outputChannelNames[8][64]{};
};

struct AudioDriverSharedMemoryState {
    OSSharedPtr<IOBufferMemoryDescriptor> txQueueMem;
    OSSharedPtr<IOMemoryMap> txQueueMap;
    uint64_t txQueueBytes{0};
    bool txQueueValid{false};

    OSSharedPtr<IOBufferMemoryDescriptor> sharedOutputBuffer;
    OSSharedPtr<IOMemoryMap> sharedOutputMap;
    uint64_t sharedOutputBytes{0};
    uint32_t zeroCopyFrameCapacity{0};
    bool zeroCopyEnabled{false};

    OSSharedPtr<IOBufferMemoryDescriptor> rxQueueMem;
    OSSharedPtr<IOMemoryMap> rxQueueMap;
    uint64_t rxQueueBytes{0};
    bool rxQueueValid{false};
};

// Runtime layout is intentionally organized around hot-path state ownership, not field packing.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct AudioDriverRuntimeState {
    OSSharedPtr<IOTimerDispatchSource> timestampTimer;
    OSSharedPtr<OSAction> timestampTimerAction;
    uint64_t hostTicksPerBuffer{0};
    uint64_t ztsPeriodTicks{0};
    std::atomic<uint64_t> ztsBaseHostTicks{0};
    std::atomic<uint64_t> ztsBaseSampleFrame{0};
    std::atomic<uint64_t> ztsTimerPublishes{0};
    std::atomic<bool> isRunning{false};

    uint64_t metricsLogCounter{0};
    ASFW::Encoding::PacketAssembler packetAssembler;
    bool rxStartupDrained{false};

    ASFW::Audio::Runtime::AudioTransportControlBlock directAudioControl;
    ASFW::Audio::Runtime::AudioGraphBinding directAudioGraph;
    ASFW::AudioEngine::Direct::FireWireAudioEngine directAudioEngine;
    ASFW::Audio::Runtime::DirectAudioDebugLogState directAudioDebugLog;
    std::atomic<bool> directAudioSkeletonBound{false};
    std::atomic<uint64_t> ioDebugCallbacks{0};
};

namespace {

constexpr uint64_t kNanosPerSecond = 1'000'000'000ULL;
constexpr uint64_t kZtsTimerLeewayTicks = 0;

[[nodiscard]] uint64_t AudioPeriodNanos(uint32_t frames, uint32_t sampleRateHz) noexcept {
    if (frames == 0 || sampleRateHz == 0) {
        return 0;
    }
    return (static_cast<uint64_t>(frames) * kNanosPerSecond) / sampleRateHz;
}

[[nodiscard]] uint64_t AudioFramesForElapsedTicks(uint64_t elapsedTicks,
                                                  uint32_t sampleRateHz) noexcept {
    if (elapsedTicks == 0 || sampleRateHz == 0) {
        return 0;
    }
    const uint64_t elapsedNanos = ASFW::Timing::hostTicksToNanos(elapsedTicks);
    return (elapsedNanos * static_cast<uint64_t>(sampleRateHz)) / kNanosPerSecond;
}

void StopZtsTimer(AudioDriverRuntimeState& runtime) noexcept {
    runtime.isRunning.store(false, std::memory_order_release);
    if (runtime.timestampTimer) {
        (void)runtime.timestampTimer->SetEnable(false);
    }
}

void ScheduleNextZtsTimer(AudioDriverRuntimeState& runtime, uint64_t nowTicks) noexcept {
    if (!runtime.timestampTimer || runtime.ztsPeriodTicks == 0) {
        return;
    }
    (void)runtime.timestampTimer->WakeAtTime(kIOTimerClockMachAbsoluteTime,
                                             nowTicks + runtime.ztsPeriodTicks,
                                             kZtsTimerLeewayTicks);
}

} // namespace

struct ASFWAudioDriver_IVars {
    OSSharedPtr<IODispatchQueue> workQueue;
    OSSharedPtr<IOUserAudioDevice> audioDevice;
    OSSharedPtr<IOUserAudioStream> inputStream;
    OSSharedPtr<IOUserAudioStream> outputStream;
    OSSharedPtr<IOMemoryDescriptor> inputBuffer;
    OSSharedPtr<IOMemoryDescriptor> outputBuffer;
    OSSharedPtr<IOMemoryDescriptor> controlBuffer;
    OSSharedPtr<IOMemoryMap> inputMap;
    OSSharedPtr<IOMemoryMap> outputMap;
    OSSharedPtr<IOMemoryMap> controlMap;

    AudioDriverDeviceState device;
    AudioDriverSharedMemoryState shared;
    AudioDriverRuntimeState runtime;
};

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
        0, // ioBufferFrameSize
        ASFW::Isoch::Config::kAudioIoPeriodFrames,
        0, // sampleDelta
        0, // regressionCount
        0, // frameSizeChanges
        true); // outputAvailable

    if (!ASFW::Audio::Runtime::ShouldLogDirectAudioDebugSnapshot(
            runtime.directAudioDebugLog,
            snapshot,
            ASFW::LogDetail::NowNs())) {
        return;
    }

    ASFW_LOG(DirectAudio,
             "ADK snapshot bound=%d inBase=0x%llx outBase=0x%llx inCap=%u outCap=%u inCh=%u outCh=%u beginRead=%llu writeEnd=%llu beginSample=%llu readEndFrame=%llu writeSample=%llu writeEndFrame=%llu beginFrames=%u writeFrames=%u ioFrames=%u expectedIoFrames=%u outputAvailable=%d txPackets=%llu txUnderruns=%llu txSilence=%llu",
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
             snapshot.directTxPackets,
             snapshot.directTxUnderruns,
             snapshot.directTxSilenceSubstitutions);
}

} // namespace ASFW::Audio::DriverKit::DirectDiagnostics

namespace {

[[nodiscard]] uint32_t FrameCapacityFromSegment(const IOAddressSegment& segment,
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

[[nodiscard]] ASFW::Audio::Runtime::AudioStreamMode DirectStreamModeFromRaw(uint32_t streamModeRaw) noexcept {
    return streamModeRaw == std::to_underlying(ASFW::Isoch::Audio::StreamMode::kBlocking)
         ? ASFW::Audio::Runtime::AudioStreamMode::kBlocking
         : ASFW::Audio::Runtime::AudioStreamMode::kNonBlocking;
}

[[nodiscard]] bool BindDirectAudioSkeleton(ASFWAudioDriver_IVars& ivars) noexcept {
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
    ivars.runtime.directAudioSkeletonBound.store(bound, std::memory_order_release);
    ASFW_LOG(DirectAudio,
             "ADK DBG BIND skeleton %s outBase=%p outFrames=%u outCh=%u inBase=%p inFrames=%u inCh=%u control=%p rate=%u",
             bound ? "bound" : "inactive",
             static_cast<const void*>(ivars.runtime.directAudioGraph.memory.outputBase),
             ivars.runtime.directAudioGraph.memory.outputFrameCapacity,
             ivars.runtime.directAudioGraph.memory.outputChannels,
             static_cast<void*>(ivars.runtime.directAudioGraph.memory.inputBase),
             ivars.runtime.directAudioGraph.memory.inputFrameCapacity,
             ivars.runtime.directAudioGraph.memory.inputChannels,
             static_cast<void*>(ivars.runtime.directAudioGraph.control),
             ivars.runtime.directAudioGraph.sampleRateHz);
    return bound;
}

void PublishDirectAudioBindingToNub(ASFWAudioDriver_IVars& ivars) noexcept {
    auto* nub = ivars.device.audioNub;
    const auto& graph = ivars.runtime.directAudioGraph;
    const bool validDuplex = graph.IsValid() && graph.HasInput() && graph.HasOutput();
    if (!nub || !ivars.runtime.directAudioSkeletonBound.load(std::memory_order_acquire) ||
        !validDuplex) {
        ASFW_LOG(DirectAudio,
                 "ADK DBG BIND provider_publish skipped nub=%p skeleton=%d valid=%d",
                 static_cast<void*>(nub),
                 ivars.runtime.directAudioSkeletonBound.load(std::memory_order_acquire),
                 validDuplex);
        return;
    }

    const uint64_t outputBytes =
        static_cast<uint64_t>(graph.memory.outputFrameCapacity) *
        static_cast<uint64_t>(graph.memory.outputChannels) *
        sizeof(int32_t);
    const uint64_t inputBytes =
        static_cast<uint64_t>(graph.memory.inputFrameCapacity) *
        static_cast<uint64_t>(graph.memory.inputChannels) *
        sizeof(int32_t);

    nub->SetDirectAudioBinding(graph.memory.outputBase,
                               outputBytes,
                               graph.memory.outputFrameCapacity,
                               graph.memory.outputChannels,
                               graph.memory.inputBase,
                               inputBytes,
                               graph.memory.inputFrameCapacity,
                               graph.memory.inputChannels,
                               graph.control,
                               graph.sampleRateHz,
                               graph.audioDevice);

    ASFW_LOG(DirectAudio,
             "ADK DBG BIND provider_publish raw_local_binding=1 outBase=%p outFrames=%u outCh=%u inBase=%p inFrames=%u inCh=%u control=%p audioDevice=%p rate=%u",
             static_cast<const void*>(graph.memory.outputBase),
             graph.memory.outputFrameCapacity,
             graph.memory.outputChannels,
             static_cast<void*>(graph.memory.inputBase),
             graph.memory.inputFrameCapacity,
             graph.memory.inputChannels,
             static_cast<void*>(graph.control),
             static_cast<void*>(graph.audioDevice),
             graph.sampleRateHz);
}

void ClearDirectAudioBindingFromNub(ASFWAudioDriver_IVars& ivars) noexcept {
    if (ivars.device.audioNub) {
        ivars.device.audioNub->ClearDirectAudioBinding();
    }
}

} // namespace

bool ASFWAudioDriver::init()
{
    bool result = super::init();
    if (!result) {
        ASFW_LOG(Audio, "ASFWAudioDriver: super::init() failed");
        return false;
    }
    
    ivars = IONewZero(ASFWAudioDriver_IVars, 1);
    if (!ivars) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to allocate ivars");
        return false;
    }
    
    ASFW::Isoch::Audio::ParsedAudioDriverConfig defaultConfig{};
    ASFW::Isoch::Audio::InitializeAudioDriverConfigDefaults(defaultConfig);

    ivars->device.audioNub = nullptr;
    ivars->device.guid = defaultConfig.guid;
    ivars->device.vendorId = defaultConfig.vendorId;
    ivars->device.modelId = defaultConfig.modelId;
    strlcpy(ivars->device.deviceName, defaultConfig.deviceName, sizeof(ivars->device.deviceName));
    ivars->device.channelCount = defaultConfig.channelCount;
    ivars->device.inputChannelCount = defaultConfig.inputChannelCount;
    ivars->device.outputChannelCount = defaultConfig.outputChannelCount;
    for (uint32_t index = 0; index < ASFW::Isoch::Audio::kMaxSampleRates; ++index) {
        ivars->device.sampleRates[index] = defaultConfig.sampleRates[index];
    }
    ivars->device.sampleRateCount = defaultConfig.sampleRateCount;
    ivars->device.currentSampleRate = defaultConfig.currentSampleRate;
    ivars->device.streamModeRaw = std::to_underlying(defaultConfig.streamMode);
    ivars->device.hasPhantomOverride = defaultConfig.hasPhantomOverride;
    ivars->device.phantomSupportedMask = defaultConfig.phantomSupportedMask;
    ivars->device.phantomInitialMask = defaultConfig.phantomInitialMask;
    ivars->device.boolControlCount = defaultConfig.boolControlCount;

    strlcpy(ivars->device.inputPlugName, defaultConfig.inputPlugName, sizeof(ivars->device.inputPlugName));
    strlcpy(ivars->device.outputPlugName, defaultConfig.outputPlugName, sizeof(ivars->device.outputPlugName));
    for (uint32_t index = 0; index < ASFW::Isoch::Audio::kMaxNamedChannels; ++index) {
        strlcpy(ivars->device.inputChannelNames[index],
                defaultConfig.inputChannelNames[index],
                sizeof(ivars->device.inputChannelNames[index]));
        strlcpy(ivars->device.outputChannelNames[index],
                defaultConfig.outputChannelNames[index],
                sizeof(ivars->device.outputChannelNames[index]));
    }
    
    ASFW_LOG(Audio, "ASFWAudioDriver: init() succeeded");
    return true;
}

void ASFWAudioDriver::free()
{
    ASFW_LOG(Audio, "ASFWAudioDriver: free()");

    if (ivars) {
        // Clean up timer resources first
        if (ivars->runtime.timestampTimer) {
            StopZtsTimer(ivars->runtime);
            ivars->runtime.timestampTimer.reset();
        }
        ivars->runtime.timestampTimerAction.reset();

        ivars->runtime.directAudioSkeletonBound.store(false, std::memory_order_release);
        ivars->runtime.directAudioEngine.Unbind();
        ivars->runtime.directAudioGraph = {};

        // Release shared RX queue resources
        ivars->shared.rxQueueValid = false;
        ivars->shared.rxQueueMap.reset();
        ivars->shared.rxQueueMem.reset();
        ivars->shared.rxQueueBytes = 0;

        // Release shared TX queue resources
        ivars->shared.txQueueValid = false;
        ivars->shared.txQueueMap.reset();
        ivars->shared.txQueueMem.reset();
        ivars->device.audioNub = nullptr;
        ivars->device.boolControlCount = 0;
        ASFW::Isoch::Audio::ResetBoolControlSlots(ivars->device.boolControls,
                                                  ASFW::Isoch::Audio::kMaxBoolControls);

        ivars->outputStream.reset();
        ivars->inputStream.reset();
        ivars->outputMap.reset();
        ivars->inputMap.reset();
        ivars->controlMap.reset();
        ivars->outputBuffer.reset();
        ivars->inputBuffer.reset();
        ivars->controlBuffer.reset();
        ivars->audioDevice.reset();
        ivars->workQueue.reset();
        IOSafeDeleteNULL(ivars, ASFWAudioDriver_IVars, 1);
    }

    super::free();
}

kern_return_t IMPL(ASFWAudioDriver, Start)
{
    kern_return_t error = kIOReturnSuccess;
    
    ASFW_LOG(Audio, "ASFWAudioDriver: Start() - provider is ASFWAudioNub");
    
    error = Start(provider, SUPERDISPATCH);
    if (error != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioDriver: super::Start() failed: %d", error);
        return error;
    }

    // Note: Cannot use OSDynamicCast across DriverKit process boundaries
    // We use a global registry instead, keyed by device GUID

    // Get work queue
    ivars->workQueue = GetWorkQueue();
    if (!ivars->workQueue) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to get work queue");
        return kIOReturnInvalid;
    }
    
    ivars->device.audioNub = reinterpret_cast<ASFWAudioNub*>(provider);
    ivars->device.boolControlCount = 0;

    ASFW::Isoch::Audio::ParsedAudioDriverConfig parsedConfig{};
    ASFW::Isoch::Audio::InitializeAudioDriverConfigDefaults(parsedConfig);

    // Read device info from nub properties
    OSDictionary* propsRaw = nullptr;
    if (provider->CopyProperties(&propsRaw) == kIOReturnSuccess && propsRaw) {
        OSSharedPtr<OSDictionary> props(propsRaw, OSNoRetain);
        ASFW::Isoch::Audio::ParseAudioDriverConfigFromProperties(props.get(), parsedConfig);
    } else {
        ASFW_LOG(Audio, "ASFWAudioDriver: Using default device configuration (no nub properties)");
    }

    ASFW::Isoch::Audio::BuildFallbackBoolControls(parsedConfig);
    ASFW::Isoch::Audio::ApplyBringupSingleFormatPolicy(parsedConfig);
    ASFW::Isoch::Audio::ClampAudioDriverChannels(parsedConfig, ASFW::Isoch::Config::kMaxPcmChannels);

    ivars->device.guid = parsedConfig.guid;
    ivars->device.vendorId = parsedConfig.vendorId;
    ivars->device.modelId = parsedConfig.modelId;
    ivars->device.channelCount = parsedConfig.channelCount;
    ivars->device.inputChannelCount = parsedConfig.inputChannelCount;
    ivars->device.outputChannelCount = parsedConfig.outputChannelCount;
    strlcpy(ivars->device.deviceName, parsedConfig.deviceName, sizeof(ivars->device.deviceName));
    strlcpy(ivars->device.inputPlugName, parsedConfig.inputPlugName, sizeof(ivars->device.inputPlugName));
    strlcpy(ivars->device.outputPlugName, parsedConfig.outputPlugName, sizeof(ivars->device.outputPlugName));
    ivars->device.sampleRateCount = parsedConfig.sampleRateCount;
    ivars->device.currentSampleRate = parsedConfig.currentSampleRate;
    ivars->device.streamModeRaw = std::to_underlying(parsedConfig.streamMode);
    ivars->device.hasPhantomOverride = parsedConfig.hasPhantomOverride;
    ivars->device.phantomSupportedMask = parsedConfig.phantomSupportedMask;
    ivars->device.phantomInitialMask = parsedConfig.phantomInitialMask;
    ivars->device.boolControlCount = parsedConfig.boolControlCount;

    for (uint32_t index = 0; index < ASFW::Isoch::Audio::kMaxSampleRates; ++index) {
        ivars->device.sampleRates[index] = parsedConfig.sampleRates[index];
    }
    for (uint32_t index = 0; index < ASFW::Isoch::Audio::kMaxNamedChannels; ++index) {
        strlcpy(ivars->device.inputChannelNames[index],
                parsedConfig.inputChannelNames[index],
            sizeof(ivars->device.inputChannelNames[index]));
        strlcpy(ivars->device.outputChannelNames[index],
                parsedConfig.outputChannelNames[index],
            sizeof(ivars->device.outputChannelNames[index]));
    }
        for (uint32_t index = 0; index < ivars->device.boolControlCount; ++index) {
        ivars->device.boolControls[index].descriptor = parsedConfig.boolControls[index];
        ivars->device.boolControls[index].valid = true;
    }

    ASFW_LOG(Audio,
             "ASFWAudioDriver: Device GUID=0x%016llx vendor=0x%06x model=0x%06x boolControls=%u",
             ivars->device.guid,
             ivars->device.vendorId,
             ivars->device.modelId,
             ivars->device.boolControlCount);

    ASFW_LOG(Audio, "ASFWAudioDriver: Read device name from nub: %{public}s", ivars->device.deviceName);
    ASFW_LOG(Audio,
             "ASFWAudioDriver: Read channel counts from nub: aggregate=%u input=%u output=%u",
             ivars->device.channelCount,
             ivars->device.inputChannelCount,
             ivars->device.outputChannelCount);
    ASFW_LOG(Audio, "ASFWAudioDriver: Read %u sample rates from nub", ivars->device.sampleRateCount);
    ASFW_LOG(Audio, "ASFWAudioDriver: Input plug name: %{public}s", ivars->device.inputPlugName);
    ASFW_LOG(Audio, "ASFWAudioDriver: Output plug name: %{public}s", ivars->device.outputPlugName);
    ASFW_LOG(Audio, "ASFWAudioDriver: Current sample rate from nub: %.0f Hz", ivars->device.currentSampleRate);
    ASFW_LOG(Audio, "ASFWAudioDriver: Stream mode from nub: %{public}s",
                 ivars->device.streamModeRaw == std::to_underlying(ASFW::Isoch::Audio::StreamMode::kBlocking)
                ? "blocking" : "non-blocking");

    // Temporary bring-up policy: expose exactly one format/rate in ADK.
    // Bring-up note: dynamic sample-rate advertisement is intentionally deferred.
    ASFW_LOG(Audio, "ASFWAudioDriver: Forcing single advertised format: 48kHz / 24-bit");

    ASFW_LOG(Audio,
             "ASFWAudioDriver: Effective runtime channels: input=%u output=%u aggregate=%u",
             ivars->device.inputChannelCount,
             ivars->device.outputChannelCount,
             ivars->device.channelCount);

    // Create audio device
    auto deviceUID = OSSharedPtr(OSString::withCString("ASFWAudioDevice"), OSNoRetain);
    auto modelUID = OSSharedPtr(OSString::withCString(ivars->device.deviceName), OSNoRetain);
    auto manufacturerUID = OSSharedPtr(OSString::withCString("ASFireWire"), OSNoRetain);
    
    ivars->audioDevice = IOUserAudioDevice::Create(this,
                                                    false,  // no prewarming
                                                    deviceUID.get(),
                                                    modelUID.get(),
                                                    manufacturerUID.get(),
                                                    ASFW::Isoch::Config::kAudioIoPeriodFrames);
    if (!ivars->audioDevice) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to create IOUserAudioDevice");
        return kIOReturnNoMemory;
    }
    ASFW_LOG(Audio,
             "ASFWAudioDriver: ADK object ids device=%u",
             ivars->audioDevice->GetObjectID());
    
    // Set up IO operation handler -- the real-time audio callback
    // IMPORTANT: This runs in a real-time context. No allocations, no locks, minimal logging.
    // Capture ivars pointer for use in block
    auto* driverIvars = ivars;
    error = ivars->audioDevice->SetIOOperationHandler(
        ^kern_return_t(IOUserAudioObjectID           objectID,
                       IOUserAudioIOOperation        operation,
                       uint32_t                      ioBufferFrameSize,
                       uint64_t                      sampleTime,
                       uint64_t                      hostTime)
    {
        if (!driverIvars) {
            return kIOReturnNotReady;
        }

        // Hot-path diagnostic counter, disabled for audio stability.
        // const uint64_t callbackIndex =
        //     driverIvars->runtime.ioDebugCallbacks.fetch_add(1, std::memory_order_relaxed) + 1;
        const bool running = driverIvars->runtime.isRunning.load(std::memory_order_acquire);
        const bool skeletonBound =
            driverIvars->runtime.directAudioSkeletonBound.load(std::memory_order_acquire);
        // Hot-path diagnostic, disabled for audio stability.
        // if (callbackIndex == 1 || (callbackIndex % 1024) == 0) {
        //     ASFW_LOG(DirectAudio,
        //              "ADK DBG IO cb=%llu op=%u running=%d skeleton=%d frames=%u sample=%llu host=%llu",
        //              callbackIndex,
        //              static_cast<uint32_t>(operation),
        //              running,
        //              skeletonBound,
        //              ioBufferFrameSize,
        //              sampleTime,
        //              hostTime);
        // }

        if (!running) {
            // Hot-path diagnostic, disabled for audio stability.
            // if (callbackIndex == 1 || (callbackIndex % 1024) == 0) {
            //     ASFW_LOG(DirectAudio,
            //              "ADK DBG IO not_running cb=%llu op=%u frames=%u sample=%llu",
            //              callbackIndex,
            //              static_cast<uint32_t>(operation),
            //              ioBufferFrameSize,
            //              sampleTime);
            // }
            return kIOReturnNotReady;
        }

        // Driver IO buffers are provisioned for Config::kAudioIoPeriodFrames frames.
        if (ioBufferFrameSize > ASFW::Isoch::Config::kAudioIoPeriodFrames) {
            // Hot-path diagnostic, disabled for audio stability.
            // ASFW_LOG(DirectAudio,
            //          "ADK DBG IO bad_frames cb=%llu op=%u frames=%u max=%u",
            //          callbackIndex,
            //          static_cast<uint32_t>(operation),
            //          ioBufferFrameSize,
            //          ASFW::Isoch::Config::kAudioIoPeriodFrames);
            return kIOReturnBadArgument;
        }
        
        if (skeletonBound) {
            auto* control = driverIvars->runtime.directAudioGraph.control;
            if (!control) {
                // Hot-path diagnostic, disabled for audio stability.
                // ASFW_LOG(DirectAudio,
                //          "ADK DBG IO no_control cb=%llu op=%u frames=%u",
                //          callbackIndex,
                //          static_cast<uint32_t>(operation),
                //          ioBufferFrameSize);
                return kIOReturnNotReady;
            }

            if (operation == IOUserAudioIOOperationBeginRead) {
                control->client.PublishBeginRead(sampleTime, hostTime, ioBufferFrameSize);
                control->counters.CountBeginRead();
            } else if (operation == IOUserAudioIOOperationWriteEnd) {
                control->client.PublishWriteEnd(sampleTime, hostTime, ioBufferFrameSize);
                control->counters.CountWriteEnd();
                // Hot-path diagnostic, disabled for audio stability.
                // const uint64_t writeCount =
                //     control->counters.ioWriteEndCount.load(std::memory_order_relaxed);
                // const uint64_t writtenEnd = control->client.OutputWrittenEndFrame();
                // if (writeCount == 1 || (writeCount % 1024) == 0) {
                //     ASFW_LOG(DirectAudio,
                //              "ADK DBG IO write_end count=%llu cb=%llu frames=%u sample=%llu writtenEnd=%llu",
                //              writeCount,
                //              callbackIndex,
                //              ioBufferFrameSize,
                //              sampleTime,
                //              writtenEnd);
                // }
            }
        }
        
        return kIOReturnSuccess;
    });
    if (error != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioDriver: SetIOOperationHandler failed: 0x%x", error);
        return error;
    }
    
    ASFW_LOG(Audio, "ASFWAudioDriver: IO operation handler installed");
    
    // Set device name
    auto name = OSSharedPtr(OSString::withCString(ivars->device.deviceName), OSNoRetain);
    ivars->audioDevice->SetName(name.get());
    
    // Set sample rates
    ivars->audioDevice->SetAvailableSampleRates(ivars->device.sampleRates, ivars->device.sampleRateCount);
    
    // Set initial sample rate to device's current rate (from active format)
    ivars->audioDevice->SetSampleRate(ivars->device.currentSampleRate);
    ASFW_LOG(Audio, "ASFWAudioDriver: Initial sample rate set to %.0f Hz", ivars->device.currentSampleRate);
    
    // Create stream formats - one for each supported sample rate
    // This populates the Format dropdown in Audio MIDI Setup
    // Using 24-bit audio as expected by FireWire hardware (packed in 32-bit container)
    IOUserAudioStreamBasicDescription inputFormats[8] = {};
    IOUserAudioStreamBasicDescription outputFormats[8] = {};
    uint32_t formatCount = ivars->device.sampleRateCount > 8 ? 8 : ivars->device.sampleRateCount;
    
    for (uint32_t i = 0; i < formatCount; i++) {
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
        auto fillFormat = [](IOUserAudioStreamBasicDescription& fmt, double sampleRate, uint32_t channels) {
            fmt.mSampleRate = sampleRate;
            fmt.mFormatID = IOUserAudioFormatID::LinearPCM;
            fmt.mFormatFlags = static_cast<IOUserAudioFormatFlags>(
            static_cast<uint32_t>(IOUserAudioFormatFlags::FormatFlagIsSignedInteger) |
            static_cast<uint32_t>(IOUserAudioFormatFlags::FormatFlagsNativeEndian));
            // 24-bit audio in 32-bit containers (standard for pro audio)
            fmt.mBytesPerPacket = sizeof(int32_t) * channels;
            fmt.mFramesPerPacket = 1;
            fmt.mBytesPerFrame = sizeof(int32_t) * channels;
            fmt.mChannelsPerFrame = channels;
            fmt.mBitsPerChannel = 24;
        };

        fillFormat(inputFormats[i], ivars->device.sampleRates[i], ivars->device.inputChannelCount);
        fillFormat(outputFormats[i], ivars->device.sampleRates[i], ivars->device.outputChannelCount);
    }
    
    ASFW_LOG(Audio,
             "ASFWAudioDriver: Created %u stream formats (24-bit) in=%u out=%u channels",
             formatCount,
             ivars->device.inputChannelCount,
             ivars->device.outputChannelCount);
    
    IOMemoryDescriptor* rawOutputMemory = nullptr;
    IOMemoryDescriptor* rawInputMemory = nullptr;
    IOMemoryDescriptor* rawControlMemory = nullptr;
    uint32_t sharedOutputFrames = 0;
    uint32_t sharedOutputChannels = 0;
    uint32_t sharedInputFrames = 0;
    uint32_t sharedInputChannels = 0;
    uint32_t sharedSampleRateHz = 0;
    uint64_t sharedGeneration = 0;

    ASFW_LOG(DirectAudio,
             "ADK DBG MEM request provider=%p guid=0x%016llx expectedInCh=%u expectedOutCh=%u rate=%.0f",
             static_cast<void*>(provider),
             ivars->device.guid,
             ivars->device.inputChannelCount,
             ivars->device.outputChannelCount,
             ivars->device.currentSampleRate);

    if (!ivars->device.audioNub) {
        ASFW_LOG(DirectAudio, "ADK DBG MEM request failed no_audio_nub");
        return kIOReturnNotReady;
    }

    error = ivars->device.audioNub->CopyDirectAudioMemory(&rawOutputMemory,
                                                          &rawInputMemory,
                                                          &rawControlMemory,
                                                          &sharedOutputFrames,
                                                          &sharedOutputChannels,
                                                          &sharedInputFrames,
                                                          &sharedInputChannels,
                                                          &sharedSampleRateHz,
                                                          &sharedGeneration);
    if (error != kIOReturnSuccess || !rawOutputMemory || !rawInputMemory || !rawControlMemory) {
        ASFW_LOG(DirectAudio,
                 "ADK DBG MEM request failed kr=0x%x outMem=%p inMem=%p controlMem=%p",
                 error,
                 static_cast<void*>(rawOutputMemory),
                 static_cast<void*>(rawInputMemory),
                 static_cast<void*>(rawControlMemory));
        if (rawOutputMemory) { rawOutputMemory->release(); }
        if (rawInputMemory) { rawInputMemory->release(); }
        if (rawControlMemory) { rawControlMemory->release(); }
        return (error == kIOReturnSuccess) ? kIOReturnNoMemory : error;
    }

    ivars->outputBuffer = ASFW::Common::AdoptRetained(rawOutputMemory);
    ivars->inputBuffer = ASFW::Common::AdoptRetained(rawInputMemory);
    ivars->controlBuffer = ASFW::Common::AdoptRetained(rawControlMemory);

    if (sharedOutputChannels != ivars->device.outputChannelCount ||
        sharedInputChannels != ivars->device.inputChannelCount ||
        sharedSampleRateHz != static_cast<uint32_t>(ivars->device.currentSampleRate)) {
        ASFW_LOG(DirectAudio,
                 "ADK DBG MEM metadata mismatch gen=%llu sharedOutCh=%u localOutCh=%u sharedInCh=%u localInCh=%u sharedRate=%u localRate=%u",
                 sharedGeneration,
                 sharedOutputChannels,
                 ivars->device.outputChannelCount,
                 sharedInputChannels,
                 ivars->device.inputChannelCount,
                 sharedSampleRateHz,
                 static_cast<uint32_t>(ivars->device.currentSampleRate));
    }

    error = ASFW::Common::CreateSharedMapping(ivars->outputBuffer, ivars->outputMap);
    if (error != kIOReturnSuccess) {
        ASFW_LOG(DirectAudio, "ADK DBG MEM map output failed kr=0x%x", error);
        return error;
    }
    error = ASFW::Common::CreateSharedMapping(ivars->inputBuffer, ivars->inputMap);
    if (error != kIOReturnSuccess) {
        ASFW_LOG(DirectAudio, "ADK DBG MEM map input failed kr=0x%x", error);
        return error;
    }
    error = ASFW::Common::CreateSharedMapping(ivars->controlBuffer, ivars->controlMap);
    if (error != kIOReturnSuccess) {
        ASFW_LOG(DirectAudio, "ADK DBG MEM map control failed kr=0x%x", error);
        return error;
    }

    ASFW_LOG(DirectAudio,
             "ADK DBG MEM mapped gen=%llu outBase=%p outLen=%llu outFrames=%u outCh=%u inBase=%p inLen=%llu inFrames=%u inCh=%u control=%p controlLen=%llu rate=%u",
             sharedGeneration,
             reinterpret_cast<void*>(static_cast<uintptr_t>(ivars->outputMap->GetAddress())),
             ivars->outputMap->GetLength(),
             sharedOutputFrames,
             sharedOutputChannels,
             reinterpret_cast<void*>(static_cast<uintptr_t>(ivars->inputMap->GetAddress())),
             ivars->inputMap->GetLength(),
             sharedInputFrames,
             sharedInputChannels,
             reinterpret_cast<void*>(static_cast<uintptr_t>(ivars->controlMap->GetAddress())),
             ivars->controlMap->GetLength(),
             sharedSampleRateHz);
    
    ivars->inputStream = IOUserAudioStream::Create(this,
                                                    IOUserAudioStreamDirection::Input,
                                                    ivars->inputBuffer.get());
    if (!ivars->inputStream) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to create input stream");
        return kIOReturnNoMemory;
    }
    ASFW_LOG(Audio,
             "ASFWAudioDriver: ADK object ids inputStream=%u owner=%u",
             ivars->inputStream->GetObjectID(),
             ivars->inputStream->GetOwnerObjectID());
    
    // Use plug name for stream name (e.g., "Analog In")
    auto inputName = OSSharedPtr(OSString::withCString(ivars->device.inputPlugName), OSNoRetain);
    ivars->inputStream->SetName(inputName.get());
    ivars->inputStream->SetAvailableStreamFormats(inputFormats, formatCount);
    ivars->inputStream->SetCurrentStreamFormat(&inputFormats[0]);  // Initial format
    
    // Create output stream with the appropriate buffer (shared or local)
    ivars->outputStream = IOUserAudioStream::Create(this,
                                                     IOUserAudioStreamDirection::Output,
                                                     ivars->outputBuffer.get());
    if (!ivars->outputStream) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to create output stream");
        return kIOReturnNoMemory;
    }
    ASFW_LOG(Audio,
             "ASFWAudioDriver: ADK object ids outputStream=%u owner=%u",
             ivars->outputStream->GetObjectID(),
             ivars->outputStream->GetOwnerObjectID());
    
    // Use plug name for stream name (e.g., "Analog Out")
    auto outputName = OSSharedPtr(OSString::withCString(ivars->device.outputPlugName), OSNoRetain);
    ivars->outputStream->SetName(outputName.get());
    ivars->outputStream->SetAvailableStreamFormats(outputFormats, formatCount);
    ivars->outputStream->SetCurrentStreamFormat(&outputFormats[0]);  // Initial format

    const bool directAudioSkeletonBound = BindDirectAudioSkeleton(*ivars);
    ASFW_LOG(Audio,
             "ASFWAudioDriver: Direct audio skeleton %{public}s",
             directAudioSkeletonBound ? "bound" : "inactive");
    (void)sharedGeneration;
    PublishDirectAudioBindingToNub(*ivars);

    // Stream-level latency (no additional latency beyond device-level)
    ivars->outputStream->SetLatency(0);
    ivars->inputStream->SetLatency(0);

    // Add streams to device
    error = ivars->audioDevice->AddStream(ivars->inputStream.get());
    if (error != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to add input stream: %d", error);
        return error;
    }
    
    error = ivars->audioDevice->AddStream(ivars->outputStream.get());
    if (error != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to add output stream: %d", error);
        return error;
    }
    
    // Set channel names on the device (elements are 1-based)
    // This gives us "Analog Out 1", "Analog Out 2" etc. in Audio MIDI Setup
    for (uint32_t ch = 1; ch <= ivars->device.outputChannelCount && ch <= 8; ch++) {
        auto outChName = OSSharedPtr(OSString::withCString(ivars->device.outputChannelNames[ch - 1]), OSNoRetain);
        ivars->audioDevice->SetElementName(ch, IOUserAudioObjectPropertyScope::Output, outChName.get());
    }
    for (uint32_t ch = 1; ch <= ivars->device.inputChannelCount && ch <= 8; ch++) {
        auto inChName = OSSharedPtr(OSString::withCString(ivars->device.inputChannelNames[ch - 1]), OSNoRetain);
        ivars->audioDevice->SetElementName(ch, IOUserAudioObjectPropertyScope::Input, inChName.get());
    }

    ASFW::Isoch::Audio::AddBooleanControlsToDevice(*this,
                                                   *ivars->audioDevice,
                                                   ivars->device.boolControls,
                                                   ivars->device.boolControlCount);

    // Explicitly keep host control-state restore enabled.
    ivars->audioDevice->SetWantsControlsRestored(true);
    
    // Set transport type on BOTH driver and device
    // IOUserAudioDevice inherits SetTransportType from IOUserAudioClockDevice
    SetTransportType(IOUserAudioTransportType::FireWire);  // On driver
    ivars->audioDevice->SetTransportType(IOUserAudioTransportType::FireWire);  // On device

    // Clock properties for CoreAudio HAL
    // Cycle-time-based timestamps are hardware-backed; ADK's 12-point moving
    // window average handles jitter smoothing, so we don't need custom EMA.
    ivars->audioDevice->SetClockAlgorithm(IOUserAudioClockAlgorithm::TwelvePtMovingWindowAverage);
    ivars->audioDevice->SetClockIsStable(true);   // Cycle-timer-derived, hardware-backed
    ivars->audioDevice->SetClockDomain(1);        // Separate domain from built-in audio

    // Keep HAL-facing latency/safety focused on physical pipeline delay.
    // Transport queue/ring buffering remains managed internally and should not
    // inflate kAudioDevicePropertyLatency compensation.
    ivars->audioDevice->SetOutputLatency(kReportedDeviceLatencyFrames);
    ivars->audioDevice->SetInputLatency(kReportedDeviceLatencyFrames);
    ivars->audioDevice->SetOutputSafetyOffset(kReportedSafetyOffsetFrames);
    ivars->audioDevice->SetInputSafetyOffset(kReportedSafetyOffsetFrames);
    ASFW_LOG(Audio, "ASFWAudioDriver: Reported HAL latency out/in=%u, safety out/in=%u frames",
             kReportedDeviceLatencyFrames, kReportedSafetyOffsetFrames);

    // The shared output ring is now several IO periods deep (kAudioOutputRingFrames),
    // so the buffer size no longer implies the timestamp cadence. Tell the HAL the
    // zero-timestamp period explicitly (= one IO period) so its sample<->ring mapping
    // stays unambiguous. Set here during initial device configuration, before IO
    // starts. (ADK: SetZeroTimeStampPeriod expects a config-change context.)
    {
        const kern_return_t ztsKr =
            ivars->audioDevice->SetZeroTimeStampPeriod(ASFW::Isoch::Config::kAudioIoPeriodFrames);
        ASFW_LOG(Audio, "ASFWAudioDriver: SetZeroTimeStampPeriod(%u) kr=0x%x",
                 ASFW::Isoch::Config::kAudioIoPeriodFrames, ztsKr);
    }

    // Add device to driver
    error = AddObject(ivars->audioDevice.get());
    if (error != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to add device: %d", error);
        return error;
    }
    
    // Register service
    error = RegisterService();
    if (error != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioDriver: RegisterService() failed: %d", error);
        return error;
    }
    
    // Create timer for timestamp generation
    IOTimerDispatchSource* timerSource = nullptr;
    error = IOTimerDispatchSource::Create(ivars->workQueue.get(), &timerSource);
    if (error != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to create timestamp timer: %d", error);
        return error;
    }
    ivars->runtime.timestampTimer = OSSharedPtr(timerSource, OSNoRetain);
    
    // Create timer action (DriverKit generates CreateActionZtsTimerOccurred from .iig)
    OSAction* timerAction = nullptr;
    error = CreateActionZtsTimerOccurred(sizeof(void*), &timerAction);
    if (error != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to create timer action: %d", error);
        return error;
    }
    ivars->runtime.timestampTimerAction = OSSharedPtr(timerAction, OSNoRetain);
    ivars->runtime.timestampTimer->SetHandler(ivars->runtime.timestampTimerAction.get());
    
    ASFW_LOG(Audio,
             "✅ ASFWAudioDriver: Started - device '%{public}s' (in=%u out=%u aggregate=%u)",
             ivars->device.deviceName,
             ivars->device.inputChannelCount,
             ivars->device.outputChannelCount,
             ivars->device.channelCount);

    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWAudioDriver, Stop)
{
    ASFW_LOG(Audio, "ASFWAudioDriver: Stop()");
    
    if (ivars) {
        // Safe teardown: synchronously stop audio streaming so that the isoch contexts
        // disarm and stop accessing our direct memory views/control blocks.
        if (ivars->device.audioNub) {
            kern_return_t stopKr = ivars->device.audioNub->StopAudioStreaming();
            if (stopKr != kIOReturnSuccess) {
                ASFW_LOG(Audio, "ASFWAudioDriver: StopAudioStreaming failed in Stop(): 0x%x", stopKr);
            }
            ClearDirectAudioBindingFromNub(*ivars);
        }
        ivars->device.audioNub = nullptr;
    }

    if (ivars && ivars->audioDevice) {
        RemoveObject(ivars->audioDevice.get());
    }
    
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t IMPL(ASFWAudioDriver, NewUserClient)
{
    ASFW_LOG(Audio, "ASFWAudioDriver: NewUserClient(type=%u)", in_type);
    
    // Let superclass handle HAL user client
    if (in_type == kIOUserAudioDriverUserClientType) {
        return super::NewUserClient(in_type, out_user_client, SUPERDISPATCH);
    }
    
    return kIOReturnBadArgument;
}

kern_return_t ASFWAudioDriver::StartDevice(IOUserAudioObjectID in_object_id,
                                            IOUserAudioStartStopFlags in_flags)
{
    if (!ivars || !ivars->audioDevice) {
        ASFW_LOG(Audio, "ASFWAudioDriver: StartDevice failed - not initialized");
        return kIOReturnNotReady;
    }

    ASFW_LOG(Audio,
             "ASFWAudioDriver: StartDevice(id=%u flags=0x%llx deviceId=%u inStreamId=%u outStreamId=%u)",
             in_object_id,
             static_cast<uint64_t>(in_flags),
             ivars->audioDevice->GetObjectID(),
             ivars->inputStream ? ivars->inputStream->GetObjectID() : 0,
             ivars->outputStream ? ivars->outputStream->GetObjectID() : 0);

    ivars->runtime.ioDebugCallbacks.store(0, std::memory_order_relaxed);
    ivars->runtime.isRunning.store(true, std::memory_order_release);
    ASFW_LOG(DirectAudio, "ADK DBG IO running=1 before StartAudioStreaming");

    const kern_return_t superStartKr = super::StartDevice(in_object_id, in_flags);
    if (superStartKr != kIOReturnSuccess) {
        ivars->runtime.isRunning.store(false, std::memory_order_release);
        ASFW_LOG(Audio, "ASFWAudioDriver: super::StartDevice failed: 0x%x", superStartKr);
        return superStartKr;
    }
    ASFW_LOG(DirectAudio, "ADK DBG IO super StartDevice ok id=%u", in_object_id);

    if (ivars->device.audioNub) {
        const kern_return_t startKr = ivars->device.audioNub->StartAudioStreaming();
        if (startKr != kIOReturnSuccess) {
            ivars->runtime.isRunning.store(false, std::memory_order_release);
            (void)super::StopDevice(in_object_id, in_flags);
            ASFW_LOG(Audio, "ASFWAudioDriver: StartAudioStreaming failed: 0x%x", startKr);
            return startKr;
        }
    }

    const uint32_t sampleRateHz = static_cast<uint32_t>(ivars->device.currentSampleRate);
    const uint64_t periodNanos = AudioPeriodNanos(ASFW::Isoch::Config::kAudioIoPeriodFrames,
                                                  sampleRateHz);
    (void)ASFW::Timing::initializeHostTimebase();
    ivars->runtime.ztsPeriodTicks = ASFW::Timing::nanosToHostTicks(periodNanos);
    const uint64_t nowTicks = mach_absolute_time();
    ivars->runtime.ztsBaseHostTicks.store(nowTicks, std::memory_order_release);
    ivars->runtime.ztsBaseSampleFrame.store(0, std::memory_order_release);
    ivars->runtime.ztsTimerPublishes.store(0, std::memory_order_release);

    if (ivars->audioDevice) {
        ivars->audioDevice->UpdateCurrentZeroTimestamp(0, nowTicks);
    }
    if (ivars->runtime.directAudioGraph.control) {
        ivars->runtime.directAudioGraph.control->device.Publish(
            0,
            nowTicks,
            sampleRateHz == 0 ? 0 : static_cast<uint32_t>((kNanosPerSecond << 8) / sampleRateHz));
        ivars->runtime.directAudioGraph.control->counters.CountTimerZtsPublished();
    }
    ASFW_LOG(DirectAudio,
             "ZTS publish source=timer count=1 sample=0 host=%llu periodTicks=%llu rate=%u",
             nowTicks,
             ivars->runtime.ztsPeriodTicks,
             sampleRateHz);
    ivars->runtime.ztsTimerPublishes.store(1, std::memory_order_release);

    if (ivars->runtime.timestampTimer && ivars->runtime.ztsPeriodTicks != 0) {
        const kern_return_t enableKr = ivars->runtime.timestampTimer->SetEnable(true);
        if (enableKr != kIOReturnSuccess) {
            ASFW_LOG(Audio, "ASFWAudioDriver: ZTS timer enable failed: 0x%x", enableKr);
        } else {
            ScheduleNextZtsTimer(ivars->runtime, nowTicks);
        }
    } else {
        ASFW_LOG(Audio,
                 "ASFWAudioDriver: ZTS timer unavailable timer=%p periodTicks=%llu",
                 ivars->runtime.timestampTimer.get(),
                 ivars->runtime.ztsPeriodTicks);
    }

    const auto* control = ivars->runtime.directAudioGraph.control;
    ASFW_LOG(DirectAudio,
             "ADK DBG DUPLEX ready guid=0x%016llx rxStarted=1 txStarted=1 bindValid=%d hasIn=%d hasOut=%d zts=%llu writeEnd=%llu",
             ivars->device.guid,
             ivars->runtime.directAudioSkeletonBound.load(std::memory_order_acquire),
             ivars->runtime.directAudioGraph.HasInput(),
             ivars->runtime.directAudioGraph.HasOutput(),
             control ? control->counters.ztsPublished.load(std::memory_order_relaxed) : 0,
             control ? control->counters.ioWriteEndCount.load(std::memory_order_relaxed) : 0);

    ASFW_LOG(Audio, "ASFWAudioDriver: Device started");
    
    return kIOReturnSuccess;
}

kern_return_t ASFWAudioDriver::StopDevice(IOUserAudioObjectID in_object_id,
                                           IOUserAudioStartStopFlags in_flags)
{
    ASFW_LOG(Audio, "ASFWAudioDriver: StopDevice(id=%u)", in_object_id);
    
    if (ivars) {
        StopZtsTimer(ivars->runtime);
    }

    if (ivars && ivars->device.audioNub) {
        const kern_return_t stopKr = ivars->device.audioNub->StopAudioStreaming();
        if (stopKr != kIOReturnSuccess) {
            ASFW_LOG(Audio, "ASFWAudioDriver: StopAudioStreaming failed: 0x%x", stopKr);
        }
        ClearDirectAudioBindingFromNub(*ivars);
    }

    const kern_return_t superStopKr = super::StopDevice(in_object_id, in_flags);
    if (superStopKr != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioDriver: super::StopDevice failed: 0x%x", superStopKr);
    }

    return superStopKr;
}

kern_return_t ASFWAudioDriver::ApplyProtocolBooleanControl(uint32_t classIdFourCC,
                                                           uint32_t element,
                                                           bool value)
{
    if (!ivars || !ivars->device.audioNub) {
        return kIOReturnNotReady;
    }
    return ivars->device.audioNub->SetProtocolBooleanControl(classIdFourCC, element, value);
}

kern_return_t ASFWAudioDriver::ReadProtocolBooleanControl(uint32_t classIdFourCC,
                                                          uint32_t element,
                                                          bool* outValue)
{
    if (!outValue) {
        return kIOReturnBadArgument;
    }
    if (!ivars || !ivars->device.audioNub) {
        return kIOReturnNotReady;
    }
    return ivars->device.audioNub->GetProtocolBooleanControl(classIdFourCC, element, outValue);
}

void ASFWAudioDriver::ZtsTimerOccurred_Impl([[maybe_unused]] OSAction* action, [[maybe_unused]] uint64_t time)
{
    if (!ivars || !ivars->runtime.isRunning.load(std::memory_order_acquire)) {
        return;
    }

    auto* control = ivars->runtime.directAudioGraph.control;
    if (control) {
        const uint64_t rxZtsCount =
            control->counters.ztsRxPublished.load(std::memory_order_acquire);
        if (rxZtsCount > 0) {
            const uint64_t rxAdkCount =
                control->counters.ztsRxAdkPublished.load(std::memory_order_acquire);
            const uint64_t rxSample =
                control->device.sampleFrame.load(std::memory_order_acquire);
            const uint64_t rxHost =
                control->device.hostTicks.load(std::memory_order_acquire);
            if (rxAdkCount > 0) {
                if (ivars->runtime.timestampTimer) {
                    (void)ivars->runtime.timestampTimer->SetEnable(false);
                }
                ASFW_LOG(DirectAudio,
                         "ZTS timer demoted source=rx rxCount=%llu rxAdk=%llu timerCount=%llu deviceGen=%llu sample=%llu host=%llu",
                         rxZtsCount,
                         rxAdkCount,
                         control->counters.ztsTimerPublished.load(std::memory_order_relaxed),
                         control->device.generation.load(std::memory_order_acquire),
                         rxSample,
                         rxHost);
                return;
            }

            if (ivars->audioDevice && rxHost != 0) {
                ivars->audioDevice->UpdateCurrentZeroTimestamp(rxSample, rxHost);
            }
            const uint64_t bridgeCount =
                ivars->runtime.ztsTimerPublishes.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (bridgeCount <= 8 || (bridgeCount % 1024) == 0) {
                ASFW_LOG(DirectAudio,
                         "ZTS timer bridge source=rx rxCount=%llu rxAdk=%llu timerCount=%llu sample=%llu host=%llu",
                         rxZtsCount,
                         rxAdkCount,
                         control->counters.ztsTimerPublished.load(std::memory_order_relaxed),
                         rxSample,
                         rxHost);
            }
            ScheduleNextZtsTimer(ivars->runtime, mach_absolute_time());
            return;
        }
    }

    const uint32_t sampleRateHz = static_cast<uint32_t>(ivars->device.currentSampleRate);
    const uint64_t nowTicks = mach_absolute_time();
    const uint64_t baseTicks = ivars->runtime.ztsBaseHostTicks.load(std::memory_order_acquire);
    const uint64_t baseSample = ivars->runtime.ztsBaseSampleFrame.load(std::memory_order_acquire);
    const uint64_t elapsedTicks = (nowTicks >= baseTicks) ? (nowTicks - baseTicks) : 0;
    const uint64_t sampleFrame = baseSample + AudioFramesForElapsedTicks(elapsedTicks, sampleRateHz);
    const uint32_t hostNanosPerSampleQ8 =
        sampleRateHz == 0 ? 0 : static_cast<uint32_t>((kNanosPerSecond << 8) / sampleRateHz);

    if (ivars->audioDevice) {
        ivars->audioDevice->UpdateCurrentZeroTimestamp(sampleFrame, nowTicks);
    }

    if (control) {
        control->device.Publish(sampleFrame, nowTicks, hostNanosPerSampleQ8);
        control->counters.CountTimerZtsPublished();
    }

    // Hot-path timer diagnostic, disabled for audio stability.
    // const uint64_t count =
    //     ivars->runtime.ztsTimerPublishes.fetch_add(1, std::memory_order_acq_rel) + 1;
    // if (count == 1 || (count % 1000) == 0) {
    //     ASFW_LOG(DirectAudio,
    //              "ZTS publish source=timer count=%llu sample=%llu host=%llu periodTicks=%llu rate=%u",
    //              count,
    //              sampleFrame,
    //              nowTicks,
    //              ivars->runtime.ztsPeriodTicks,
    //              sampleRateHz);
    // }

    ScheduleNextZtsTimer(ivars->runtime, nowTicks);
}
