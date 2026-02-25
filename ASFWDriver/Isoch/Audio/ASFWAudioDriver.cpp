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
#include "AudioControlBuilder.hpp"
#include "AudioClockEngine.hpp"
#include "AudioDriverConfig.hpp"
#include "AudioIOPath.hpp"
#include "AudioSharedMemoryBridge.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Logging/LogConfig.hpp"
#include "../../Shared/TxSharedQueue.hpp"
#include "../Encoding/PacketAssembler.hpp"
#include "../Config/AudioTxProfiles.hpp"

#include <DriverKit/DriverKit.h>
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/OSBoolean.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <AudioDriverKit/AudioDriverKit.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <utility>

static constexpr bool kEnableZeroCopyOutputPath = false;  // temporary A/B gate

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
    ASFW::Shared::TxSharedQueueSPSC txQueueWriter;
    uint64_t txQueueBytes{0};
    bool txQueueValid{false};

    OSSharedPtr<IOBufferMemoryDescriptor> sharedOutputBuffer;
    OSSharedPtr<IOMemoryMap> sharedOutputMap;
    uint64_t sharedOutputBytes{0};
    uint32_t zeroCopyFrameCapacity{0};
    bool zeroCopyEnabled{false};

    OSSharedPtr<IOBufferMemoryDescriptor> rxQueueMem;
    OSSharedPtr<IOMemoryMap> rxQueueMap;
    ASFW::Shared::TxSharedQueueSPSC rxQueueReader;
    uint64_t rxQueueBytes{0};
    bool rxQueueValid{false};
};

struct AudioDriverRuntimeState {
    OSSharedPtr<IOTimerDispatchSource> timestampTimer;
    OSSharedPtr<OSAction> timestampTimerAction;
    uint64_t hostTicksPerBuffer{0};
    std::atomic<bool> isRunning{false};

    ASFW::Isoch::Audio::IOMetricsState ioMetrics;
    uint64_t metricsLogCounter{0};
    ASFW::Encoding::PacketAssembler packetAssembler;
    ASFW::Isoch::Audio::EncodingMetricsState encodingMetrics;
    ASFW::Isoch::Audio::ClockSyncState clockSync;
    ASFW::Isoch::Audio::ZeroCopyTimelineState zeroCopyTimeline;
    bool rxStartupDrained{false};
};

struct ASFWAudioDriver_IVars {
    OSSharedPtr<IODispatchQueue> workQueue;
    OSSharedPtr<IOUserAudioDevice> audioDevice;
    OSSharedPtr<IOUserAudioStream> inputStream;
    OSSharedPtr<IOUserAudioStream> outputStream;
    OSSharedPtr<IOBufferMemoryDescriptor> inputBuffer;
    OSSharedPtr<IOBufferMemoryDescriptor> outputBuffer;

