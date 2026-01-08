//
// ASFWAudioDriver.cpp
// ASFWDriver
//
// AudioDriverKit driver implementation
// Uses shared memory queue for cross-process audio streaming to IT context
//

#include "ASFWAudioDriver.h"
#include "ASFWAudioNub.h"
#include "../../Logging/Logging.hpp"
#include "../../Shared/TxSharedQueue.hpp"
#include "../Encoding/PacketAssembler.hpp"
#include "../Encoding/AudioRingBuffer.hpp"

#include <DriverKit/DriverKit.h>
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <AudioDriverKit/AudioDriverKit.h>
#include <atomic>

// Default audio configuration
static constexpr double kDefaultSampleRate = 48000.0;
static constexpr uint32_t kDefaultChannelCount = 2;
static constexpr uint32_t kZeroTimestampPeriod = 512;  // frames per buffer

struct ASFWAudioDriver_IVars {
    OSSharedPtr<IODispatchQueue> workQueue;
    OSSharedPtr<IOUserAudioDevice> audioDevice;
    OSSharedPtr<IOUserAudioStream> inputStream;
    OSSharedPtr<IOUserAudioStream> outputStream;
    OSSharedPtr<IOBufferMemoryDescriptor> inputBuffer;
    OSSharedPtr<IOBufferMemoryDescriptor> outputBuffer;

    // Shared memory TX queue for cross-process audio streaming
    // This memory is owned by ASFWAudioNub, we just map it here
    OSSharedPtr<IOBufferMemoryDescriptor> txQueueMem;
    OSSharedPtr<IOMemoryMap> txQueueMap;
    ASFW::Shared::TxSharedQueueSPSC txQueueWriter;
    uint64_t txQueueBytes{0};
    bool txQueueValid{false};

    // Timer for timestamp generation (Phase 1: timer-based, Phase 2: FireWire cycle time)
    OSSharedPtr<IOTimerDispatchSource> timestampTimer;
    OSSharedPtr<OSAction> timestampTimerAction;
    uint64_t hostTicksPerBuffer;  // Timer interval in mach absolute time ticks
    bool isRunning;
    
    // Device info from nub
    char deviceName[128];
    uint32_t channelCount;
    double sampleRates[8];
    uint32_t sampleRateCount;
    double currentSampleRate;  // From device's active format
    
    // Plug names for streams (from MusicSubunit discovery)
    char inputPlugName[64];
    char outputPlugName[64];
    
    // Channel names (parsed from device descriptors)
    // For Duet: "Analog In 1", "Analog In 2", "Analog Out 1", "Analog Out 2"
    char inputChannelNames[8][64];
    char outputChannelNames[8][64];
    
    // Phase 6: IO operation metrics for validation
    struct IOMetrics {
        std::atomic<uint64_t> totalFramesReceived{0};   // From BeginWrite (output from apps)
        std::atomic<uint64_t> totalFramesSent{0};       // From BeginRead (input to apps)
        std::atomic<uint64_t> callbackCount{0};         // Total IO callbacks
        std::atomic<uint64_t> underruns{0};             // Gaps in data
        uint64_t startTime{0};                          // For rate calculation
    } ioMetrics;
    
    // Metrics logging counter (not atomic, only used in timer callback)
    uint64_t metricsLogCounter{0};
    
    // Phase 1.5: AM824/AMDTP encoding pipeline
    ASFW::Encoding::PacketAssembler packetAssembler;
    
    // Encoding validation metrics (for testing without DMA)
    struct EncodingMetrics {
        uint64_t packetsGenerated{0};
        uint64_t dataPackets{0};
        uint64_t noDataPackets{0};
        uint64_t overruns{0};        // Ring buffer overflow (too much data)
        uint64_t lastLogPackets{0};
        double lastLogElapsedSec{0.0};  // For accurate pkts/s calculation
    } encodingMetrics;
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
    
    // Default values
    strlcpy(ivars->deviceName, "FireWire Audio", sizeof(ivars->deviceName));
    ivars->channelCount = kDefaultChannelCount;
    ivars->sampleRates[0] = 44100.0;
    ivars->sampleRates[1] = 48000.0;
    ivars->sampleRateCount = 2;
    ivars->currentSampleRate = kDefaultSampleRate;  // Default until read from nub
    
