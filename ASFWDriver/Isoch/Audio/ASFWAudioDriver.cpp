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

#include <DriverKit/DriverKit.h>
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <AudioDriverKit/AudioDriverKit.h>
#include <atomic>
#include <cmath>

// Default audio configuration
static constexpr double kDefaultSampleRate = 48000.0;
static constexpr uint32_t kDefaultChannelCount = 2;
static constexpr uint32_t kZeroTimestampPeriod = 512;  // frames per buffer
static constexpr bool kEnableZeroCopyOutputPath = false;  // temporary A/B gate

// Report only hardware/presentation pipeline latency to HAL.
// Software queue/ring buffering should not be baked into device latency fields.
static constexpr uint32_t kReportedDeviceLatencyFrames = 24;  // ~0.5ms @ 48kHz
static constexpr uint32_t kReportedSafetyOffsetFrames = 32;   // ~0.67ms @ 48kHz

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

    // ZERO-COPY: Shared output audio buffer from ASFWAudioNub
    // This buffer is used by IOUserAudioStream AND IT DMA
    // CoreAudio writes here, IT DMA reads directly - no intermediate copy!
    OSSharedPtr<IOBufferMemoryDescriptor> sharedOutputBuffer;  // From ASFWAudioNub
    OSSharedPtr<IOMemoryMap> sharedOutputMap;                  // Local mapping
    uint64_t sharedOutputBytes{0};
    uint32_t zeroCopyFrameCapacity{0};
    bool zeroCopyEnabled{false};

    // Timer for timestamp generation (Phase 1: timer-based, Phase 2: FireWire cycle time)
    OSSharedPtr<IOTimerDispatchSource> timestampTimer;
    OSSharedPtr<OSAction> timestampTimerAction;
    uint64_t hostTicksPerBuffer;  // Timer interval in mach absolute time ticks
    std::atomic<bool> isRunning{false};
    
    // Device info from nub
    char deviceName[128];
    uint32_t channelCount;
    double sampleRates[8];
    uint32_t sampleRateCount;
    double currentSampleRate;  // From device's active format
    uint32_t streamModeRaw{0}; // 0=non-blocking, 1=blocking
    
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
    
    // AM824/AMDTP encoding pipeline
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

    // Clock synchronization (software PLL)
    // Feeds back TX queue fill level to adjust CoreAudio timestamps,
    // keeping production rate matched to FireWire bus consumption rate.
    struct ClockSync {
        // Fill-level tracking
        uint32_t targetFillLevel{0};       // Ideal TX queue fill (set at StartDevice)
        int64_t  fillErrorIntegral{0};     // Accumulated fill error (for I term)
        int32_t  lastFillError{0};         // Previous fill error (for smoothing)

        // Adaptive tick adjustment
        double   nominalTicksPerBuffer{0}; // Exact nominal value (double precision)
        double   currentTicksPerBuffer{0}; // Adjusted value after PLL correction
        double   fractionalTicks{0.0};     // Sub-tick accumulator for jitter-free rounding

        // Drift statistics
        uint64_t adjustmentCount{0};
        double   maxCorrectionPpm{0.0};
    } clockSync;

    // ZERO-COPY producer timeline state.
    // Keeps published frame counters aligned to CoreAudio sampleTime.
    struct ZeroCopyTimeline {
        bool valid{false};
        uint64_t lastSampleTime{0};
        uint64_t publishedSampleTime{0};
        uint64_t discontinuities{0};
        uint32_t phaseFrames{0};
    } zeroCopyTimeline;

    // RX shared memory queue for FireWire input (IR context -> CoreAudio)
    OSSharedPtr<IOBufferMemoryDescriptor> rxQueueMem;
    OSSharedPtr<IOMemoryMap> rxQueueMap;
    ASFW::Shared::TxSharedQueueSPSC rxQueueReader;
    uint64_t rxQueueBytes{0};
    bool rxQueueValid{false};
    bool rxStartupDrained{false};

    // RX clock sync removed — replaced by cycle-time-based rate estimation.
    // The controller's IR Poll writes corrHostNanosPerSampleQ8 to the shared
    // RX queue header; ZtsTimerOccurred reads it to derive host_ticks_per_buffer.
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
    ivars->sampleRates[0] = kDefaultSampleRate;
    ivars->sampleRateCount = 1;
    ivars->currentSampleRate = kDefaultSampleRate;
    ivars->streamModeRaw = 0;
    
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

        // Release ZERO-COPY shared output buffer resources
        ivars->zeroCopyEnabled = false;
        ivars->sharedOutputMap.reset();
        ivars->sharedOutputBuffer.reset();
        ivars->sharedOutputBytes = 0;
        ivars->zeroCopyFrameCapacity = 0;

        // Release shared RX queue resources
        ivars->rxQueueValid = false;
        ivars->rxQueueMap.reset();
        ivars->rxQueueMem.reset();
        ivars->rxQueueBytes = 0;

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

        if (OSNumber* mode = OSDynamicCast(OSNumber, props->getObject("ASFWStreamMode"))) {
            ivars->streamModeRaw = (mode->unsigned32BitValue() == 1u) ? 1u : 0u;
            ASFW_LOG(Audio, "ASFWAudioDriver: Stream mode from nub: %{public}s",
                     ivars->streamModeRaw == 1u ? "blocking" : "non-blocking");
        }

    } else {
        ASFW_LOG(Audio, "ASFWAudioDriver: Using default device configuration (no nub properties)");
    }

    // Temporary bring-up policy: expose exactly one format/rate in ADK.
    ivars->sampleRates[0] = kDefaultSampleRate;
    ivars->sampleRateCount = 1;
    ivars->currentSampleRate = kDefaultSampleRate;
    ASFW_LOG(Audio, "ASFWAudioDriver: Forcing single advertised format: 48kHz / 24-bit");

    // Clamp channels to supported range before using it for buffer sizing/format setup.
    if (ivars->channelCount == 0) {
        ivars->channelCount = kDefaultChannelCount;
        ASFW_LOG(Audio, "ASFWAudioDriver: Invalid channel count 0 from nub, using default %u",
                 ivars->channelCount);
    } else if (ivars->channelCount > ASFW::Encoding::kMaxSupportedChannels) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Clamping channel count %u -> %u",
                 ivars->channelCount, ASFW::Encoding::kMaxSupportedChannels);
        ivars->channelCount = ASFW::Encoding::kMaxSupportedChannels;
    }

    // Reconfigure packet assembler with actual channel count
    ivars->packetAssembler.reconfigure(ivars->channelCount, 0);
    ivars->packetAssembler.setStreamMode(
        ivars->streamModeRaw == 1u
            ? ASFW::Encoding::StreamMode::kBlocking
            : ASFW::Encoding::StreamMode::kNonBlocking);
    ASFW_LOG(Audio, "ASFWAudioDriver: PacketAssembler configured for %u channels", ivars->channelCount);

    // Get shared TX queue from provider (ASFWAudioNub)
    // This enables cross-process audio streaming to IsochTransmitContext
    // NOTE: OSDynamicCast fails across DriverKit process boundaries (provider is a proxy).
    // The kernel matching dictionary guarantees provider IS ASFWAudioNub, so use reinterpret_cast.
    // IIG-generated dispatch routes the RPC correctly.
    ASFWAudioNub* nub = reinterpret_cast<ASFWAudioNub*>(provider);

    // Map shared RX queue from nub (for BeginRead audio data from FireWire IR context)
    {
        IOBufferMemoryDescriptor* rxMem = nullptr;
        uint64_t rxBytes = 0;
        kern_return_t rxErr = nub->CopyRxQueueMemory(&rxMem, &rxBytes);
        if (rxErr == kIOReturnSuccess && rxMem && rxBytes > 0) {
            ivars->rxQueueMem.reset(rxMem, OSNoRetain);
            ivars->rxQueueBytes = rxBytes;
            IOMemoryMap* rxMap = nullptr;
            rxErr = rxMem->CreateMapping(kIOMemoryMapCacheModeDefault, 0, 0, 0, 0, &rxMap);
            if (rxErr == kIOReturnSuccess && rxMap) {
                ivars->rxQueueMap.reset(rxMap, OSNoRetain);
                void* base = reinterpret_cast<void*>(rxMap->GetAddress());
                if (ivars->rxQueueReader.Attach(base, rxBytes)) {
                    ivars->rxQueueValid = true;
                    ASFW_LOG(Audio, "ASFWAudioDriver: RX shared queue mapped: %llu bytes, base=%p", rxBytes, base);
                } else {
                    ASFW_LOG(Audio, "ASFWAudioDriver: RX queue Attach failed - invalid header?");
                    ivars->rxQueueMap.reset();
                    ivars->rxQueueMem.reset();
                    ivars->rxQueueBytes = 0;
                }
            } else {
                ASFW_LOG(Audio, "ASFWAudioDriver: RX queue CreateMapping failed: 0x%x", rxErr);
                ivars->rxQueueMem.reset();
                ivars->rxQueueBytes = 0;
            }
        } else {
            ASFW_LOG(Audio, "ASFWAudioDriver: CopyRxQueueMemory failed: 0x%x (RX input will be silent until IR starts)", rxErr);
        }
    }

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
        if (!driverIvars || !driverIvars->isRunning.load(std::memory_order_acquire)) {
            return kIOReturnNotReady;
        }

        // Driver IO buffers are provisioned for kZeroTimestampPeriod frames.
        if (ioBufferFrameSize > kZeroTimestampPeriod) {
            return kIOReturnBadArgument;
        }
        
        driverIvars->ioMetrics.callbackCount.fetch_add(1, std::memory_order_relaxed);
        
        // IOUserAudioIOOperation uses C-style constants, not C++ scoped enums
        // IOUserAudioIOOperationBeginRead = 0: Called prior to reading from device (input streams)
        // IOUserAudioIOOperationWriteEnd = 1: Called after writing to device (output streams)
        switch (operation) {
            case IOUserAudioIOOperationBeginRead: {
                // Input: CoreAudio wants to READ audio FROM us (capture/microphone)
                // Read from FireWire RX ring buffer if available, else fill silence.
                IOAddressSegment segment{};
                kern_return_t kr = driverIvars->inputBuffer->GetAddressRange(&segment);
                if (kr == kIOReturnSuccess && segment.address != 0) {
                    // Calculate buffer offset from sampleTime
                    uint32_t bufferFrames = kZeroTimestampPeriod;
                    uint32_t offsetFrames = static_cast<uint32_t>(sampleTime % bufferFrames);
                    uint32_t firstFrames = ioBufferFrameSize;
                    uint32_t secondFrames = 0;
                    if ((offsetFrames + ioBufferFrameSize) > bufferFrames) {
                        firstFrames = bufferFrames - offsetFrames;
                        secondFrames = ioBufferFrameSize - firstFrames;
                    }

                    uint64_t offsetBytes = uint64_t(offsetFrames) * sizeof(int32_t) * driverIvars->channelCount;
                    size_t firstBytes = size_t(firstFrames) * sizeof(int32_t) * driverIvars->channelCount;
                    size_t secondBytes = size_t(secondFrames) * sizeof(int32_t) * driverIvars->channelCount;

                    // One-time startup drain: discard excess frames to reach target fill
                    if (driverIvars->rxQueueValid && !driverIvars->rxStartupDrained) {
                        uint32_t fill = driverIvars->rxQueueReader.FillLevelFrames();
                        constexpr uint32_t kRxTargetFill = 2048;
                        if (fill > kRxTargetFill + 256) {
                            uint32_t excess = fill - kRxTargetFill;
                            driverIvars->rxQueueReader.ConsumeFrames(excess);
                        }
                        driverIvars->rxStartupDrained = true;
                    }

                    // Read from shared RX queue if available
                    if (driverIvars->rxQueueValid) {
                        int32_t* pcmFirst = reinterpret_cast<int32_t*>(segment.address + offsetBytes);
                        uint32_t read1 = driverIvars->rxQueueReader.Read(pcmFirst, firstFrames);
                        if (read1 < firstFrames) {
                            memset(pcmFirst + read1 * driverIvars->channelCount, 0,
                                   size_t(firstFrames - read1) * sizeof(int32_t) * driverIvars->channelCount);
                        }
                        if (secondFrames > 0) {
                            int32_t* pcmSecond = reinterpret_cast<int32_t*>(segment.address);
                            if (read1 == firstFrames) {
                                uint32_t read2 = driverIvars->rxQueueReader.Read(pcmSecond, secondFrames);
                                if (read2 < secondFrames) {
                                    memset(pcmSecond + read2 * driverIvars->channelCount, 0,
                                           size_t(secondFrames - read2) * sizeof(int32_t) * driverIvars->channelCount);
                                }
                            } else {
                                memset(pcmSecond, 0, secondBytes);
                            }
                        }
                    } else {
                        // No RX queue: fill with silence
                        memset(reinterpret_cast<void*>(segment.address + offsetBytes), 0, firstBytes);
                        if (secondFrames > 0) {
                            memset(reinterpret_cast<void*>(segment.address), 0, secondBytes);
                        }
                    }
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
                    // CoreAudio writes at (sampleTime % bufferFrames) within the IO buffer.
                    // We must read from that offset, not always from the start.
                    uint32_t bufferFrames = kZeroTimestampPeriod;
                    uint32_t offsetFrames = static_cast<uint32_t>(sampleTime % bufferFrames);
                    uint64_t offsetBytes = uint64_t(offsetFrames) * sizeof(int32_t) * driverIvars->channelCount;
                    uint32_t firstFrames = ioBufferFrameSize;
                    uint32_t secondFrames = 0;
                    if ((offsetFrames + ioBufferFrameSize) > bufferFrames) {
                        firstFrames = bufferFrames - offsetFrames;
                        secondFrames = ioBufferFrameSize - firstFrames;
                    }

                    const int32_t* pcmDataFirst = reinterpret_cast<const int32_t*>(segment.address + offsetBytes);
                    const int32_t* pcmDataSecond = reinterpret_cast<const int32_t*>(segment.address);
                    uint32_t framesWritten = 0;
                    uint32_t framesRequested = ioBufferFrameSize;

                    // Write to shared TX queue if available, otherwise local buffer
                    if (driverIvars->txQueueValid) {
                        if (driverIvars->zeroCopyEnabled) {
                            // ZERO-COPY mode: samples are already in shared output buffer.
                            // Publish frame availability in sampleTime units and share
                            // phase so consumer maps queue index -> physical buffer offset.
                            auto& tl = driverIvars->zeroCopyTimeline;
                            bool rebased = false;
                            if (!tl.valid) {
                                tl.valid = true;
                                tl.lastSampleTime = sampleTime;
                                tl.publishedSampleTime = sampleTime;
                                rebased = true;
                            } else if (sampleTime < tl.lastSampleTime) {
                                // HAL restarted timeline (or discontinuity): rebase safely.
                                tl.discontinuities++;
                                tl.lastSampleTime = sampleTime;
                                tl.publishedSampleTime = sampleTime;
                                rebased = true;
                            } else {
                                tl.lastSampleTime = sampleTime;
                            }

                            if (rebased) {
                                const uint32_t bufferFrames = (driverIvars->zeroCopyFrameCapacity > 0)
                                                                 ? driverIvars->zeroCopyFrameCapacity
                                                                 : kZeroTimestampPeriod;
                                const uint32_t writeIdx = driverIvars->txQueueWriter.WriteIndexFrames();
                                const uint32_t samplePos = static_cast<uint32_t>(sampleTime % bufferFrames);
                                const uint32_t phase = (samplePos + bufferFrames - (writeIdx % bufferFrames)) % bufferFrames;
                                tl.phaseFrames = phase;
                                driverIvars->txQueueWriter.ProducerSetZeroCopyPhaseFrames(phase);
                                driverIvars->txQueueWriter.ProducerRequestConsumerResync();
                            }

                            uint64_t desiredPublishedSample = sampleTime + ioBufferFrameSize;

                            if (desiredPublishedSample < tl.publishedSampleTime) {
                                tl.discontinuities++;
                                tl.publishedSampleTime = sampleTime;
                                const uint32_t bufferFrames = (driverIvars->zeroCopyFrameCapacity > 0)
                                                                 ? driverIvars->zeroCopyFrameCapacity
                                                                 : kZeroTimestampPeriod;
                                const uint32_t writeIdx = driverIvars->txQueueWriter.WriteIndexFrames();
                                const uint32_t samplePos = static_cast<uint32_t>(sampleTime % bufferFrames);
                                const uint32_t phase = (samplePos + bufferFrames - (writeIdx % bufferFrames)) % bufferFrames;
                                tl.phaseFrames = phase;
                                driverIvars->txQueueWriter.ProducerSetZeroCopyPhaseFrames(phase);
                                driverIvars->txQueueWriter.ProducerRequestConsumerResync();
                                desiredPublishedSample = sampleTime + ioBufferFrameSize;
                            }

                            uint64_t toPublish64 = desiredPublishedSample - tl.publishedSampleTime;
                            uint32_t toPublish = (toPublish64 > 0xFFFFFFFFull)
                                                   ? 0xFFFFFFFFu
                                                   : static_cast<uint32_t>(toPublish64);
                            framesRequested = toPublish;
                            framesWritten = driverIvars->txQueueWriter.PublishFrames(toPublish);
                            tl.publishedSampleTime += framesWritten;
                        } else {
                            // Legacy mode: copy PCM payload into shared queue.
                            uint32_t w0 = driverIvars->txQueueWriter.Write(pcmDataFirst, firstFrames);
                            framesWritten = w0;
                            if (w0 == firstFrames && secondFrames > 0) {
                                framesWritten += driverIvars->txQueueWriter.Write(pcmDataSecond, secondFrames);
                            }
                        }

                    } else {
                        // Fallback to local buffer for debugging without IT
                        uint32_t w0 = driverIvars->packetAssembler.ringBuffer().write(pcmDataFirst, firstFrames);
                        framesWritten = w0;
                        if (w0 == firstFrames && secondFrames > 0) {
                            framesWritten += driverIvars->packetAssembler.ringBuffer().write(pcmDataSecond, secondFrames);
                        }
                    }

                    if (framesWritten < framesRequested) {
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
    
    // ========================================================================
    // ZERO-COPY: Try to get shared output buffer from ASFWAudioNub
    // If successful, CoreAudio writes here and IT DMA reads directly!
    // ========================================================================
    IOBufferMemoryDescriptor* sharedOutMem = nullptr;
    uint64_t sharedOutBytes = 0;
    if (kEnableZeroCopyOutputPath) {
        kern_return_t zeroCopyErr = nub->CopyOutputAudioMemory(&sharedOutMem, &sharedOutBytes);
        
        if (zeroCopyErr == kIOReturnSuccess && sharedOutMem && sharedOutBytes > 0) {
            // Store the shared buffer
            ivars->sharedOutputBuffer.reset(sharedOutMem, OSNoRetain);
            ivars->sharedOutputBytes = sharedOutBytes;
            
            // Create local mapping for IOUserAudioStream
            IOMemoryMap* sharedOutMap = nullptr;
            zeroCopyErr = sharedOutMem->CreateMapping(
                kIOMemoryMapCacheModeDefault,
                0,    // offset
                0,    // address
                0,    // length
                0,    // options
                &sharedOutMap);
            
            if (zeroCopyErr == kIOReturnSuccess && sharedOutMap) {
                ivars->sharedOutputMap.reset(sharedOutMap, OSNoRetain);
                ivars->zeroCopyEnabled = true;
                ivars->zeroCopyFrameCapacity =
                    static_cast<uint32_t>(sharedOutBytes / (sizeof(int32_t) * ivars->channelCount));

                // Use shared buffer for output stream (ZERO-COPY!)
                ivars->outputBuffer.reset(sharedOutMem, OSRetain);  // Keep a reference

                ASFW_LOG(Audio, "ASFWAudioDriver: ✅ ZERO-COPY enabled! Shared output buffer: %llu bytes (%u frames)",
                         sharedOutBytes, ivars->zeroCopyFrameCapacity);
            } else {
                ASFW_LOG(Audio, "ASFWAudioDriver: ZERO-COPY CreateMapping failed: 0x%x, falling back",
                         zeroCopyErr);
                ivars->sharedOutputBuffer.reset();
                ivars->sharedOutputBytes = 0;
                ivars->zeroCopyFrameCapacity = 0;
            }
        } else {
            ASFW_LOG(Audio, "ASFWAudioDriver: ZERO-COPY CopyOutputAudioMemory failed: 0x%x, using local buffer",
                     zeroCopyErr);
            ivars->zeroCopyFrameCapacity = 0;
        }
    } else {
        ivars->zeroCopyEnabled = false;
        ivars->sharedOutputMap.reset();
        ivars->sharedOutputBuffer.reset();
        ivars->sharedOutputBytes = 0;
        ivars->zeroCopyFrameCapacity = 0;
        ASFW_LOG(Audio, "ASFWAudioDriver: ZERO-COPY disabled by build flag; using TX queue path");
    }
    
    // Fallback: Create local output buffer if ZERO-COPY not available
    if (!ivars->zeroCopyEnabled) {
        error = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, bufferBytes, 0,
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
    auto outputName = OSSharedPtr(OSString::withCString(ivars->outputPlugName), OSNoRetain);
    ivars->outputStream->SetName(outputName.get());
    ivars->outputStream->SetAvailableStreamFormats(formats, formatCount);
    ivars->outputStream->SetCurrentStreamFormat(&formats[0]);  // Initial format

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

    // Reset RX startup drain flag so first BeginRead discards excess
    ivars->rxStartupDrained = false;

    // Reset zero-copy timeline tracking for this session
    ivars->zeroCopyTimeline.valid = false;
    ivars->zeroCopyTimeline.lastSampleTime = 0;
    ivars->zeroCopyTimeline.publishedSampleTime = 0;
    ivars->zeroCopyTimeline.discontinuities = 0;
    ivars->zeroCopyTimeline.phaseFrames = 0;
    
    // Calculate timer interval based on current sample rate
    struct mach_timebase_info timebase_info;
    mach_timebase_info(&timebase_info);
    
    double sample_rate = ivars->currentSampleRate;
    double host_ticks_per_buffer = static_cast<double>(kZeroTimestampPeriod * NSEC_PER_SEC) / sample_rate;
    host_ticks_per_buffer = (host_ticks_per_buffer * static_cast<double>(timebase_info.denom)) 
                          / static_cast<double>(timebase_info.numer);
    ivars->hostTicksPerBuffer = static_cast<uint64_t>(host_ticks_per_buffer);

    // Initialize clock sync PLL
    ivars->clockSync.nominalTicksPerBuffer = host_ticks_per_buffer;
    ivars->clockSync.currentTicksPerBuffer = host_ticks_per_buffer;
    ivars->clockSync.fractionalTicks = 0.0;
    ivars->clockSync.fillErrorIntegral = 0;
    ivars->clockSync.lastFillError = 0;
    ivars->clockSync.adjustmentCount = 0;
    ivars->clockSync.maxCorrectionPpm = 0.0;

    // RX clock sync: cycle-time rate estimation initialized by controller's IR Poll.
    // No local state to reset — corrHostNanosPerSampleQ8 lives in shared queue header.

    // Ask IT consumer to flush stale queue data at next refill.
    if (ivars->txQueueValid) {
        ivars->txQueueWriter.ProducerSetZeroCopyPhaseFrames(0);
        ivars->txQueueWriter.ProducerRequestConsumerResync();
    }

    // Target fill for PLL:
    // - Legacy queue mode: half of queue capacity
    // - Zero-copy mode: half of shared output buffer (avoid stale-buffer reads)
    if (ivars->txQueueValid) {
        if (ivars->zeroCopyEnabled && ivars->zeroCopyFrameCapacity > 0) {
            // Keep read pointer comfortably behind writes in the small 512-frame
            // shared output buffer while preserving some jitter headroom.
            uint32_t target = (ivars->zeroCopyFrameCapacity * 5) / 8;  // 320 for 512-frame buffer
            if (target < 8) target = 8;
            ivars->clockSync.targetFillLevel = target;
        } else {
            // Legacy queue-copy path drains quickly into IT ring, so queue fill is small.
            // Use a low target to avoid permanent -100ppm clamp.
            ivars->clockSync.targetFillLevel = 64;
        }
    } else {
        ivars->clockSync.targetFillLevel = 2048;  // Default for 4096-frame queue
    }

    ASFW_LOG(Audio, "ASFWAudioDriver: Clock sync target fill=%u (zeroCopy=%s)",
             ivars->clockSync.targetFillLevel,
             ivars->zeroCopyEnabled ? "YES" : "NO");

    ASFW_LOG(Audio, "ASFWAudioDriver: Timer interval = %llu ticks (%.0f Hz, period=%u frames)",
             ivars->hostTicksPerBuffer, sample_rate, kZeroTimestampPeriod);
    
    // Clear timestamps to initial state
    ivars->audioDevice->UpdateCurrentZeroTimestamp(0, 0);
    
    // Start the timer - first callback will set initial timestamp
    uint64_t current_time = mach_absolute_time();
    ivars->timestampTimer->WakeAtTime(kIOTimerClockMachAbsoluteTime, 
                                       current_time + ivars->hostTicksPerBuffer, 0);
    ivars->timestampTimer->SetEnable(true);
    
    ivars->isRunning.store(true, std::memory_order_release);
    ASFW_LOG(Audio, "ASFWAudioDriver: Timestamp timer started");
    
    return kIOReturnSuccess;
}

kern_return_t ASFWAudioDriver::StopDevice(IOUserAudioObjectID in_object_id,
                                           IOUserAudioStartStopFlags in_flags)
{
    ASFW_LOG(Audio, "ASFWAudioDriver: StopDevice(id=%u)", in_object_id);
    
    ivars->isRunning.store(false, std::memory_order_release);

    // Reset clock sync state
    ivars->clockSync.fillErrorIntegral = 0;
    ivars->clockSync.lastFillError = 0;
    ivars->clockSync.fractionalTicks = 0.0;
    ivars->clockSync.adjustmentCount = 0;
    ivars->clockSync.maxCorrectionPpm = 0.0;

    ivars->zeroCopyTimeline.valid = false;

    if (ivars && ivars->timestampTimer) {
        ivars->timestampTimer->SetEnable(false);
        ASFW_LOG(Audio, "ASFWAudioDriver: Timestamp timer stopped");
    }

    return kIOReturnSuccess;
}

// Timer callback - called periodically to update zero timestamps
void ASFWAudioDriver::ZtsTimerOccurred_Impl(OSAction* action, uint64_t time)
{
    if (!ivars || !ivars->isRunning.load(std::memory_order_acquire) || !ivars->audioDevice) {
        return;
    }
    
    const bool localEncodingActive = !ivars->txQueueValid;

    // Read RX fill level directly from shared memory queue (no RPC needed)
    uint32_t rxFill = 0;
    bool rxPllReady = false;
    if (ivars->rxQueueValid) {
        rxFill = ivars->rxQueueReader.FillLevelFrames();
        rxPllReady = true;
    }

    // Get current timestamps
    uint64_t current_sample_time = 0;
    uint64_t current_host_time = 0;
    ivars->audioDevice->GetCurrentZeroTimestamp(&current_sample_time, &current_host_time);

    // --- Clock sync: Adjust ticks based on cycle-time rate or fill level feedback ---
    // Priority: cycle-time q8 > TX zero-copy PLL > RX nominal > legacy TX nominal
    // One clock device, one rate: cycle-time syncs both TX and RX to the FW bus clock.
    uint64_t host_ticks_per_buffer = static_cast<uint64_t>(ivars->clockSync.currentTicksPerBuffer);

    // Priority 1: Cycle-time rate from RX queue (syncs both TX and RX to bus clock)
    uint32_t q8 = ivars->rxQueueValid
                ? ivars->rxQueueReader.CorrHostNanosPerSampleQ8() : 0;

    if (q8 > 0) {
        double nanosPerSample = q8 / 256.0;
        struct mach_timebase_info tb;
        mach_timebase_info(&tb);
        double hostTicksPerSample = nanosPerSample * static_cast<double>(tb.denom)
                                  / static_cast<double>(tb.numer);
        ivars->clockSync.currentTicksPerBuffer = hostTicksPerSample * kZeroTimestampPeriod;

        double exactTicks = ivars->clockSync.currentTicksPerBuffer + ivars->clockSync.fractionalTicks;
        host_ticks_per_buffer = static_cast<uint64_t>(exactTicks);
        ivars->clockSync.fractionalTicks = exactTicks - static_cast<double>(host_ticks_per_buffer);

    } else if (ivars->zeroCopyEnabled && ivars->txQueueValid) {
        // Priority 2: TX zero-copy fill-level PLL (fallback until q8 arrives)
        uint32_t fillLevel = ivars->txQueueWriter.FillLevelFrames();
        int32_t fillError = static_cast<int32_t>(fillLevel)
                          - static_cast<int32_t>(ivars->clockSync.targetFillLevel);

        constexpr double kMaxPpm = 100.0;
        constexpr int32_t kDeadbandFrames = 8;
        constexpr double kPpmPerFrame = 0.45;
        constexpr double kIppmPerFrameTick = 0.0008;
        constexpr int64_t kIntegralClamp = 200000;

        int32_t controlError = fillError;
        if (std::abs(controlError) <= kDeadbandFrames) {
            controlError = 0;
        }

        double ppmUnclamped = (kPpmPerFrame * controlError)
                            + (kIppmPerFrameTick * static_cast<double>(ivars->clockSync.fillErrorIntegral));
        bool satHigh = (ppmUnclamped > kMaxPpm) && (controlError > 0);
        bool satLow  = (ppmUnclamped < -kMaxPpm) && (controlError < 0);

        if (!(satHigh || satLow)) {
            ivars->clockSync.fillErrorIntegral += controlError;
            if (ivars->clockSync.fillErrorIntegral > kIntegralClamp)
                ivars->clockSync.fillErrorIntegral = kIntegralClamp;
            if (ivars->clockSync.fillErrorIntegral < -kIntegralClamp)
                ivars->clockSync.fillErrorIntegral = -kIntegralClamp;
        }

        ppmUnclamped = (kPpmPerFrame * controlError)
                     + (kIppmPerFrameTick * static_cast<double>(ivars->clockSync.fillErrorIntegral));
        double corrPpm = ppmUnclamped;
        if (corrPpm > kMaxPpm) corrPpm = kMaxPpm;
        if (corrPpm < -kMaxPpm) corrPpm = -kMaxPpm;

        double correction = ivars->clockSync.nominalTicksPerBuffer * (corrPpm / 1e6);
        ivars->clockSync.currentTicksPerBuffer = ivars->clockSync.nominalTicksPerBuffer + correction;
        ivars->clockSync.lastFillError = fillError;
        ivars->clockSync.adjustmentCount++;

        if (std::fabs(corrPpm) > ivars->clockSync.maxCorrectionPpm) {
            ivars->clockSync.maxCorrectionPpm = std::fabs(corrPpm);
        }

        double exactTicks = ivars->clockSync.currentTicksPerBuffer + ivars->clockSync.fractionalTicks;
        host_ticks_per_buffer = static_cast<uint64_t>(exactTicks);
        ivars->clockSync.fractionalTicks = exactTicks - static_cast<double>(host_ticks_per_buffer);

    } else if (rxPllReady) {
        // Priority 3: RX queue present but no q8 yet — use nominal
        double exactTicks = ivars->clockSync.currentTicksPerBuffer + ivars->clockSync.fractionalTicks;
        host_ticks_per_buffer = static_cast<uint64_t>(exactTicks);
        ivars->clockSync.fractionalTicks = exactTicks - static_cast<double>(host_ticks_per_buffer);

    } else if (ivars->txQueueValid && !ivars->zeroCopyEnabled) {
        // Priority 4: Legacy TX queue-copy path: bursty fill signal, keep nominal clocking
        uint32_t fillLevel = ivars->txQueueWriter.FillLevelFrames();
        int32_t fillError = static_cast<int32_t>(fillLevel)
                          - static_cast<int32_t>(ivars->clockSync.targetFillLevel);
        ivars->clockSync.lastFillError = fillError;
        ivars->clockSync.fillErrorIntegral = 0;
        ivars->clockSync.currentTicksPerBuffer = ivars->clockSync.nominalTicksPerBuffer;
        ivars->clockSync.fractionalTicks = 0.0;
        ivars->clockSync.maxCorrectionPpm = 0.0;
        host_ticks_per_buffer = static_cast<uint64_t>(ivars->clockSync.nominalTicksPerBuffer);
    }

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
        
        // Ring buffer metrics are meaningful only on local (non-shared-queue) encoding path.
        uint32_t ringFillLevel = 0;
        uint64_t ringUnderruns = 0;
        if (localEncodingActive) {
            ringFillLevel = ivars->packetAssembler.bufferFillLevel();
            ringUnderruns = ivars->packetAssembler.underrunCount();
        }
        
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
            
            ASFW_LOG(Audio, "IO: %.1fs recv=%llu sent=%llu (%.0f/s) cb=%llu ring=%u rxFill=%u overruns=%llu underruns=%llu/%llu | LocalEnc:%{public}s %llu pkts (%.0f/s, D:%llu N:%llu)",
                     elapsedSec, framesReceived, framesSent, framesPerSec, callbacks, ringFillLevel, rxFill,
                     ivars->encodingMetrics.overruns, underruns, ringUnderruns,
                     localEncodingActive ? "ON" : "OFF",
                     ivars->encodingMetrics.packetsGenerated, packetsPerSec,
                     ivars->encodingMetrics.dataPackets, ivars->encodingMetrics.noDataPackets);

            // Clock sync diagnostics
            double corrPpm = ((ivars->clockSync.currentTicksPerBuffer
                               - ivars->clockSync.nominalTicksPerBuffer)
                              / ivars->clockSync.nominalTicksPerBuffer) * 1e6;
            if (q8 > 0) {
                uint32_t txFill = ivars->txQueueValid ? ivars->txQueueWriter.FillLevelFrames() : 0;
                ASFW_LOG(Audio, "CLK: q8=%u corr=%.1f ppm rxFill=%u txFill=%u (cycle-time, unified)",
                         q8, corrPpm, rxFill, txFill);
            } else if (ivars->zeroCopyEnabled && ivars->txQueueValid) {
                uint32_t fill = ivars->txQueueWriter.FillLevelFrames();
                ASFW_LOG(Audio, "CLK-TX: fill=%u target=%u err=%d integral=%lld corr=%.1f ppm (max=%.1f) zcDisc=%llu",
                         fill, ivars->clockSync.targetFillLevel,
                         ivars->clockSync.lastFillError,
                         ivars->clockSync.fillErrorIntegral,
                         corrPpm, ivars->clockSync.maxCorrectionPpm,
                         ivars->zeroCopyTimeline.discontinuities);
            } else if (rxPllReady) {
                ASFW_LOG(Audio, "CLK-RX: fill=%u corr=0.0 ppm q8=0 (awaiting cycle-time)",
                         rxFill);
            } else if (ivars->txQueueValid) {
                uint32_t fill = ivars->txQueueWriter.FillLevelFrames();
                ASFW_LOG(Audio, "CLK: fill=%u target=%u err=%d nominal (legacy TX path)",
                         fill, ivars->clockSync.targetFillLevel,
                         ivars->clockSync.lastFillError);
            }

            ivars->encodingMetrics.lastLogPackets = ivars->encodingMetrics.packetsGenerated;
            ivars->encodingMetrics.lastLogElapsedSec = elapsedSec;
        }
    }
    
    // Consume audio from ring buffer and validate encoding
    // Timer fires at buffer rate (~86Hz for 512 frames), so we need to process
    // all accumulated audio each tick to keep the ring buffer from overflowing.
    // At 48kHz with 512-frame buffers:
    //   blocking mode: 8 samples per DATA packet (+ periodic NO-DATA)
    //   non-blocking mode: 6 samples per packet (DATA every cycle)
    // Drain until ring buffer has less than one packet's worth of samples
    if (localEncodingActive) {
        while (ivars->packetAssembler.bufferFillLevel() >= ivars->packetAssembler.samplesPerDataPacket()) {
            auto pkt = ivars->packetAssembler.assembleNext(0xFFFF);  // Phase 1.5: dummy SYT
            ivars->encodingMetrics.packetsGenerated++;
            if (pkt.isData) {
                ivars->encodingMetrics.dataPackets++;
            } else {
                ivars->encodingMetrics.noDataPackets++;
            }
        }
    }
}