    AudioDriverDeviceState device;
    AudioDriverSharedMemoryState shared;
    AudioDriverRuntimeState runtime;
};

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
            ivars->runtime.timestampTimer->SetEnable(false);
            ivars->runtime.timestampTimer.reset();
        }
        ivars->runtime.timestampTimerAction.reset();

        // Release ZERO-COPY shared output buffer resources
        ASFW::Isoch::Audio::ResetZeroCopyState(ivars->shared.sharedOutputBuffer,
                       ivars->shared.sharedOutputMap,
                       ivars->shared.sharedOutputBytes,
                       ivars->shared.zeroCopyFrameCapacity,
                       ivars->shared.zeroCopyEnabled);

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
        ivars->outputBuffer.reset();
        ivars->inputBuffer.reset();
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

    // Get shared TX queue from provider (ASFWAudioNub)
    // This enables cross-process audio streaming to IsochTransmitContext.
    ASFWAudioNub* nub = ivars->device.audioNub;

    // Map shared RX queue from nub (for BeginRead audio data from FireWire IR context)
    {
        const kern_return_t rxErr = ASFW::Isoch::Audio::MapRxQueueFromNub(*nub,
                                                                           ivars->shared.rxQueueMem,
                                                                           ivars->shared.rxQueueMap,
                                                                           ivars->shared.rxQueueBytes,
                                                                           ivars->shared.rxQueueReader,
                                                                           ivars->shared.rxQueueValid);
        if (rxErr == kIOReturnSuccess && ivars->shared.rxQueueValid) {
            ASFW_LOG(Audio,
                     "ASFWAudioDriver: RX shared queue mapped: %llu bytes",
                     ivars->shared.rxQueueBytes);
        } else {
            ASFW_LOG(Audio,
                     "ASFWAudioDriver: CopyRxQueueMemory/map failed: 0x%x (RX input will be silent until IR starts)",
                     rxErr);
        }
    }

    {
        const kern_return_t txErr = ASFW::Isoch::Audio::MapTxQueueFromNub(*nub,
                                                                           ivars->shared.txQueueMem,
                                                                           ivars->shared.txQueueMap,
                                                                           ivars->shared.txQueueBytes,
                                                                           ivars->shared.txQueueWriter,
                                                                           ivars->shared.txQueueValid);
        if (txErr == kIOReturnSuccess && ivars->shared.txQueueValid) {
            ASFW_LOG(Audio,
                     "ASFWAudioDriver: TX shared queue attached - bytes=%llu capacity=%u frames",
                     ivars->shared.txQueueBytes,
                     ivars->shared.txQueueWriter.CapacityFrames());
        } else {
            ASFW_LOG(Audio, "ASFWAudioDriver: CopyTransmitQueueMemory/map failed: 0x%x", txErr);
        }
    }

    // Runtime truth: shared queue headers reflect the actual channel topology used by
    // the ASFWDriver process (which may be refined from protocol caps after nub creation).
    if (ivars->shared.rxQueueValid) {
        ivars->device.inputChannelCount = ivars->shared.rxQueueReader.Channels();
    }
    if (ivars->shared.txQueueValid) {
        ivars->device.outputChannelCount = ivars->shared.txQueueWriter.Channels();
    }
    if (ivars->device.inputChannelCount == 0) {
        ivars->device.inputChannelCount = ivars->device.channelCount;
    }
    if (ivars->device.outputChannelCount == 0) {
        ivars->device.outputChannelCount = ivars->device.channelCount;
    }
    ivars->device.channelCount = std::max(ivars->device.inputChannelCount,
                                          ivars->device.outputChannelCount);

    ASFW_LOG(Audio,
             "ASFWAudioDriver: Effective runtime channels: input=%u output=%u aggregate=%u",
             ivars->device.inputChannelCount,
             ivars->device.outputChannelCount,
             ivars->device.channelCount);

    // PacketAssembler is for host->device playback path, so it must use output channels.
    ivars->runtime.packetAssembler.reconfigure(ivars->device.outputChannelCount, 0);
    ivars->runtime.packetAssembler.setStreamMode(
        ivars->device.streamModeRaw == std::to_underlying(ASFW::Isoch::Audio::StreamMode::kBlocking)
            ? ASFW::Encoding::StreamMode::kBlocking
            : ASFW::Encoding::StreamMode::kNonBlocking);
    ASFW_LOG(Audio, "ASFWAudioDriver: PacketAssembler configured for %u playback channels",
             ivars->device.outputChannelCount);
    
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
    
    // Set up IO operation handler -- the real-time audio callback
    // IMPORTANT: This runs in a real-time context. No allocations, no locks, minimal logging.
    // Capture ivars pointer for use in block
    auto* driverIvars = ivars;
    ivars->audioDevice->SetIOOperationHandler(
        ^kern_return_t(IOUserAudioObjectID           objectID,
                       IOUserAudioIOOperation        operation,
                       uint32_t                      ioBufferFrameSize,
                       uint64_t                      sampleTime,
                       uint64_t                      hostTime)
    {
        if (!driverIvars || !driverIvars->runtime.isRunning.load(std::memory_order_acquire)) {
            return kIOReturnNotReady;
        }

        // Driver IO buffers are provisioned for Config::kAudioIoPeriodFrames frames.
        if (ioBufferFrameSize > ASFW::Isoch::Config::kAudioIoPeriodFrames) {
            return kIOReturnBadArgument;
        }
        
        driverIvars->runtime.ioMetrics.callbackCount.fetch_add(1, std::memory_order_relaxed);
        ASFW::Isoch::Audio::AudioIOPathState ioPathState{
            .inputBuffer = driverIvars->inputBuffer.get(),
            .outputBuffer = driverIvars->outputBuffer.get(),
            .inputChannelCount = driverIvars->device.inputChannelCount,
            .outputChannelCount = driverIvars->device.outputChannelCount,
            .ioBufferPeriodFrames = ASFW::Isoch::Config::kAudioIoPeriodFrames,
            .rxStartupDrained = &driverIvars->runtime.rxStartupDrained,
            .rxQueueValid = driverIvars->shared.rxQueueValid,
            .rxQueueReader = &driverIvars->shared.rxQueueReader,
            .txQueueValid = driverIvars->shared.txQueueValid,
            .txQueueWriter = &driverIvars->shared.txQueueWriter,
            .zeroCopyEnabled = driverIvars->shared.zeroCopyEnabled,
            .zeroCopyFrameCapacity = driverIvars->shared.zeroCopyFrameCapacity,
            .zeroCopyTimeline = &driverIvars->runtime.zeroCopyTimeline,
            .packetAssembler = &driverIvars->runtime.packetAssembler,
            .encodingOverruns = &driverIvars->runtime.encodingMetrics.overruns,
        };

        const kern_return_t operationStatus = ASFW::Isoch::Audio::HandleIOOperation(ioPathState,
                                                                                      operation,
                                                                                      ioBufferFrameSize,
                                                                                      sampleTime);

        if (operationStatus != kIOReturnSuccess) {
            return operationStatus;
        }

        if (operation == IOUserAudioIOOperationBeginRead) {
            driverIvars->runtime.ioMetrics.totalFramesSent.fetch_add(ioBufferFrameSize, std::memory_order_relaxed);
        } else if (operation == IOUserAudioIOOperationWriteEnd) {
            driverIvars->runtime.ioMetrics.totalFramesReceived.fetch_add(ioBufferFrameSize, std::memory_order_relaxed);
        }
        
        return kIOReturnSuccess;
    });
    
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
    
    // Buffer sizes (still use 32-bit containers for 24-bit audio)
    const uint32_t inputBufferBytes =
        ASFW::Isoch::Config::kAudioIoPeriodFrames * sizeof(int32_t) * ivars->device.inputChannelCount;
    const uint32_t outputBufferBytes =
        ASFW::Isoch::Config::kAudioIoPeriodFrames * sizeof(int32_t) * ivars->device.outputChannelCount;
    
    // Create input buffer and stream
    error = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, inputBufferBytes, 0,
                                              ivars->inputBuffer.attach());
    if (error != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to create input buffer: %d", error);
        return error;
    }
    
    ivars->inputStream = IOUserAudioStream::Create(this,
                                                    IOUserAudioStreamDirection::Input,
                                                    ivars->inputBuffer.get());
    if (!ivars->inputStream) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to create input stream");
        return kIOReturnNoMemory;
    }
    
    // Use plug name for stream name (e.g., "Analog In")
    auto inputName = OSSharedPtr(OSString::withCString(ivars->device.inputPlugName), OSNoRetain);
    ivars->inputStream->SetName(inputName.get());
    ivars->inputStream->SetAvailableStreamFormats(inputFormats, formatCount);
    ivars->inputStream->SetCurrentStreamFormat(&inputFormats[0]);  // Initial format
    
    // ========================================================================
    // ZERO-COPY: Try to get shared output buffer from ASFWAudioNub
    // If successful, CoreAudio writes here and IT DMA reads directly!
    // ========================================================================
    {
        const kern_return_t zeroCopyErr = ASFW::Isoch::Audio::MapZeroCopyOutputFromNub(
            kEnableZeroCopyOutputPath,
            *nub,
            ivars->device.outputChannelCount,
            ivars->outputBuffer,
            ivars->shared.sharedOutputBuffer,
            ivars->shared.sharedOutputMap,
            ivars->shared.sharedOutputBytes,
            ivars->shared.zeroCopyFrameCapacity,
            ivars->shared.zeroCopyEnabled);

        if (zeroCopyErr == kIOReturnSuccess && ivars->shared.zeroCopyEnabled) {
            ASFW_LOG(Audio,
                     "ASFWAudioDriver: ✅ ZERO-COPY enabled! Shared output buffer: %llu bytes (%u frames)",
                     ivars->shared.sharedOutputBytes,
                     ivars->shared.zeroCopyFrameCapacity);
        } else if (!kEnableZeroCopyOutputPath) {
            ASFW_LOG(Audio, "ASFWAudioDriver: ZERO-COPY disabled by build flag; using TX queue path");
        } else {
            ASFW_LOG(Audio,
                     "ASFWAudioDriver: ZERO-COPY mapping failed: 0x%x, using local buffer",
                     zeroCopyErr);
        }
    }
    
    // Fallback: Create local output buffer if ZERO-COPY not available
    if (!ivars->shared.zeroCopyEnabled) {
        error = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, outputBufferBytes, 0,
                                                  ivars->outputBuffer.attach());
        if (error != kIOReturnSuccess) {
            ASFW_LOG(Audio, "ASFWAudioDriver: Failed to create output buffer: %d", error);
            return error;
        }
        ASFW_LOG(Audio, "ASFWAudioDriver: Using local output buffer (fallback)");
    }
    
    // Create output stream with the appropriate buffer (shared or local)
    ivars->outputStream = IOUserAudioStream::Create(this,
                                                     IOUserAudioStreamDirection::Output,
                                                     ivars->outputBuffer.get());
    if (!ivars->outputStream) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to create output stream");
        return kIOReturnNoMemory;
    }
    
    // Use plug name for stream name (e.g., "Analog Out")
    auto outputName = OSSharedPtr(OSString::withCString(ivars->device.outputPlugName), OSNoRetain);
    ivars->outputStream->SetName(outputName.get());
    ivars->outputStream->SetAvailableStreamFormats(outputFormats, formatCount);
    ivars->outputStream->SetCurrentStreamFormat(&outputFormats[0]);  // Initial format

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
    ASFW_LOG(Audio, "ASFWAudioDriver: StartDevice(id=%u)", in_object_id);
    
    if (!ivars || !ivars->audioDevice || !ivars->runtime.timestampTimer) {
        ASFW_LOG(Audio, "ASFWAudioDriver: StartDevice failed - not initialized");
        return kIOReturnNotReady;
    }

    if (ivars->device.audioNub) {
        const kern_return_t startKr = ivars->device.audioNub->StartAudioStreaming();
        if (startKr != kIOReturnSuccess) {
            ASFW_LOG(Audio, "ASFWAudioDriver: StartAudioStreaming failed: 0x%x", startKr);
        }
    }

    ASFW::Isoch::Audio::AudioClockEngineState clockState{
        .audioDevice = ivars->audioDevice.get(),
        .timestampTimer = ivars->runtime.timestampTimer.get(),
        .txQueueValid = ivars->shared.txQueueValid,
        .txQueueWriter = &ivars->shared.txQueueWriter,
        .rxQueueValid = ivars->shared.rxQueueValid,
        .rxQueueReader = &ivars->shared.rxQueueReader,
        .zeroCopyEnabled = ivars->shared.zeroCopyEnabled,
        .zeroCopyFrameCapacity = ivars->shared.zeroCopyFrameCapacity,
        .zeroCopyTimeline = &ivars->runtime.zeroCopyTimeline,
        .ioBufferPeriodFrames = ASFW::Isoch::Config::kAudioIoPeriodFrames,
        .currentSampleRate = ivars->device.currentSampleRate,
        .hostTicksPerBuffer = &ivars->runtime.hostTicksPerBuffer,
        .clockSync = &ivars->runtime.clockSync,
        .ioMetrics = &ivars->runtime.ioMetrics,
        .metricsLogCounter = &ivars->runtime.metricsLogCounter,
        .packetAssembler = &ivars->runtime.packetAssembler,
        .encodingMetrics = &ivars->runtime.encodingMetrics,
        .rxStartupDrained = &ivars->runtime.rxStartupDrained,
    };
    ASFW::Isoch::Audio::PrepareClockEngineForStart(clockState);
    
    ivars->runtime.isRunning.store(true, std::memory_order_release);
    ASFW_LOG(Audio, "ASFWAudioDriver: Timestamp timer started");
    
    return kIOReturnSuccess;
}