    // Default plug names
    strlcpy(ivars->inputPlugName, "Input", sizeof(ivars->inputPlugName));
    strlcpy(ivars->outputPlugName, "Output", sizeof(ivars->outputPlugName));
    
    // Default channel names (will be overwritten if available from device)
    for (uint32_t i = 0; i < 8; i++) {
        snprintf(ivars->inputChannelNames[i], sizeof(ivars->inputChannelNames[i]), "In %u", i + 1);
        snprintf(ivars->outputChannelNames[i], sizeof(ivars->outputChannelNames[i]), "Out %u", i + 1);
    }
    
    ASFW_LOG(Audio, "ASFWAudioDriver: init() succeeded");
    return true;
}

void ASFWAudioDriver::free()
{
    ASFW_LOG(Audio, "ASFWAudioDriver: free()");

    if (ivars) {
        // Clean up timer resources first
        if (ivars->timestampTimer) {
            ivars->timestampTimer->SetEnable(false);
            ivars->timestampTimer.reset();
        }
        ivars->timestampTimerAction.reset();

        // Release shared TX queue resources
        ivars->txQueueValid = false;
        ivars->txQueueMap.reset();
        ivars->txQueueMem.reset();

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
    
    // Read device info from nub properties
    OSDictionary* propsRaw = nullptr;
    if (provider->CopyProperties(&propsRaw) == kIOReturnSuccess && propsRaw) {
        OSSharedPtr<OSDictionary> props(propsRaw, OSNoRetain);
        
        // Device name from Config ROM
        if (OSString* name = OSDynamicCast(OSString, props->getObject("ASFWDeviceName"))) {
            strlcpy(ivars->deviceName, name->getCStringNoCopy(), sizeof(ivars->deviceName));
            ASFW_LOG(Audio, "ASFWAudioDriver: Read device name from nub: %{public}s", ivars->deviceName);
        }
        
        // Channel count
        if (OSNumber* count = OSDynamicCast(OSNumber, props->getObject("ASFWChannelCount"))) {
            ivars->channelCount = count->unsigned32BitValue();
            ASFW_LOG(Audio, "ASFWAudioDriver: Read channel count from nub: %u", ivars->channelCount);
        }
        
        // Sample rates array
        if (OSArray* rates = OSDynamicCast(OSArray, props->getObject("ASFWSampleRates"))) {
            ivars->sampleRateCount = 0;
            for (uint32_t i = 0; i < rates->getCount() && i < 8; i++) {
                if (OSNumber* rate = OSDynamicCast(OSNumber, rates->getObject(i))) {
                    ivars->sampleRates[ivars->sampleRateCount++] = 
                        static_cast<double>(rate->unsigned32BitValue());
                }
            }
            ASFW_LOG(Audio, "ASFWAudioDriver: Read %u sample rates from nub", ivars->sampleRateCount);
        }
        
        // Input plug name (e.g. "Analog In")
        if (OSString* name = OSDynamicCast(OSString, props->getObject("ASFWInputPlugName"))) {
            strlcpy(ivars->inputPlugName, name->getCStringNoCopy(), sizeof(ivars->inputPlugName));
            ASFW_LOG(Audio, "ASFWAudioDriver: Input plug name: %{public}s", ivars->inputPlugName);
        }
        
        // Output plug name (e.g. "Analog Out")
        if (OSString* name = OSDynamicCast(OSString, props->getObject("ASFWOutputPlugName"))) {
            strlcpy(ivars->outputPlugName, name->getCStringNoCopy(), sizeof(ivars->outputPlugName));
            ASFW_LOG(Audio, "ASFWAudioDriver: Output plug name: %{public}s", ivars->outputPlugName);
        }
        
        // Generate channel names from plug names
        // e.g. "Analog In" -> "Analog In 1", "Analog In 2"
        for (uint32_t i = 0; i < ivars->channelCount && i < 8; i++) {
            // fixlog_ignore_start
            snprintf(ivars->inputChannelNames[i], sizeof(ivars->inputChannelNames[i]), 
                     "%s %u", ivars->inputPlugName, i + 1);
            snprintf(ivars->outputChannelNames[i], sizeof(ivars->outputChannelNames[i]), 
                     "%s %u", ivars->outputPlugName, i + 1);
            // fixlog_ignore_end
        }
        
        // Current sample rate (from device's active format)
        if (OSNumber* rate = OSDynamicCast(OSNumber, props->getObject("ASFWCurrentSampleRate"))) {
            ivars->currentSampleRate = static_cast<double>(rate->unsigned32BitValue());
            ASFW_LOG(Audio, "ASFWAudioDriver: Current sample rate from nub: %.0f Hz",
                     ivars->currentSampleRate);
        }

    } else {
        ASFW_LOG(Audio, "ASFWAudioDriver: Using default device configuration (no nub properties)");
    }

    // Get shared TX queue from provider (ASFWAudioNub)
    // This enables cross-process audio streaming to IsochTransmitContext
    // NOTE: OSDynamicCast fails across DriverKit process boundaries (provider is a proxy).
    // The kernel matching dictionary guarantees provider IS ASFWAudioNub, so use reinterpret_cast.
    // IIG-generated dispatch routes the RPC correctly.
    ASFWAudioNub* nub = reinterpret_cast<ASFWAudioNub*>(provider);
    {
        IOBufferMemoryDescriptor* txMem = nullptr;
        uint64_t txBytes = 0;
        kern_return_t txErr = nub->CopyTransmitQueueMemory(&txMem, &txBytes);

        if (txErr == kIOReturnSuccess && txMem && txBytes > 0) {
            // Store the memory descriptor (already retained by CopyTransmitQueueMemory)
            ivars->txQueueMem.reset(txMem, OSNoRetain);
            ivars->txQueueBytes = txBytes;

            // Create local mapping
            IOMemoryMap* txMap = nullptr;
            txErr = txMem->CreateMapping(
                kIOMemoryMapCacheModeDefault,
                0,    // offset
                0,    // address (kernel chooses)
                0,    // length (entire descriptor)
                0,    // options
                &txMap);

            if (txErr == kIOReturnSuccess && txMap) {
                ivars->txQueueMap.reset(txMap, OSNoRetain);

                // Attach writer to shared memory
                void* base = reinterpret_cast<void*>(txMap->GetAddress());
                if (ivars->txQueueWriter.Attach(base, txBytes)) {
                    ivars->txQueueValid = true;
                    ASFW_LOG(Audio, "ASFWAudioDriver: TX shared queue attached - bytes=%llu capacity=%u frames",
                             txBytes, ivars->txQueueWriter.CapacityFrames());
                } else {
                    ASFW_LOG(Audio, "ASFWAudioDriver: TX queue Attach failed - invalid header?");
                    ivars->txQueueMap.reset();
                    ivars->txQueueMem.reset();
                    ivars->txQueueBytes = 0;
                }
            } else {
                ASFW_LOG(Audio, "ASFWAudioDriver: TX queue CreateMapping failed: 0x%x", txErr);
                ivars->txQueueMem.reset();
                ivars->txQueueBytes = 0;
            }
        } else {
            ASFW_LOG(Audio, "ASFWAudioDriver: CopyTransmitQueueMemory failed: 0x%x", txErr);
        }
    }
    
    // Create audio device
    auto deviceUID = OSSharedPtr(OSString::withCString("ASFWAudioDevice"), OSNoRetain);
    auto modelUID = OSSharedPtr(OSString::withCString(ivars->deviceName), OSNoRetain);
    auto manufacturerUID = OSSharedPtr(OSString::withCString("ASFireWire"), OSNoRetain);
    
    ivars->audioDevice = IOUserAudioDevice::Create(this,
                                                    false,  // no prewarming
                                                    deviceUID.get(),
                                                    modelUID.get(),
                                                    manufacturerUID.get(),
                                                    kZeroTimestampPeriod);
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
        if (!driverIvars || !driverIvars->isRunning) {
            return kIOReturnNotReady;
        }
        
        driverIvars->ioMetrics.callbackCount.fetch_add(1, std::memory_order_relaxed);
        
        // IOUserAudioIOOperation uses C-style constants, not C++ scoped enums
        // IOUserAudioIOOperationBeginRead = 0: Called prior to reading from device (input streams)
        // IOUserAudioIOOperationWriteEnd = 1: Called after writing to device (output streams)
        switch (operation) {
            case IOUserAudioIOOperationBeginRead: {
                // Input: CoreAudio wants to READ audio FROM us (capture/microphone)
                // Phase 1: Fill with silence (no FireWire RX yet)
                IOAddressSegment segment{};
                kern_return_t kr = driverIvars->inputBuffer->GetAddressRange(&segment);
                if (kr == kIOReturnSuccess && segment.address != 0) {
                    // Zero the buffer - silence
                    size_t bytesToZero = ioBufferFrameSize * sizeof(int32_t) * driverIvars->channelCount;
                    memset(reinterpret_cast<void*>(segment.address), 0, bytesToZero);
                }
                driverIvars->ioMetrics.totalFramesSent.fetch_add(ioBufferFrameSize, std::memory_order_relaxed);
                break;
            }
            
            case IOUserAudioIOOperationWriteEnd: {
                // Output: CoreAudio has just WRITTEN audio TO us (playback)
                // Write PCM to shared TX queue (consumed by IsochTransmitContext)
                IOAddressSegment segment{};
                kern_return_t kr = driverIvars->outputBuffer->GetAddressRange(&segment);
                if (kr == kIOReturnSuccess && segment.address != 0) {
                    const int32_t* pcmData = reinterpret_cast<const int32_t*>(segment.address);
                    uint32_t framesWritten = 0;

                    // Write to shared TX queue if available, otherwise local buffer
                    if (driverIvars->txQueueValid) {
                        framesWritten = driverIvars->txQueueWriter.Write(pcmData, ioBufferFrameSize);

                        // Log first write and periodically after
                        static uint64_t txWriteCount = 0;
                        if (txWriteCount == 0 || (txWriteCount % 1000) == 0) {
                            int32_t sample0 = pcmData[0];
                            int32_t sample1 = pcmData[1];
                            ASFW_LOG(Audio, "TX_Q[%llu]: %u frames written (fill=%u) samples=[%08x,%08x]",
                                     txWriteCount, framesWritten,
                                     driverIvars->txQueueWriter.FillLevelFrames(),
                                     (uint32_t)sample0, (uint32_t)sample1);
                        }
                        txWriteCount++;
                    } else {
                        // Fallback to local buffer for debugging without IT
                        framesWritten = driverIvars->packetAssembler.ringBuffer().write(pcmData, ioBufferFrameSize);
                    }

                    if (framesWritten < ioBufferFrameSize) {
                        // Ring buffer overflow - too much data, not consuming fast enough
                        driverIvars->encodingMetrics.overruns++;
                    }
                }
                driverIvars->ioMetrics.totalFramesReceived.fetch_add(ioBufferFrameSize, std::memory_order_relaxed);
                break;
            }
            
            default:
                break;
        }
        
        return kIOReturnSuccess;
    });
    
    ASFW_LOG(Audio, "ASFWAudioDriver: IO operation handler installed");
    
    // Set device name
    auto name = OSSharedPtr(OSString::withCString(ivars->deviceName), OSNoRetain);
    ivars->audioDevice->SetName(name.get());
    
    // Set sample rates
    ivars->audioDevice->SetAvailableSampleRates(ivars->sampleRates, ivars->sampleRateCount);
    
    // Set initial sample rate to device's current rate (from active format)
    ivars->audioDevice->SetSampleRate(ivars->currentSampleRate);
    ASFW_LOG(Audio, "ASFWAudioDriver: Initial sample rate set to %.0f Hz", ivars->currentSampleRate);
    
    // Create stream formats - one for each supported sample rate
    // This populates the Format dropdown in Audio MIDI Setup
    // Using 24-bit audio as expected by FireWire hardware (packed in 32-bit container)
    IOUserAudioStreamBasicDescription formats[8] = {};
    uint32_t formatCount = ivars->sampleRateCount > 8 ? 8 : ivars->sampleRateCount;
    
    for (uint32_t i = 0; i < formatCount; i++) {
        formats[i].mSampleRate = ivars->sampleRates[i];
        formats[i].mFormatID = IOUserAudioFormatID::LinearPCM;
        formats[i].mFormatFlags = static_cast<IOUserAudioFormatFlags>(
            static_cast<uint32_t>(IOUserAudioFormatFlags::FormatFlagIsSignedInteger) |
            static_cast<uint32_t>(IOUserAudioFormatFlags::FormatFlagsNativeEndian));
        // 24-bit audio in 32-bit containers (standard for pro audio)
        formats[i].mBytesPerPacket = sizeof(int32_t) * ivars->channelCount;
        formats[i].mFramesPerPacket = 1;
        formats[i].mBytesPerFrame = sizeof(int32_t) * ivars->channelCount;
        formats[i].mChannelsPerFrame = ivars->channelCount;
        formats[i].mBitsPerChannel = 24;  // 24-bit audio for hardware
    }
    
    ASFW_LOG(Audio, "ASFWAudioDriver: Created %u stream formats (24-bit)", formatCount);
    
    // Buffer size (still use 32-bit containers for 24-bit audio)
    const uint32_t bufferBytes = kZeroTimestampPeriod * sizeof(int32_t) * ivars->channelCount;
    
    // Create input buffer and stream
    error = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, bufferBytes, 0,
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
    auto inputName = OSSharedPtr(OSString::withCString(ivars->inputPlugName), OSNoRetain);
    ivars->inputStream->SetName(inputName.get());
    ivars->inputStream->SetAvailableStreamFormats(formats, formatCount);
    ivars->inputStream->SetCurrentStreamFormat(&formats[0]);  // Initial format
    
    // Create output buffer and stream
    error = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, bufferBytes, 0,
                                              ivars->outputBuffer.attach());
    if (error != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to create output buffer: %d", error);
        return error;
    }
    
    ivars->outputStream = IOUserAudioStream::Create(this,
                                                     IOUserAudioStreamDirection::Output,
                                                     ivars->outputBuffer.get());
    if (!ivars->outputStream) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to create output stream");
        return kIOReturnNoMemory;
    }
    
    // Use plug name for stream name (e.g., "Analog Out")
    auto outputName = OSSharedPtr(OSString::withCString(ivars->outputPlugName), OSNoRetain);
    ivars->outputStream->SetName(outputName.get());
    ivars->outputStream->SetAvailableStreamFormats(formats, formatCount);
    ivars->outputStream->SetCurrentStreamFormat(&formats[0]);  // Initial format
    
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
    for (uint32_t ch = 1; ch <= ivars->channelCount && ch <= 8; ch++) {
        auto outChName = OSSharedPtr(OSString::withCString(ivars->outputChannelNames[ch-1]), OSNoRetain);
        ivars->audioDevice->SetElementName(ch, IOUserAudioObjectPropertyScope::Output, outChName.get());
        
        auto inChName = OSSharedPtr(OSString::withCString(ivars->inputChannelNames[ch-1]), OSNoRetain);
        ivars->audioDevice->SetElementName(ch, IOUserAudioObjectPropertyScope::Input, inChName.get());
    }
    
    // Set transport type on BOTH driver and device
    // IOUserAudioDevice inherits SetTransportType from IOUserAudioClockDevice
    SetTransportType(IOUserAudioTransportType::FireWire);  // On driver
    ivars->audioDevice->SetTransportType(IOUserAudioTransportType::FireWire);  // On device
    
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
    ivars->timestampTimer = OSSharedPtr(timerSource, OSNoRetain);
    
    // Create timer action (DriverKit generates CreateActionZtsTimerOccurred from .iig)
    OSAction* timerAction = nullptr;
    error = CreateActionZtsTimerOccurred(sizeof(void*), &timerAction);
    if (error != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Failed to create timer action: %d", error);
        return error;
    }
    ivars->timestampTimerAction = OSSharedPtr(timerAction, OSNoRetain);
    ivars->timestampTimer->SetHandler(ivars->timestampTimerAction.get());
    
    ASFW_LOG(Audio, "✅ ASFWAudioDriver: Started - device '%{public}s' with %u channels",
             ivars->deviceName, ivars->channelCount);
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWAudioDriver, Stop)
{
    ASFW_LOG(Audio, "ASFWAudioDriver: Stop()");
    
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
    
    if (!ivars || !ivars->audioDevice || !ivars->timestampTimer) {
        ASFW_LOG(Audio, "ASFWAudioDriver: StartDevice failed - not initialized");
        return kIOReturnNotReady;
    }
    
    // Reset IO metrics for this session
    ivars->ioMetrics.totalFramesReceived.store(0, std::memory_order_relaxed);
    ivars->ioMetrics.totalFramesSent.store(0, std::memory_order_relaxed);
    ivars->ioMetrics.callbackCount.store(0, std::memory_order_relaxed);
    ivars->ioMetrics.underruns.store(0, std::memory_order_relaxed);
    ivars->ioMetrics.startTime = mach_absolute_time();
    ivars->metricsLogCounter = 0;
    
    // Reset encoding pipeline for this session
    ivars->packetAssembler.reset();
    
    // Calculate timer interval based on current sample rate
    struct mach_timebase_info timebase_info;
    mach_timebase_info(&timebase_info);
    
    double sample_rate = ivars->currentSampleRate;
    double host_ticks_per_buffer = static_cast<double>(kZeroTimestampPeriod * NSEC_PER_SEC) / sample_rate;
    host_ticks_per_buffer = (host_ticks_per_buffer * static_cast<double>(timebase_info.denom)) 
                          / static_cast<double>(timebase_info.numer);
    ivars->hostTicksPerBuffer = static_cast<uint64_t>(host_ticks_per_buffer);
    
    ASFW_LOG(Audio, "ASFWAudioDriver: Timer interval = %llu ticks (%.0f Hz, period=%u frames)",
             ivars->hostTicksPerBuffer, sample_rate, kZeroTimestampPeriod);
    
    // Clear timestamps to initial state
    ivars->audioDevice->UpdateCurrentZeroTimestamp(0, 0);
    
    // Start the timer - first callback will set initial timestamp
    uint64_t current_time = mach_absolute_time();
    ivars->timestampTimer->WakeAtTime(kIOTimerClockMachAbsoluteTime, 
                                       current_time + ivars->hostTicksPerBuffer, 0);
    ivars->timestampTimer->SetEnable(true);
    
    ivars->isRunning = true;
    ASFW_LOG(Audio, "ASFWAudioDriver: Timestamp timer started");
    
    return kIOReturnSuccess;
}

