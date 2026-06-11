//
// ASFWAudioDriverGraph.cpp
// ASFWDriver
//
// ADK graph construction/teardown for ASFWAudioDriver.
//
#include "ASFWAudioDriverPrivate.hpp"
#include "Config/AudioProfileRegistry.hpp"
#include "../Config/TimingCursorPolicy.hpp"
#include "../../Common/TimingUtils.hpp"
#include "../../Common/DriverKitOwnership.hpp"
#include "../../Shared/Isoch/IsochAudioTransport.hpp"
#include "../../Audio/Wire/AMDTP/AmdtpRateGeometry.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSString.h>

#include <algorithm>
#include <cstring>
#include <type_traits>
#include <utility>

namespace ASFW::Audio::DriverKit {
namespace {

void CopyParsedConfigToDeviceState(const ASFW::Isoch::Audio::ParsedAudioDriverConfig& parsedConfig,
                                   AudioDriverDeviceState& device) noexcept {
    device.guid = parsedConfig.guid;
    device.vendorId = parsedConfig.vendorId;
    device.modelId = parsedConfig.modelId;
    device.channelCount = parsedConfig.channelCount;
    device.inputChannelCount = parsedConfig.inputChannelCount;
    device.outputChannelCount = parsedConfig.outputChannelCount;
    strlcpy(device.deviceName, parsedConfig.deviceName, sizeof(device.deviceName));
    strlcpy(device.inputPlugName, parsedConfig.inputPlugName, sizeof(device.inputPlugName));
    strlcpy(device.outputPlugName, parsedConfig.outputPlugName, sizeof(device.outputPlugName));
    device.sampleRateCount = parsedConfig.sampleRateCount;
    device.currentSampleRate = parsedConfig.currentSampleRate;
    device.streamModeRaw = std::to_underlying(parsedConfig.streamMode);
    device.hasPhantomOverride = parsedConfig.hasPhantomOverride;
    device.phantomSupportedMask = parsedConfig.phantomSupportedMask;
    device.phantomInitialMask = parsedConfig.phantomInitialMask;
    device.boolControlCount = parsedConfig.boolControlCount;

    for (uint32_t index = 0; index < ASFW::Isoch::Audio::kMaxSampleRates; ++index) {
        device.sampleRates[index] = parsedConfig.sampleRates[index];
    }
    for (uint32_t index = 0; index < ASFW::Isoch::Audio::kMaxNamedChannels; ++index) {
        strlcpy(device.inputChannelNames[index],
                parsedConfig.inputChannelNames[index],
                sizeof(device.inputChannelNames[index]));
        strlcpy(device.outputChannelNames[index],
                parsedConfig.outputChannelNames[index],
                sizeof(device.outputChannelNames[index]));
    }
    ASFW::Isoch::Audio::ResetBoolControlSlots(device.boolControls,
                                              ASFW::Isoch::Audio::kMaxBoolControls);
    for (uint32_t index = 0; index < device.boolControlCount; ++index) {
        device.boolControls[index].descriptor = parsedConfig.boolControls[index];
        device.boolControls[index].valid = true;
    }
}

[[nodiscard]] kern_return_t ValidateDeviceStateForGraph(const AudioDriverDeviceState& device) noexcept {
    if (device.guid == 0 ||
        device.inputChannelCount == 0 ||
        device.outputChannelCount == 0 ||
        device.sampleRateCount == 0 ||
        device.currentSampleRate <= 0.0) {
        ASFW_LOG(Audio,
                 "ASFWAudioDriver: invalid audio config guid=0x%016llx in=%u out=%u rates=%u currentRate=%.0f",
                 device.guid,
                 device.inputChannelCount,
                 device.outputChannelCount,
                 device.sampleRateCount,
                 device.currentSampleRate);
        return kIOReturnBadArgument;
    }
    return kIOReturnSuccess;
}

void FillPcm24Format(IOUserAudioStreamBasicDescription& fmt,
                     double sampleRate,
                     uint32_t channels) noexcept {
    fmt.mSampleRate = sampleRate;
    fmt.mFormatID = IOUserAudioFormatID::LinearPCM;
    fmt.mFormatFlags = static_cast<IOUserAudioFormatFlags>(
        static_cast<uint32_t>(IOUserAudioFormatFlags::FormatFlagIsSignedInteger) |
        static_cast<uint32_t>(IOUserAudioFormatFlags::FormatFlagsNativeEndian));
    fmt.mBytesPerPacket = sizeof(int32_t) * channels;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = sizeof(int32_t) * channels;
    fmt.mChannelsPerFrame = channels;
    fmt.mBitsPerChannel = 24;
}

} // namespace

void ResetDeviceStateFromDefaultConfig(ASFWAudioDriver_IVars& ivars) noexcept {
    ASFW::Isoch::Audio::ParsedAudioDriverConfig defaultConfig{};
    ASFW::Isoch::Audio::InitializeAudioDriverConfigDefaults(defaultConfig);
    ivars.device.audioNub = nullptr;
    CopyParsedConfigToDeviceState(defaultConfig, ivars.device);
}

kern_return_t BuildAudioGraph(ASFWAudioDriver& driver,
                              IOService* provider,
                              ASFWAudioDriver_IVars& ivars,
                              AudioGraphStartState& state) noexcept {
    if (!provider) {
        ASFW_LOG(Audio, "ASFWAudioDriver: BuildAudioGraph failed - null provider");
        return kIOReturnBadArgument;
    }

    (void)ASFW::Timing::initializeHostTimebase();

    ivars.workQueue = driver.GetWorkQueue();
    if (!ivars.workQueue) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to get work queue");
        return kIOReturnInvalid;
    }

