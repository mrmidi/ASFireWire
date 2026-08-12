//
// ASFWAudioDriverGraph.cpp
// ASFWDriver
//
// ADK graph construction/teardown for ASFWAudioDriver.
//
#include <new>

#include "ASFWAudioDevice.h"
#include "ASFWAudioDriverPrivate.hpp"
#include "../Config/InputSafetyPolicy.hpp"
#include "../Config/TimingCursorPolicy.hpp"
#include "../../Common/TimingUtils.hpp"
#include "../../Common/DriverKitOwnership.hpp"
#include "../Shared/AudioTimingGeometry.hpp"
#include "../../Audio/Wire/AMDTP/AmdtpRateGeometry.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSString.h>
#include <AudioDriverKit/IOUserAudioUtils.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <utility>

namespace ASFW::Audio::DriverKit {
namespace {

void CopyParsedConfigToDeviceState(const ASFW::Isoch::Audio::ParsedAudioDriverConfig& parsedConfig,
                                   AudioDriverDeviceState& device) noexcept {
    device.endpointId = parsedConfig.endpointId;
    device.deviceInstanceId = parsedConfig.deviceInstanceId;
    device.observedGuid = parsedConfig.observedGuid;
    device.channelCount = parsedConfig.channelCount;
    device.inputChannelCount = parsedConfig.inputChannelCount;
    device.outputChannelCount = parsedConfig.outputChannelCount;
    strlcpy(device.deviceName, parsedConfig.deviceName, sizeof(device.deviceName));
    strlcpy(device.vendorName, parsedConfig.vendorName, sizeof(device.vendorName));
    strlcpy(device.coreAudioUid, parsedConfig.coreAudioUid,
            sizeof(device.coreAudioUid));
    strlcpy(device.inputPlugName, parsedConfig.inputPlugName, sizeof(device.inputPlugName));
    strlcpy(device.outputPlugName, parsedConfig.outputPlugName, sizeof(device.outputPlugName));
    device.sampleRateCount = parsedConfig.sampleRateCount;
    device.currentSampleRate = parsedConfig.currentSampleRate;
    device.streamModeRaw = std::to_underlying(parsedConfig.streamMode);
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
    if (device.endpointId == 0 || device.deviceInstanceId == 0 ||
        (device.inputChannelCount == 0 && device.outputChannelCount == 0) ||
        device.sampleRateCount == 0 ||
        device.currentSampleRate <= 0.0) {
        ASFW_LOG(Audio,
                 "ASFWAudioDriver: invalid audio config endpoint=%llu instance=%llu in=%u out=%u rates=%u currentRate=%.0f",
                 device.endpointId,
                 device.deviceInstanceId,
                 device.inputChannelCount,
                 device.outputChannelCount,
                 device.sampleRateCount,
                 device.currentSampleRate);
        return kIOReturnBadArgument;
    }
    return kIOReturnSuccess;
}

} // namespace

void FillFloat32Format(IOUserAudioStreamBasicDescription& fmt,
                       double sampleRate,
                       uint32_t channels) noexcept {
    fmt.mSampleRate = sampleRate;
    fmt.mFormatID = IOUserAudioFormatID::LinearPCM;
    fmt.mFormatFlags =
        IOUserAudioFormatFlags::FormatFlagsNativeFloatPacked;
    fmt.mBytesPerPacket = sizeof(float) * channels;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = sizeof(float) * channels;
    fmt.mChannelsPerFrame = channels;
    fmt.mBitsPerChannel = 32;
}

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
        if (!ASFW::Isoch::Audio::ParseAudioDriverConfigFromProperties(
                props.get(), parsedConfig)) {
            ASFW_LOG_ERROR(Audio,
                           "ASFWAudioDriver: rejected missing or invalid resolved endpoint profile property");
            return kIOReturnBadArgument;
        }
    } else {
        ASFW_LOG_ERROR(Audio,
                       "ASFWAudioDriver: missing nub properties; no default identity/profile fallback exists");
        return kIOReturnNotReady;
    }
    ASFW::Isoch::Audio::ClampAudioDriverChannels(parsedConfig, ASFW::Encoding::kMaxPcmChannels);
    ivars.resolvedProfile.Load(parsedConfig.resolvedProfile);
    if (!ivars.resolvedProfile.IsValid()) {
        ASFW_LOG_ERROR(Audio, "ASFWAudioDriver: resolved endpoint profile has invalid stream geometry");
        return kIOReturnBadArgument;
    }
    CopyParsedConfigToDeviceState(parsedConfig, ivars.device);

    kern_return_t error = ValidateDeviceStateForGraph(ivars.device);
    if (error != kIOReturnSuccess) {
        return error;
    }
    const auto requireAdkSuccess =
        [&](const char* operation, kern_return_t status) noexcept -> bool {
        FailIfError(
            status,
            {
                error = status;
                ASFW_LOG(Audio,
                         "ADK FATAL GRAPH op=%{public}s kr=0x%x",
                         operation ? operation : "unknown",
                         status);
            },
            Failure,
            "AudioDriverKit graph operation failed");
        ASFW_LOG(Audio,
                 "ADK GRAPH op=%{public}s kr=0x%x",
                 operation ? operation : "unknown",
                 status);
        return true;

    Failure:
        return false;
    };

    ASFW_LOG(Audio,
             "ASFWAudioDriver: endpoint=%llu instance=%llu observedGUID=0x%016llx boolControls=%u",
             ivars.device.endpointId,
             ivars.device.deviceInstanceId,
             ivars.device.observedGuid,
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

    ASFW_LOG(Audio,
             "ASFWAudioDriver: Advertising %u Float32 format(s) (input/output) across rates",
             ivars.device.sampleRateCount);
    ASFW_LOG(Audio,
             "ASFWAudioDriver: Effective runtime channels: input=%u output=%u aggregate=%u",
             ivars.device.inputChannelCount,
             ivars.device.outputChannelCount,
             ivars.device.channelCount);

    // The device UID must be unique per physical device: macOS persists
    // per-device audio state (Audio MIDI Setup speaker configuration incl. the
    // preferred stereo pair, volumes) keyed by this UID. A shared constant UID
    // let one device's stereo-pair setting silently remap the output channels
    // of every ASFW device (BUGLIST.md Bug 1: "right channel only").
    auto deviceUID = OSSharedPtr(
        OSString::withCString(ivars.device.coreAudioUid), OSNoRetain);
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

    // The init argument is the declared zero-timestamp period. Shared stream
    // memory is sized separately by the selected HAL buffer profile.
    constexpr auto bufferProfile =
        ASFW::Audio::Shared::kActiveAudioHalBufferProfile;
    const uint32_t target_period =
        ASFW::Audio::Shared::AudioTimingGeometry::kHalZeroTimestampPeriodFrames;
    ASFW_LOG(
        Audio,
        "ASFWAudioDriver: HAL buffer profile=%{public}s ring=%u ioBudget=%u zts=%u",
        bufferProfile.name,
        bufferProfile.frameRingFrames,
        bufferProfile.clientIoBudgetFrames,
        bufferProfile.zeroTimestampPeriodFrames);
    ASFW_LOG(Audio, "ASFWAudioDriver: Creating IOUserAudioDevice with ZTS period target: %u frames", target_period);

    ivars.audioDevice = OSSharedPtr(OSTypeAlloc(ASFWAudioDevice), OSNoRetain);
    if (!ivars.audioDevice) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to allocate ASFWAudioDevice");
        return kIOReturnNoMemory;
    }
    if (!ivars.audioDevice->init(&driver, false,
                                 deviceUID.get(), modelUID.get(),
                                 manufacturerUID.get(), target_period)) {
        ASFW_LOG(Audio, "ASFWAudioDriver: ASFWAudioDevice::init failed");
        return kIOReturnNoMemory;
    }
    ivars.audioDevice->SetDriverIvars(&ivars);

    // Do not let the host save/restore a stale stream format from a prior
    // session: the device must come up at the rate this graph selects below,
    // and we drive rate changes explicitly through HandleChangeSampleRate.
    // (Default behavior is restore-enabled; see IOUserAudioDevice header.)
    ivars.audioDevice->SetWantsStreamFormatsRestored(false);

    const uint32_t current_period = ivars.audioDevice->GetZeroTimestampPeriod();
    ASFW_LOG(Audio, "ASFWAudioDriver: IOUserAudioDevice created. GetZeroTimestampPeriod() confirmed: %u frames", current_period);
    ASFW_LOG(Audio,
             "ASFWAudioDriver: ADK object ids device=%u",
             ivars.audioDevice->GetObjectID());

    auto name = OSSharedPtr(OSString::withCString(ivars.device.deviceName), OSNoRetain);
    if (!name) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to create device name string");
        return kIOReturnNoMemory;
    }
    if (!requireAdkSuccess(
            "device.SetName",
            ivars.audioDevice->SetName(name.get()))) {
        return error;
    }
    if (!requireAdkSuccess(
            "device.SetAvailableSampleRates",
            ivars.audioDevice->SetAvailableSampleRates(
                ivars.device.sampleRates,
                ivars.device.sampleRateCount))) {
        return error;
    }
    if (!requireAdkSuccess(
            "device.SetSampleRate",
            ivars.audioDevice->SetSampleRate(
                ivars.device.currentSampleRate))) {
        return error;
    }
    ASFW_LOG(Audio, "ASFWAudioDriver: Initial sample rate set to %.0f Hz", ivars.device.currentSampleRate);

    IOUserAudioStreamBasicDescription inputFormats[8] = {};
    IOUserAudioStreamBasicDescription outputFormats[8] = {};
    const uint32_t formatCount = ivars.device.sampleRateCount > 8 ? 8 : ivars.device.sampleRateCount;
    const bool hasInputStream = ivars.device.inputChannelCount != 0;
    const bool hasOutputStream = ivars.device.outputChannelCount != 0;
    uint32_t currentFormatIndex = 0;
    for (uint32_t i = 0; i < formatCount; i++) {
        if (hasInputStream) {
            FillFloat32Format(inputFormats[i], ivars.device.sampleRates[i], ivars.device.inputChannelCount);
        }
        if (hasOutputStream) {
            FillFloat32Format(outputFormats[i], ivars.device.sampleRates[i], ivars.device.outputChannelCount);
        }
        if (ivars.device.sampleRates[i] == ivars.device.currentSampleRate) {
            currentFormatIndex = i;
        }
    }

    ASFW_LOG(Audio,
             "ASFWAudioDriver: Created %u stream formats input=float32/%u ch output=float32/%u ch; "
             "current format index=%u (%.0f Hz)",
             formatCount,
             ivars.device.inputChannelCount,
             ivars.device.outputChannelCount,
             currentFormatIndex,
             ivars.device.sampleRates[currentFormatIndex]);

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
             "ADK DBG MEM request provider=%p endpoint=%llu expectedInCh=%u expectedOutCh=%u rate=%.0f",
             static_cast<void*>(provider),
             ivars.device.endpointId,
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

    if ((hasOutputStream && directOutputChannels != ivars.device.outputChannelCount) ||
        (hasInputStream && directInputChannels != ivars.device.inputChannelCount) ||
        directOutputChannels == 0 || directInputChannels == 0 ||
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
    if (directOutputFrames != bufferProfile.frameRingFrames ||
        directInputFrames != bufferProfile.frameRingFrames) {
        ASFW_LOG(
            Audio,
            "ADK FATAL MEM ring/profile mismatch profile=%{public}s outFrames=%u inFrames=%u expectedRing=%u ztsPeriod=%u",
            bufferProfile.name,
            directOutputFrames,
            directInputFrames,
            bufferProfile.frameRingFrames,
            target_period);
        return kIOReturnBadArgument;
    }
    ASFW_LOG(
        Audio,
        "ADK GRAPH state=stream-ring/ZTS profile=%{public}s outFrames=%u inFrames=%u ztsPeriod=%u",
        bufferProfile.name,
        directOutputFrames,
        directInputFrames,
        target_period);

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

    if (hasInputStream) {
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
        if (!requireAdkSuccess(
                "inputStream.SetName",
                ivars.inputStream->SetName(inputName.get()))) {
            return error;
        }
        if (!requireAdkSuccess(
                "inputStream.SetAvailableStreamFormats",
                ivars.inputStream->SetAvailableStreamFormats(
                    inputFormats, formatCount))) {
            return error;
        }
        if (!requireAdkSuccess(
                "inputStream.SetCurrentStreamFormat",
                ivars.inputStream->SetCurrentStreamFormat(
                    &inputFormats[currentFormatIndex]))) {
            return error;
        }
    }

    if (hasOutputStream) {
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
        if (!requireAdkSuccess(
                "outputStream.SetName",
                ivars.outputStream->SetName(outputName.get()))) {
            return error;
        }
        if (!requireAdkSuccess(
                "outputStream.SetAvailableStreamFormats",
                ivars.outputStream->SetAvailableStreamFormats(
                    outputFormats, formatCount))) {
            return error;
        }
        if (!requireAdkSuccess(
                "outputStream.SetCurrentStreamFormat",
                ivars.outputStream->SetCurrentStreamFormat(
                    &outputFormats[currentFormatIndex]))) {
            return error;
        }
    }

    ASFW_LOG(Audio,
             "ASFWAudioDriver: ADK streams fully created. deviceId=%u inputStream=%u outputStream=%u",
             ivars.audioDevice ? ivars.audioDevice->GetObjectID() : 0,
             ivars.inputStream ? ivars.inputStream->GetObjectID() : 0,
             ivars.outputStream ? ivars.outputStream->GetObjectID() : 0);

    const bool directAudioSkeletonBound = BindDirectAudioSkeleton(
        ivars,
        DirectAudioMemoryGeometry{
            .inputChannels = directInputChannels,
            .outputChannels = directOutputChannels,
        });
    ASFW_LOG(Audio,
             "ASFWAudioDriver: Direct audio skeleton %{public}s",
             directAudioSkeletonBound ? "bound" : "inactive");
    if (!directAudioSkeletonBound) {
        return kIOReturnNotReady;
    }

    if (ivars.outputStream && !requireAdkSuccess(
                                  "outputStream.SetLatency",
                                  ivars.outputStream->SetLatency(0))) {
        return error;
    }
    if (ivars.inputStream && !requireAdkSuccess(
                                 "inputStream.SetLatency",
                                 ivars.inputStream->SetLatency(0))) {
        return error;
    }

    if (ivars.inputStream) {
        error = ivars.audioDevice->AddStream(ivars.inputStream.get());
        if (!requireAdkSuccess("device.AddStream(input)", error)) {
            return error;
        }
        state.inputStreamAdded = true;
    }
    if (ivars.outputStream) {
        error = ivars.audioDevice->AddStream(ivars.outputStream.get());
        if (!requireAdkSuccess("device.AddStream(output)", error)) {
            return error;
        }
        state.outputStreamAdded = true;
    }

    // Install the RT handler only after every exposed stream is fully
    // configured and attached. The direct transport buffers remain duplex even
    // for playback-only CoreAudio devices.
    error = InstallIOOperationHandler(*ivars.audioDevice, ivars);
    if (!requireAdkSuccess("device.SetIOOperationHandler", error)) {
        return error;
    }
    ASFW_LOG(Audio, "ASFWAudioDriver: IO operation handler installed");

    for (uint32_t ch = 1; ch <= ivars.device.outputChannelCount && ch <= ASFW::Isoch::Audio::kMaxNamedChannels; ch++) {
        auto outChName = OSSharedPtr(OSString::withCString(ivars.device.outputChannelNames[ch - 1]), OSNoRetain);
        if (outChName) {
            const kern_return_t status =
                ivars.audioDevice->SetElementName(
                    ch,
                    IOUserAudioObjectPropertyScope::Output,
                    outChName.get());
            ASFW_LOG(Audio,
                     "ADK GRAPH op=device.SetElementName(output) channel=%u kr=0x%x",
                     ch,
                     status);
            if (status != kIOReturnSuccess) {
                return status;
            }
        }
    }
    for (uint32_t ch = 1; ch <= ivars.device.inputChannelCount && ch <= ASFW::Isoch::Audio::kMaxNamedChannels; ch++) {
        auto inChName = OSSharedPtr(OSString::withCString(ivars.device.inputChannelNames[ch - 1]), OSNoRetain);
        if (inChName) {
            const kern_return_t status =
                ivars.audioDevice->SetElementName(
                    ch,
                    IOUserAudioObjectPropertyScope::Input,
                    inChName.get());
            ASFW_LOG(Audio,
                     "ADK GRAPH op=device.SetElementName(input) channel=%u kr=0x%x",
                     ch,
                     status);
            if (status != kIOReturnSuccess) {
                return status;
            }
        }
    }

    error = ASFW::Isoch::Audio::AddBooleanControlsToDevice(
        driver,
        *ivars.audioDevice,
        ivars.device.boolControls,
        ivars.device.boolControlCount);
    if (!requireAdkSuccess("device.AddBooleanControls", error)) {
        return error;
    }

    // The device is the source of truth for its own control state - we poll
    // HwState to track the physical knob - so a host plist restore at publish
    // time would fight the hardware (IOUserAudioClockDevice.iig:878-891).
    // Device-wide with no per-control opt-out, hence a single decision here.
    ivars.audioDevice->SetWantsControlsRestored(false);
    if (!requireAdkSuccess(
            "driver.SetTransportType",
            driver.SetTransportType(
                IOUserAudioTransportType::FireWire))) {
        return error;
    }
    if (!requireAdkSuccess(
            "device.SetTransportType",
            ivars.audioDevice->SetTransportType(
                IOUserAudioTransportType::FireWire))) {
        return error;
    }
    // Let the HAL clock algorithm absorb the residual jitter in the anchor
    // stream. The ZTS anchors are self-timestamped (sampleFrame, hostTicks)
    // pairs computed at the IR interrupt and back-interpolated, so cross-queue
    // delivery jitter (the OSAction wake + ring drain) does NOT corrupt the
    // VALUES — it only affects freshness, which is exactly what the host clock
    // filter is for. Raw was a bring-up setting to eyeball the raw cycle→host
    // conversion (it forwards anchors un-filtered, so every coalesced-batch /
    // sub-cycle wobble reaches CoreAudio); SimpleIIR is the ADK default and the
    // documented design ("HAL smooths via IOUserAudioClockAlgorithm"). Switch to
    // TwelvePtMovingWindowAverage if heavier smoothing of bursty delivery is
    // wanted (at the cost of slower rate-change tracking).
    if (!requireAdkSuccess(
            "device.SetClockAlgorithm",
            ivars.audioDevice->SetClockAlgorithm(
                IOUserAudioClockAlgorithm::SimpleIIR))) {
        return error;
    }
    if (!requireAdkSuccess(
            "device.SetClockIsStable",
            ivars.audioDevice->SetClockIsStable(true))) {
        return error;
    }
    if (!requireAdkSuccess(
            "device.SetClockDomain",
            ivars.audioDevice->SetClockDomain(1))) {
        return error;
    }
    const double currentSampleRate = ivars.device.currentSampleRate;
    const auto policy = ASFW::Audio::TimingCursorPolicy::MakeDice1xBlocking(
        static_cast<uint32_t>(currentSampleRate));
    const auto* profile = &ivars.resolvedProfile;

    uint32_t outLatency = 0;
    uint32_t inLatency = 0;
    uint32_t outSafety = 0;
    uint32_t inSafety = 0;

    outLatency = profile->TxReportedLatencyFrames(currentSampleRate);
    inLatency = profile->RxReportedLatencyFrames(currentSampleRate);
    outSafety = profile->TxSafetyOffsetFrames(currentSampleRate);
    inSafety = profile->RxSafetyOffsetFrames(currentSampleRate);

    constexpr uint32_t kSchedulingJitterFrames = 64;
    // Data-visibility margin only; the IO buffer size is NOT folded in (see
    // RequiredInputSafetyFrames). This floors the profile's per-rate value
    // (RxSafetyOffsetFrames) at one interrupt batch + jitter, never inflates it.
    const uint32_t requiredInputSafety =
        ASFW::Audio::RequiredInputSafetyFrames(
            inSafety,
            ASFW::Audio::Shared::AudioTimingGeometry::
                kMaximumNominalFramesPerInterrupt,
            kSchedulingJitterFrames);
    if (inSafety != requiredInputSafety) {
        ASFW_LOG(
            Audio,
            "ASFWAudioDriver: input safety %u -> %u (maxIRQFrames=%u jitter=%u)",
            inSafety,
            requiredInputSafety,
            ASFW::Audio::Shared::AudioTimingGeometry::
                kMaximumNominalFramesPerInterrupt,
            kSchedulingJitterFrames);
        inSafety = requiredInputSafety;
    }

    if (!requireAdkSuccess(
            "device.SetOutputLatency",
            ivars.audioDevice->SetOutputLatency(outLatency))) {
        return error;
    }
    if (!requireAdkSuccess(
            "device.SetInputLatency",
            ivars.audioDevice->SetInputLatency(inLatency))) {
        return error;
    }
    if (!requireAdkSuccess(
            "device.SetOutputSafetyOffset",
            ivars.audioDevice->SetOutputSafetyOffset(outSafety))) {
        return error;
    }
    if (!requireAdkSuccess(
            "device.SetInputSafetyOffset",
            ivars.audioDevice->SetInputSafetyOffset(inSafety))) {
        return error;
    }

    ASFW_LOG(Audio, "ASFWAudioDriver: Reported HAL latency out=%u/in=%u, safety out=%u/in=%u frames",
             outLatency, inLatency, outSafety, inSafety);

    const uint32_t configuredZtsPeriod =
        ivars.audioDevice->GetZeroTimestampPeriod();
    if (configuredZtsPeriod != policy.HalZeroTimestampPeriodFrames()) {
        ASFW_LOG(
            Audio,
            "ADK FATAL graph op=device.GetZeroTimestampPeriod expected=%u actual=%u",
            policy.HalZeroTimestampPeriodFrames(),
            configuredZtsPeriod);
        return kIOReturnUnsupported;
    }
    ASFW_LOG(Audio,
             "ADK GRAPH state=device.GetZeroTimestampPeriod value=%u",
             configuredZtsPeriod);

    error = driver.AddObject(ivars.audioDevice.get());
    if (!requireAdkSuccess("driver.AddObject(device)", error)) {
        return error;
    }
    state.audioDeviceAdded = true;

    error = driver.RegisterService();
    if (!requireAdkSuccess("driver.RegisterService", error)) {
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