kern_return_t ASFWAudioDriver::StopDevice(IOUserAudioObjectID in_object_id,
                                           IOUserAudioStartStopFlags in_flags)
{
    ASFW_LOG(Audio, "ASFWAudioDriver: StopDevice(id=%u)", in_object_id);
    
    ivars->isRunning = false;
    
    if (ivars && ivars->timestampTimer) {
        ivars->timestampTimer->SetEnable(false);
        ASFW_LOG(Audio, "ASFWAudioDriver: Timestamp timer stopped");
    }
    
    return kIOReturnSuccess;
}

// Timer callback - called periodically to update zero timestamps
void ASFWAudioDriver::ZtsTimerOccurred_Impl(OSAction* action, uint64_t time)
{
    if (!ivars || !ivars->isRunning || !ivars->audioDevice) {
        return;
    }
    
    // Get current timestamps
    uint64_t current_sample_time = 0;
    uint64_t current_host_time = 0;
    ivars->audioDevice->GetCurrentZeroTimestamp(&current_sample_time, &current_host_time);
    
    uint64_t host_ticks_per_buffer = ivars->hostTicksPerBuffer;
    
    if (current_host_time != 0) {
        // Increment by one buffer period
        current_sample_time += kZeroTimestampPeriod;
        current_host_time += host_ticks_per_buffer;
    } else {
        // First timestamp - use current time as reference
        current_sample_time = 0;
        current_host_time = time;
    }
    
    // Update the device with new timestamp
    ivars->audioDevice->UpdateCurrentZeroTimestamp(current_sample_time, current_host_time);
    
    // Schedule next timer fire
    ivars->timestampTimer->WakeAtTime(kIOTimerClockMachAbsoluteTime,
                                       current_host_time + host_ticks_per_buffer, 0);
    
    // Log IO metrics periodically (~every 5 seconds)
    // At 44.1kHz/512 frames = ~86 callbacks/sec → 430 callbacks ≈ 5 seconds
    if (++ivars->metricsLogCounter % 430 == 0) {
        uint64_t framesReceived = ivars->ioMetrics.totalFramesReceived.load(std::memory_order_relaxed);
        uint64_t framesSent = ivars->ioMetrics.totalFramesSent.load(std::memory_order_relaxed);
        uint64_t callbacks = ivars->ioMetrics.callbackCount.load(std::memory_order_relaxed);
        uint64_t underruns = ivars->ioMetrics.underruns.load(std::memory_order_relaxed);
        
        // Ring buffer metrics from encoding pipeline
        uint32_t ringFillLevel = ivars->packetAssembler.bufferFillLevel();
        uint64_t ringUnderruns = ivars->packetAssembler.underrunCount();
        
        // Calculate elapsed time in seconds
        uint64_t elapsed = time - ivars->ioMetrics.startTime;
        struct mach_timebase_info timebase;
        mach_timebase_info(&timebase);
        double elapsedSec = (double)elapsed * (double)timebase.numer / (double)timebase.denom / 1e9;
        
        if (elapsedSec > 0.0) {
            double framesPerSec = (double)framesReceived / elapsedSec;
            
            // Calculate actual time delta since last log for accurate pkts/s
            double dt = elapsedSec - ivars->encodingMetrics.lastLogElapsedSec;
            uint64_t dp = ivars->encodingMetrics.packetsGenerated - ivars->encodingMetrics.lastLogPackets;
            double packetsPerSec = (dt > 0.0) ? (double)dp / dt : 0.0;
            
            ASFW_LOG(Audio, "IO: %.1fs recv=%llu (%.0f/s) ring=%u overruns=%llu underruns=%llu/%llu | Encoding: %llu pkts (%.0f/s, D:%llu N:%llu)",
                     elapsedSec, framesReceived, framesPerSec, ringFillLevel, 
                     ivars->encodingMetrics.overruns, underruns, ringUnderruns,
                     ivars->encodingMetrics.packetsGenerated, packetsPerSec,
                     ivars->encodingMetrics.dataPackets, ivars->encodingMetrics.noDataPackets);
            
            ivars->encodingMetrics.lastLogPackets = ivars->encodingMetrics.packetsGenerated;
            ivars->encodingMetrics.lastLogElapsedSec = elapsedSec;
        }
    }
    
    // Phase 1.5: Consume audio from ring buffer and validate encoding
    // Timer fires at buffer rate (~86Hz for 512 frames), so we need to process
    // all accumulated audio each tick to keep the ring buffer from overflowing.
    // At 48kHz with 512-frame buffers:
    //   512 frames / 8 samples per DATA = 64 DATA packets needed
    //   Total with NO-DATA: ~85 packets per buffer period (8 cycles = 6 DATA + 2 NO-DATA)
    // Drain until ring buffer has less than one packet's worth of samples
    while (ivars->packetAssembler.bufferFillLevel() >= 8) {  // Need 8 samples for DATA packet
        auto pkt = ivars->packetAssembler.assembleNext(0xFFFF);  // Phase 1.5: dummy SYT
        ivars->encodingMetrics.packetsGenerated++;
        if (pkt.isData) {
            ivars->encodingMetrics.dataPackets++;
        } else {
            ivars->encodingMetrics.noDataPackets++;
        }
    }
}