    ivars.device.audioNub = reinterpret_cast<ASFWAudioNub*>(provider);
    if (!ivars.device.audioNub) {
        ASFW_LOG(Audio, "ASFWAudioDriver: BuildAudioGraph failed - null audio nub");
        return kIOReturnNotReady;
    }

    ASFW::Isoch::Audio::ParsedAudioDriverConfig parsedConfig{};
    ASFW::Isoch::Audio::InitializeAudioDriverConfigDefaults(parsedConfig);

    OSDictionary* propsRaw = nullptr;
    if (provider->CopyProperties(&propsRaw) == kIOReturnSuccess && propsRaw) {
        OSSharedPtr<OSDictionary> props(propsRaw, OSNoRetain);
        ASFW::Isoch::Audio::ParseAudioDriverConfigFromProperties(props.get(), parsedConfig);
    } else {
        ASFW_LOG(Audio, "ASFWAudioDriver: Using default device configuration (no nub properties)");
    }

    // Resolve audio profile registry on startup
    if (const auto* profile = ASFW::Isoch::Audio::AudioProfileRegistry::FindProfile(
            parsedConfig.vendorId, parsedConfig.modelId, parsedConfig.guid)) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Resolved profile '%{public}s'", profile->Name());
        strlcpy(parsedConfig.deviceName, profile->Name(), sizeof(parsedConfig.deviceName));

        const uint32_t rxChannels = profile->RxChannelCount();
        const uint32_t txChannels = profile->TxChannelCount();
        if (rxChannels > 0) {
            parsedConfig.inputChannelCount = rxChannels;
        }
        if (txChannels > 0) {
            parsedConfig.outputChannelCount = txChannels;
        }
        parsedConfig.channelCount = std::max(parsedConfig.inputChannelCount, parsedConfig.outputChannelCount);