kern_return_t ASFWAudioDriver::StopDevice(IOUserAudioObjectID in_object_id,
                                           IOUserAudioStartStopFlags in_flags)
{
    ASFW_LOG(Audio, "ASFWAudioDriver: StopDevice(id=%u)", in_object_id);
    
    ivars->runtime.isRunning.store(false, std::memory_order_release);

    if (ivars && ivars->device.audioNub) {
        const kern_return_t stopKr = ivars->device.audioNub->StopAudioStreaming();
        if (stopKr != kIOReturnSuccess) {
            ASFW_LOG(Audio, "ASFWAudioDriver: StopAudioStreaming failed: 0x%x", stopKr);
        }
    }

    ASFW::Isoch::Audio::AudioClockEngineState clockState{
        .timestampTimer = ivars ? ivars->runtime.timestampTimer.get() : nullptr,
        .zeroCopyTimeline = ivars ? &ivars->runtime.zeroCopyTimeline : nullptr,
        .clockSync = ivars ? &ivars->runtime.clockSync : nullptr,
    };
    ASFW::Isoch::Audio::PrepareClockEngineForStop(clockState);

    return kIOReturnSuccess;
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

// Timer callback - called periodically to update zero timestamps
void ASFWAudioDriver::ZtsTimerOccurred_Impl([[maybe_unused]] OSAction* action, uint64_t time)
{

    if (!ivars || !ivars->runtime.isRunning.load(std::memory_order_acquire) || !ivars->audioDevice) {
        return;
    }

    ASFW::Isoch::Audio::AudioClockEngineState clockState{
        .audioDevice = ivars->audioDevice.get(),
        .timestampTimer = ivars->runtime.timestampTimer.get(),
        .txQueueValid = ivars->shared.txQueueValid,
        .txQueueWriter = &ivars->shared.txQueueWriter,
        .rxQueueValid = ivars->shared.rxQueueValid,
        .rxQueueReader = &ivars->shared.rxQueueReader,
        .zeroCopyEnabled = ivars->shared.zeroCopyEnabled,
        .zeroCopyFrameCapacity = ivars->shared.zeroCopyFrameCapacity,
        .zeroCopyTimeline = &ivars->runtime.zeroCopyTimeline,
        .ioBufferPeriodFrames = ASFW::Isoch::Config::kAudioIoPeriodFrames,
        .currentSampleRate = ivars->device.currentSampleRate,
        .hostTicksPerBuffer = &ivars->runtime.hostTicksPerBuffer,
        .clockSync = &ivars->runtime.clockSync,
        .ioMetrics = &ivars->runtime.ioMetrics,
        .metricsLogCounter = &ivars->runtime.metricsLogCounter,
        .packetAssembler = &ivars->runtime.packetAssembler,
        .encodingMetrics = &ivars->runtime.encodingMetrics,
        .rxStartupDrained = &ivars->runtime.rxStartupDrained,
    };

    ASFW::Isoch::Audio::HandleClockTimerTick(clockState, time);
}