        // Regenerate channel names for the updated channel counts
        for (uint32_t index = 0; index < parsedConfig.inputChannelCount && index < ASFW::Isoch::Audio::kMaxNamedChannels; ++index) {
            snprintf(parsedConfig.inputChannelNames[index],
                     sizeof(parsedConfig.inputChannelNames[index]),
                     "%s %u",
                     parsedConfig.inputPlugName,
                     index + 1);
        }
        for (uint32_t index = 0; index < parsedConfig.outputChannelCount && index < ASFW::Isoch::Audio::kMaxNamedChannels; ++index) {
            snprintf(parsedConfig.outputChannelNames[index],
                     sizeof(parsedConfig.outputChannelNames[index]),
                     "%s %u",
                     parsedConfig.outputPlugName,
                     index + 1);
        }
    }

    ASFW::Isoch::Audio::BuildFallbackBoolControls(parsedConfig);
    ASFW::Isoch::Audio::ApplyBringupSingleFormatPolicy(parsedConfig);
    ASFW::Isoch::Audio::ClampAudioDriverChannels(parsedConfig, ASFW::Encoding::kMaxPcmChannels);
    CopyParsedConfigToDeviceState(parsedConfig, ivars.device);

    kern_return_t error = ValidateDeviceStateForGraph(ivars.device);
    if (error != kIOReturnSuccess) {
        return error;
    }

    ASFW_LOG(Audio,
             "ASFWAudioDriver: Device GUID=0x%016llx vendor=0x%06x model=0x%06x boolControls=%u",
             ivars.device.guid,
             ivars.device.vendorId,
             ivars.device.modelId,
             ivars.device.boolControlCount);
    ASFW_LOG(Audio, "ASFWAudioDriver: Read device name from nub: %{public}s", ivars.device.deviceName);
    ASFW_LOG(Audio,
             "ASFWAudioDriver: Read channel counts from nub: aggregate=%u input=%u output=%u",
             ivars.device.channelCount,
             ivars.device.inputChannelCount,
             ivars.device.outputChannelCount);
    ASFW_LOG(Audio, "ASFWAudioDriver: Read %u sample rates from nub", ivars.device.sampleRateCount);
    ASFW_LOG(Audio, "ASFWAudioDriver: Input plug name: %{public}s", ivars.device.inputPlugName);
    ASFW_LOG(Audio, "ASFWAudioDriver: Output plug name: %{public}s", ivars.device.outputPlugName);
    ASFW_LOG(Audio, "ASFWAudioDriver: Current sample rate from nub: %.0f Hz", ivars.device.currentSampleRate);
    ASFW_LOG(Audio, "ASFWAudioDriver: Stream mode from nub: %{public}s",
             ivars.device.streamModeRaw == std::to_underlying(ASFW::Isoch::Audio::StreamMode::kBlocking)
             ? "blocking" : "non-blocking");

    ASFW_LOG(Audio, "ASFWAudioDriver: Forcing single advertised format: 48kHz / 24-bit");
    ASFW_LOG(Audio,
             "ASFWAudioDriver: Effective runtime channels: input=%u output=%u aggregate=%u",
             ivars.device.inputChannelCount,
             ivars.device.outputChannelCount,
             ivars.device.channelCount);

    auto deviceUID = OSSharedPtr(OSString::withCString("ASFWAudioDevice"), OSNoRetain);
    auto modelUID = OSSharedPtr(OSString::withCString(ivars.device.deviceName), OSNoRetain);
    auto manufacturerUID = OSSharedPtr(OSString::withCString("ASFireWire"), OSNoRetain);
    if (!deviceUID || !modelUID || !manufacturerUID) {
        ASFW_LOG(Audio,
                 "ASFWAudioDriver: Failed to create device identity strings deviceUID=%p modelUID=%p manufacturerUID=%p",
                 static_cast<void*>(deviceUID.get()),
                 static_cast<void*>(modelUID.get()),
                 static_cast<void*>(manufacturerUID.get()));
        return kIOReturnNoMemory;
    }

    ivars.audioDevice = IOUserAudioDevice::Create(&driver,
                                                  false,
                                                  deviceUID.get(),
                                                  modelUID.get(),
                                                  manufacturerUID.get(),
                                                  ASFW::IsochTransport::kAudioIoPeriodFrames);
    if (!ivars.audioDevice) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to create IOUserAudioDevice");
        return kIOReturnNoMemory;
    }
    ASFW_LOG(Audio,
             "ASFWAudioDriver: ADK object ids device=%u",
             ivars.audioDevice->GetObjectID());

    error = InstallIOOperationHandler(*ivars.audioDevice, ivars);
    if (error != kIOReturnSuccess) {
        return error;
    }
    ASFW_LOG(Audio, "ASFWAudioDriver: IO operation handler installed");

    auto name = OSSharedPtr(OSString::withCString(ivars.device.deviceName), OSNoRetain);
    if (!name) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to create device name string");
        return kIOReturnNoMemory;
    }
    ivars.audioDevice->SetName(name.get());
    ivars.audioDevice->SetAvailableSampleRates(ivars.device.sampleRates, ivars.device.sampleRateCount);
    ivars.audioDevice->SetSampleRate(ivars.device.currentSampleRate);
    ASFW_LOG(Audio, "ASFWAudioDriver: Initial sample rate set to %.0f Hz", ivars.device.currentSampleRate);

    IOUserAudioStreamBasicDescription inputFormats[8] = {};
    IOUserAudioStreamBasicDescription outputFormats[8] = {};
    const uint32_t formatCount = ivars.device.sampleRateCount > 8 ? 8 : ivars.device.sampleRateCount;
    for (uint32_t i = 0; i < formatCount; i++) {
        FillPcm24Format(inputFormats[i], ivars.device.sampleRates[i], ivars.device.inputChannelCount);
        FillPcm24Format(outputFormats[i], ivars.device.sampleRates[i], ivars.device.outputChannelCount);
    }

    ASFW_LOG(Audio,
             "ASFWAudioDriver: Created %u stream formats (24-bit) in=%u out=%u channels",
             formatCount,
             ivars.device.inputChannelCount,
             ivars.device.outputChannelCount);

    IOMemoryDescriptor* rawOutputMemory = nullptr;
    IOMemoryDescriptor* rawInputMemory = nullptr;
    IOMemoryDescriptor* rawControlMemory = nullptr;
    uint32_t directOutputFrames = 0;
    uint32_t directOutputChannels = 0;
    uint32_t directInputFrames = 0;
    uint32_t directInputChannels = 0;
    uint32_t directSampleRateHz = 0;
    uint64_t directGeneration = 0;

    ASFW_LOG(DirectAudio,
             "ADK DBG MEM request provider=%p guid=0x%016llx expectedInCh=%u expectedOutCh=%u rate=%.0f",
             static_cast<void*>(provider),
             ivars.device.guid,
             ivars.device.inputChannelCount,
             ivars.device.outputChannelCount,
             ivars.device.currentSampleRate);

    error = ivars.device.audioNub->CopyDirectAudioMemory(&rawOutputMemory,
                                                         &rawInputMemory,
                                                         &rawControlMemory,
                                                         &directOutputFrames,
                                                         &directOutputChannels,
                                                         &directInputFrames,
                                                         &directInputChannels,
                                                         &directSampleRateHz,
                                                         &directGeneration);
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

    ivars.outputBuffer = ASFW::Common::AdoptRetained(rawOutputMemory);
    ivars.inputBuffer = ASFW::Common::AdoptRetained(rawInputMemory);
    ivars.controlBuffer = ASFW::Common::AdoptRetained(rawControlMemory);

    if (directOutputChannels != ivars.device.outputChannelCount ||
        directInputChannels != ivars.device.inputChannelCount ||
        directSampleRateHz != static_cast<uint32_t>(ivars.device.currentSampleRate)) {
        ASFW_LOG(DirectAudio,
                 "ADK FATAL MEM metadata mismatch gen=%llu directOutCh=%u localOutCh=%u directInCh=%u localInCh=%u directRate=%u localRate=%u",
                 directGeneration,
                 directOutputChannels,
                 ivars.device.outputChannelCount,
                 directInputChannels,
                 ivars.device.inputChannelCount,
                 directSampleRateHz,
                 static_cast<uint32_t>(ivars.device.currentSampleRate));
        return kIOReturnBadArgument;
    }

    error = ASFW::Common::CreateSharedMapping(ivars.outputBuffer, ivars.outputMap);
    if (error != kIOReturnSuccess) {
        ASFW_LOG(DirectAudio, "ADK DBG MEM map output failed kr=0x%x", error);
        return error;
    }
    error = ASFW::Common::CreateSharedMapping(ivars.inputBuffer, ivars.inputMap);
    if (error != kIOReturnSuccess) {
        ASFW_LOG(DirectAudio, "ADK DBG MEM map input failed kr=0x%x", error);
        return error;
    }
    error = ASFW::Common::CreateSharedMapping(ivars.controlBuffer, ivars.controlMap);
    if (error != kIOReturnSuccess) {
        ASFW_LOG(DirectAudio, "ADK DBG MEM map control failed kr=0x%x", error);
        return error;
    }

    ASFW_LOG(DirectAudio,
             "ADK DBG MEM mapped gen=%llu outBase=%p outLen=%llu outFrames=%u outCh=%u inBase=%p inLen=%llu inFrames=%u inCh=%u control=%p controlLen=%llu rate=%u",
             directGeneration,
             reinterpret_cast<void*>(static_cast<uintptr_t>(ivars.outputMap->GetAddress())),
             ivars.outputMap->GetLength(),
             directOutputFrames,
             directOutputChannels,
             reinterpret_cast<void*>(static_cast<uintptr_t>(ivars.inputMap->GetAddress())),
             ivars.inputMap->GetLength(),
             directInputFrames,
             directInputChannels,
             reinterpret_cast<void*>(static_cast<uintptr_t>(ivars.controlMap->GetAddress())),
             ivars.controlMap->GetLength(),
             directSampleRateHz);

    ivars.inputStream = IOUserAudioStream::Create(&driver,
                                                  IOUserAudioStreamDirection::Input,
                                                  ivars.inputBuffer.get());
    if (!ivars.inputStream) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to create input stream");
        return kIOReturnNoMemory;
    }
    ASFW_LOG(Audio,
             "ASFWAudioDriver: ADK object ids inputStream=%u owner=%u",
             ivars.inputStream->GetObjectID(),
             ivars.inputStream->GetOwnerObjectID());

    auto inputName = OSSharedPtr(OSString::withCString(ivars.device.inputPlugName), OSNoRetain);
    if (!inputName) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to create input stream name");
        return kIOReturnNoMemory;
    }
    ivars.inputStream->SetName(inputName.get());
    ivars.inputStream->SetAvailableStreamFormats(inputFormats, formatCount);
    ivars.inputStream->SetCurrentStreamFormat(&inputFormats[0]);

    ivars.outputStream = IOUserAudioStream::Create(&driver,
                                                   IOUserAudioStreamDirection::Output,
                                                   ivars.outputBuffer.get());
    if (!ivars.outputStream) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to create output stream");
        return kIOReturnNoMemory;
    }
    ASFW_LOG(Audio,
             "ASFWAudioDriver: ADK object ids outputStream=%u owner=%u",
             ivars.outputStream->GetObjectID(),
             ivars.outputStream->GetOwnerObjectID());

    auto outputName = OSSharedPtr(OSString::withCString(ivars.device.outputPlugName), OSNoRetain);
    if (!outputName) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to create output stream name");
        return kIOReturnNoMemory;
    }
    ivars.outputStream->SetName(outputName.get());
    ivars.outputStream->SetAvailableStreamFormats(outputFormats, formatCount);
    ivars.outputStream->SetCurrentStreamFormat(&outputFormats[0]);

    ASFW_LOG(Audio,
             "ASFWAudioDriver: ADK streams fully created. deviceId=%u inputStream=%u outputStream=%u",
             ivars.audioDevice ? ivars.audioDevice->GetObjectID() : 0,
             ivars.inputStream ? ivars.inputStream->GetObjectID() : 0,
             ivars.outputStream ? ivars.outputStream->GetObjectID() : 0);

    const bool directAudioSkeletonBound = BindDirectAudioSkeleton(ivars);
    ASFW_LOG(Audio,
             "ASFWAudioDriver: Direct audio skeleton %{public}s",
             directAudioSkeletonBound ? "bound" : "inactive");
    if (!directAudioSkeletonBound) {
        return kIOReturnNotReady;
    }

    ivars.outputStream->SetLatency(0);
    ivars.inputStream->SetLatency(0);

    error = ivars.audioDevice->AddStream(ivars.inputStream.get());
    if (error != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to add input stream: %d", error);
        return error;
    }
    state.inputStreamAdded = true;

    error = ivars.audioDevice->AddStream(ivars.outputStream.get());
    if (error != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to add output stream: %d", error);
        return error;
    }
    state.outputStreamAdded = true;

    for (uint32_t ch = 1; ch <= ivars.device.outputChannelCount && ch <= 8; ch++) {
        auto outChName = OSSharedPtr(OSString::withCString(ivars.device.outputChannelNames[ch - 1]), OSNoRetain);
        if (outChName) {
            ivars.audioDevice->SetElementName(ch, IOUserAudioObjectPropertyScope::Output, outChName.get());
        }
    }
    for (uint32_t ch = 1; ch <= ivars.device.inputChannelCount && ch <= 8; ch++) {
        auto inChName = OSSharedPtr(OSString::withCString(ivars.device.inputChannelNames[ch - 1]), OSNoRetain);
        if (inChName) {
            ivars.audioDevice->SetElementName(ch, IOUserAudioObjectPropertyScope::Input, inChName.get());
        }
    }

    ASFW::Isoch::Audio::AddBooleanControlsToDevice(driver,
                                                   *ivars.audioDevice,
                                                   ivars.device.boolControls,
                                                   ivars.device.boolControlCount);

    ivars.audioDevice->SetWantsControlsRestored(true);
    driver.SetTransportType(IOUserAudioTransportType::FireWire);
    ivars.audioDevice->SetTransportType(IOUserAudioTransportType::FireWire);
    // Should test IIR clock algorithm as well and make it configurable later
    ivars.audioDevice->SetClockAlgorithm(IOUserAudioClockAlgorithm::TwelvePtMovingWindowAverage);
    ivars.audioDevice->SetClockIsStable(true);
    ivars.audioDevice->SetClockDomain(1);
    const auto policy = ASFW::Audio::TimingCursorPolicy::MakeDice48kBlocking();
    const double currentSampleRate = ivars.device.currentSampleRate;
    const auto* profile = ASFW::Isoch::Audio::AudioProfileRegistry::FindProfile(
        ivars.device.vendorId, ivars.device.modelId, ivars.device.guid);

    uint32_t outLatency = 0;
    uint32_t inLatency = 0;
    uint32_t outSafety = 0;
    uint32_t inSafety = 0;

    if (profile) {
        outLatency = profile->TxReportedLatencyFrames(currentSampleRate);
        inLatency = profile->RxReportedLatencyFrames(currentSampleRate);
        outSafety = profile->TxSafetyOffsetFrames(currentSampleRate);
        inSafety = profile->RxSafetyOffsetFrames(currentSampleRate);
    } else {
        outLatency = policy.ReportedLatencyFrames(ASFW::Audio::AudioDirection::Output);
        inLatency = policy.ReportedLatencyFrames(ASFW::Audio::AudioDirection::Input);
        outSafety = policy.SafetyOffsetFrames(ASFW::Audio::AudioDirection::Output);
        inSafety = policy.SafetyOffsetFrames(ASFW::Audio::AudioDirection::Input);
    }

    ivars.audioDevice->SetOutputLatency(outLatency);
    ivars.audioDevice->SetInputLatency(inLatency);
    ivars.audioDevice->SetOutputSafetyOffset(outSafety);
    ivars.audioDevice->SetInputSafetyOffset(inSafety);

    ASFW_LOG(Audio, "ASFWAudioDriver: Reported HAL latency out=%u/in=%u, safety out=%u/in=%u frames",
             outLatency, inLatency, outSafety, inSafety);

    const kern_return_t ztsKr =
        ivars.audioDevice->SetZeroTimeStampPeriod(policy.HalZeroTimestampPeriodFrames());
    ASFW_LOG(Audio, "ASFWAudioDriver: SetZeroTimeStampPeriod(%u) kr=0x%x",
             policy.HalZeroTimestampPeriodFrames(), ztsKr);

    error = driver.AddObject(ivars.audioDevice.get());
    if (error != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to add device: %d", error);
        return error;
    }
    state.audioDeviceAdded = true;

    error = driver.RegisterService();
    if (error != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioDriver: RegisterService() failed: %d", error);
        return error;
    }

    ASFW_LOG(Audio,
             "✅ ASFWAudioDriver: Started - device '%{public}s' (in=%u out=%u aggregate=%u)",
             ivars.device.deviceName,
             ivars.device.inputChannelCount,
             ivars.device.outputChannelCount,
             ivars.device.channelCount);
    return kIOReturnSuccess;
}

void TearDownAudioGraph(ASFWAudioDriver& driver,
                        ASFWAudioDriver_IVars& ivars,
                        AudioGraphStartState* state) noexcept {
    ivars.runtime.isRunning.store(false, std::memory_order_release);
    StopZtsMirrorTimer(ivars);
    UnbindDirectAudioSkeleton(ivars);

    if (ivars.audioDevice && state) {
        if (state->outputStreamAdded && ivars.outputStream) {
            (void)ivars.audioDevice->RemoveStream(ivars.outputStream.get());
            state->outputStreamAdded = false;
        }
        if (state->inputStreamAdded && ivars.inputStream) {
            (void)ivars.audioDevice->RemoveStream(ivars.inputStream.get());
            state->inputStreamAdded = false;
        }
    }

    if (state && state->audioDeviceAdded && ivars.audioDevice) {
        (void)driver.RemoveObject(ivars.audioDevice.get());
        state->audioDeviceAdded = false;
    }

    ivars.outputStream.reset();
    ivars.inputStream.reset();
    ivars.outputMap.reset();
    ivars.inputMap.reset();
    ivars.controlMap.reset();
    ivars.outputBuffer.reset();
    ivars.inputBuffer.reset();
    ivars.controlBuffer.reset();
    ivars.audioDevice.reset();
    ivars.workQueue.reset();
    ivars.device.audioNub = nullptr;
}

} // namespace ASFW::Audio::DriverKit
