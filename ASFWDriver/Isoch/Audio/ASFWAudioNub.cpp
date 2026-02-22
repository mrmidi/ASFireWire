//
// ASFWAudioNub.cpp
// ASFWDriver
//
// Implementation of audio nub published by ASFWDriver
// Manages shared memory for cross-process TX audio queue
//

#include "ASFWAudioNub.h"
#include "ASFWDriver.h"
#include "../../Controller/ControllerCore.hpp"
#include "../../Discovery/DeviceRegistry.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Protocols/AVC/IAVCDiscovery.hpp"
#include "../../Protocols/Audio/IDeviceProtocol.hpp"
#include "../../Shared/TxSharedQueue.hpp"

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>

// TX queue capacity: 4096 frames = ~85ms @ 48kHz
// This provides good buffering without excessive latency
static constexpr uint32_t kTxQueueCapacityFrames = 4096;

// ZERO-COPY: Output audio buffer size
// Match kZeroTimestampPeriod from ASFWAudioDriver (512 frames)
static constexpr uint32_t kOutputAudioBufferFrames = 512;
// RX queue capacity: 4096 frames = ~85ms @ 48kHz (matches AudioRingBuffer)
static constexpr uint32_t kRxQueueCapacityFrames = 4096;

static constexpr uint8_t kAutoIsochReceiveChannel = 0;   // Device TX -> Host IR
static constexpr uint8_t kAutoIsochTransmitChannel = 1;  // Host IT -> Device RX

static ASFWDriver* GetParentASFWDriver(const ASFWAudioNub_IVars* iv)
{
    if (!iv || !iv->parentDriver) {
        return nullptr;
    }
    return OSDynamicCast(ASFWDriver, iv->parentDriver);
}

struct ProtocolRuntimeBinding {
    ASFW::Discovery::DeviceRecord* device{nullptr};
    ASFW::Audio::IDeviceProtocol* protocol{nullptr};
    ASFW::Protocols::AVC::IAVCDiscovery* avcDiscovery{nullptr};
};

static kern_return_t ResolveProtocolRuntimeBinding(const ASFWAudioNub_IVars* iv,
                                                   ProtocolRuntimeBinding& outBinding)
{
    if (!iv || iv->guid == 0) {
        return kIOReturnNotReady;
    }

    const ASFWDriver* parent = GetParentASFWDriver(iv);
    if (!parent) {
        return kIOReturnNotReady;
    }

    const auto* controllerCore = static_cast<ASFW::Driver::ControllerCore*>(parent->GetControllerCore());
    if (!controllerCore) {
        return kIOReturnNotReady;
    }

    auto* registry = controllerCore->GetDeviceRegistry();
    if (!registry) {
        return kIOReturnNotReady;
    }

    auto* device = registry->FindByGuid(iv->guid);
    if (!device) {
        return kIOReturnNotFound;
    }
    if (!device->protocol) {
        return kIOReturnUnsupported;
    }

    auto* avcDiscovery = controllerCore->GetAVCDiscovery();
    if (!avcDiscovery) {
        return kIOReturnNotReady;
    }

    outBinding.device = device;
    outBinding.protocol = device->protocol.get();
    outBinding.avcDiscovery = avcDiscovery;
    return kIOReturnSuccess;
}

static void ConfigureDeviceDuplex48kBestEffort(const ASFWDriver* parent)
{
    if (!parent) {
        return;
    }

    const auto* controllerCore = static_cast<ASFW::Driver::ControllerCore*>(parent->GetControllerCore());
    if (!controllerCore) {
        ASFW_LOG(Audio, "ASFWAudioNub: AutoStart: missing ControllerCore");
        return;
    }

    const auto* registry = controllerCore->GetDeviceRegistry();
    const auto topology = controllerCore->LatestTopology();
    if (!registry || !topology.has_value()) {
        ASFW_LOG(Audio, "ASFWAudioNub: AutoStart: missing DeviceRegistry/Topology");
        return;
    }

    auto devices = registry->LiveDevices(static_cast<ASFW::Discovery::Generation>(topology->generation));
    for (auto& device : devices) {
        if (!device.protocol) {
            continue;
        }

        if (const IOReturn status = device.protocol->StartDuplex48k(); status == kIOReturnSuccess) {
            ASFW_LOG(Audio, "ASFWAudioNub: Device duplex configured at 48kHz (GUID=%llx)", device.guid);
        } else {
            ASFW_LOG(Audio, "ASFWAudioNub: Device duplex config failed (GUID=%llx status=0x%x)",
                     device.guid, status);
        }
        return;
    }

    ASFW_LOG(Audio, "ASFWAudioNub: AutoStart: no protocol device available for duplex config");
}

static void AutoStartStreamsIfNeeded(const ASFWAudioNub* self, const ASFWAudioNub_IVars* iv)
{
    (void)self;
    if (!iv) {
        return;
    }

    ASFWDriver* parent = GetParentASFWDriver(iv);
    if (!parent) {
        return;
    }

    const bool irRunning = parent->GetIsochReceiveContext() != nullptr;
    const bool itRunning = parent->GetIsochTransmitContext() != nullptr;
    if (irRunning && itRunning) {
        return;
    }

    ConfigureDeviceDuplex48kBestEffort(parent);

    ASFW_LOG(Audio, "ASFWAudioNub: AutoStart -> duplex staged (IR ch%u, IT ch%u)",
             kAutoIsochReceiveChannel, kAutoIsochTransmitChannel);

    if (!irRunning) {
        if (const kern_return_t irKr = parent->StartIsochReceive(kAutoIsochReceiveChannel);
            irKr == kIOReturnSuccess) {
            ASFW_LOG(Audio, "ASFWAudioNub: ✅ IR started");
        } else {
            ASFW_LOG(Audio, "ASFWAudioNub: StartIsochReceive failed: 0x%x", irKr);
        }
        // Stage IR first; IT is retried on later RPC once RX SYT has progressed.
        return;
    }

    if (!itRunning) {
        const kern_return_t itKr = parent->StartIsochTransmit(kAutoIsochTransmitChannel);
        if (itKr == kIOReturnSuccess) {
            ASFW_LOG(Audio, "ASFWAudioNub: ✅ IT started");
        } else {
            ASFW_LOG(Audio, "ASFWAudioNub: StartIsochTransmit failed: 0x%x", itKr);
        }
    }
}

static void AutoStopStreamsBestEffort(const ASFWAudioNub* self, const ASFWAudioNub_IVars* iv)
{
    (void)self;
    if (!iv) {
        return;
    }
    ASFWDriver* parent = GetParentASFWDriver(iv);
    if (!parent) {
        return;
    }

    ASFW_LOG(Audio, "ASFWAudioNub: AutoStop -> duplex");
    parent->StopIsochReceive();
    parent->StopIsochTransmit();
}

// Helper to create and initialize the TX queue
static kern_return_t CreateTxQueue(ASFWAudioNub_IVars* iv)
{
    if (!iv) return kIOReturnBadArgument;

    // Already created?
    if (iv->txQueueMem && iv->txQueueMap) {
        return kIOReturnSuccess;
    }

    if (iv->channelCount == 0 || iv->channelCount > 16) {
        ASFW_LOG(Audio, "ASFWAudioNub: CreateTxQueue: invalid channelCount=%u (SetChannelCount not called?)",
                 iv->channelCount);
        return kIOReturnNotReady;
    }

    const uint64_t bytes = ASFW::Shared::TxSharedQueueSPSC::RequiredBytes(kTxQueueCapacityFrames, iv->channelCount);

    // Allocate IOBufferMemoryDescriptor
    IOBufferMemoryDescriptor* mem = nullptr;
    kern_return_t kr = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionOutIn,  // Bidirectional for producer/consumer
        bytes,
        64,  // 64-byte alignment for cache lines
        &mem);

    if (kr != kIOReturnSuccess || !mem) {
        ASFW_LOG(Audio, "ASFWAudioNub: Failed to create IOBufferMemoryDescriptor: 0x%x", kr);
        return kr ? kr : kIOReturnNoMemory;
    }

    kr = mem->SetLength(bytes);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioNub: SetLength failed: 0x%x", kr);
        mem->release();
        return kr;
    }

    // Create local mapping for initialization and IT context access
    IOMemoryMap* map = nullptr;
    kr = mem->CreateMapping(
        kIOMemoryMapCacheModeDefault,
        0,    // offset
        0,    // address (0 = kernel chooses)
        0,    // length (0 = entire descriptor)
        0,    // options
        &map);

    if (kr != kIOReturnSuccess || !map) {
        ASFW_LOG(Audio, "ASFWAudioNub: CreateMapping failed: 0x%x", kr);
        mem->release();
        return kr ? kr : kIOReturnNoMemory;
    }

    // Initialize SPSC queue in shared memory
    auto* base = reinterpret_cast<void*>(map->GetAddress());
    if (const bool initOk = ASFW::Shared::TxSharedQueueSPSC::InitializeInPlace(
            base, bytes, kTxQueueCapacityFrames, iv->channelCount);
        !initOk) {
        ASFW_LOG(Audio, "ASFWAudioNub: TxSharedQueue initialization failed");
        map->release();
        mem->release();
        return kIOReturnError;
    }

    iv->txQueueMem = mem;    // retained
    iv->txQueueMap = map;    // retained
    iv->txQueueBytes = bytes;

    ASFW_LOG(Audio, "ASFWAudioNub: TX queue created: %llu bytes, %u frames capacity, base=%p",
             bytes, kTxQueueCapacityFrames, base);

    return kIOReturnSuccess;
}

// Helper to create and initialize the RX queue (mirrors CreateTxQueue)
static kern_return_t CreateRxQueue(ASFWAudioNub_IVars* iv)
{
    if (!iv) return kIOReturnBadArgument;

    // Already created?
    if (iv->rxQueueMem && iv->rxQueueMap) {
        return kIOReturnSuccess;
    }

    if (iv->channelCount == 0 || iv->channelCount > 16) {
        ASFW_LOG(Audio, "ASFWAudioNub: CreateRxQueue: invalid channelCount=%u (SetChannelCount not called?)",
                 iv->channelCount);
        return kIOReturnNotReady;
    }

    const uint64_t bytes = ASFW::Shared::TxSharedQueueSPSC::RequiredBytes(kRxQueueCapacityFrames, iv->channelCount);

    // Allocate IOBufferMemoryDescriptor
    IOBufferMemoryDescriptor* mem = nullptr;
    kern_return_t kr = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionOutIn,  // Bidirectional for producer/consumer
        bytes,
        64,  // 64-byte alignment for cache lines
        &mem);

    if (kr != kIOReturnSuccess || !mem) {
        ASFW_LOG(Audio, "ASFWAudioNub: Failed to create RX queue IOBufferMemoryDescriptor: 0x%x", kr);
        return kr ? kr : kIOReturnNoMemory;
    }

    kr = mem->SetLength(bytes);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioNub: RX queue SetLength failed: 0x%x", kr);
        mem->release();
        return kr;
    }

    // Create local mapping for initialization and IR context access
    IOMemoryMap* map = nullptr;
    kr = mem->CreateMapping(
        kIOMemoryMapCacheModeDefault,
        0,    // offset
        0,    // address (0 = kernel chooses)
        0,    // length (0 = entire descriptor)
        0,    // options
        &map);

    if (kr != kIOReturnSuccess || !map) {
        ASFW_LOG(Audio, "ASFWAudioNub: RX queue CreateMapping failed: 0x%x", kr);
        mem->release();
        return kr ? kr : kIOReturnNoMemory;
    }

    // Initialize SPSC queue in shared memory
    auto* base = reinterpret_cast<void*>(map->GetAddress());
    if (const bool initOk = ASFW::Shared::TxSharedQueueSPSC::InitializeInPlace(
            base, bytes, kRxQueueCapacityFrames, iv->channelCount);
        !initOk) {
        ASFW_LOG(Audio, "ASFWAudioNub: RX shared queue initialization failed");
        map->release();
        mem->release();
        return kIOReturnError;
    }

    iv->rxQueueMem = mem;    // retained
    iv->rxQueueMap = map;    // retained
    iv->rxQueueBytes = bytes;

    ASFW_LOG(Audio, "ASFWAudioNub: RX queue created: %llu bytes, %u frames capacity, base=%p",
             bytes, kRxQueueCapacityFrames, base);

    return kIOReturnSuccess;
}

bool ASFWAudioNub::init()
{
    if (const bool result = super::init(); !result) {
        ASFW_LOG(Audio, "ASFWAudioNub: super::init() failed");
        return false;
    }

    ivars = IONewZero(ASFWAudioNub_IVars, 1);
    if (!ivars) {
        ASFW_LOG(Audio, "ASFWAudioNub: Failed to allocate ivars");
        return false;
    }

    ivars->parentDriver = nullptr;
    ivars->txQueueMem = nullptr;
    ivars->txQueueMap = nullptr;
    ivars->txQueueBytes = 0;
    ivars->guid = 0;
    
    // ZERO-COPY: Initialize output audio buffer ivars
    ivars->outputAudioMem = nullptr;
    ivars->outputAudioMap = nullptr;
    ivars->outputAudioBytes = 0;
    ivars->outputAudioFrameCapacity = 0;
    ivars->streamModeRaw = 0;

    ASFW_LOG(Audio, "ASFWAudioNub: init() succeeded");
    return true;
}

void ASFWAudioNub::free()
{
    ASFW_LOG(Audio, "ASFWAudioNub: free()");
    if (ivars) {
        // Release ZERO-COPY output audio buffer
        if (ivars->outputAudioMap) {
            ivars->outputAudioMap->release();
            ivars->outputAudioMap = nullptr;
        }
        if (ivars->outputAudioMem) {
            ivars->outputAudioMem->release();
            ivars->outputAudioMem = nullptr;
        }
        
        // Release shared RX queue memory resources
        if (ivars->rxQueueMap) {
            ivars->rxQueueMap->release();
            ivars->rxQueueMap = nullptr;
        }
        if (ivars->rxQueueMem) {
            ivars->rxQueueMem->release();
            ivars->rxQueueMem = nullptr;
        }

        // Release shared TX queue memory resources
        if (ivars->txQueueMap) {
            ivars->txQueueMap->release();
            ivars->txQueueMap = nullptr;
        }
        if (ivars->txQueueMem) {
            ivars->txQueueMem->release();
            ivars->txQueueMem = nullptr;
        }
        IOSafeDeleteNULL(ivars, ASFWAudioNub_IVars, 1);
    }
    super::free();
}

kern_return_t IMPL(ASFWAudioNub, Start)
{
    kern_return_t error = Start(provider, SUPERDISPATCH);
    if (error != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioNub: super::Start() failed: %d", error);
        return error;
    }

    // Store reference to parent driver (ASFWDriver)
    ivars->parentDriver = provider;

    // Channel count is set directly via SetChannelCount() by AVCDiscovery
    // TX queue and audio buffer are created lazily on first RPC access
    // (CopyTransmitQueueMemory / CopyOutputAudioMemory)

    // Register the service so ASFWAudioDriver can match on us
    error = RegisterService();
    if (error != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioNub: RegisterService() failed: %d", error);
        return error;
    }

    ASFW_LOG(Audio, "ASFWAudioNub[%p]: Started and registered", this);
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWAudioNub, Stop)
{
    ASFW_LOG(Audio, "ASFWAudioNub: Stop()");
    if (ivars) {
        AutoStopStreamsBestEffort(this, ivars);
        ivars->parentDriver = nullptr;
        // Note: Don't release txQueueMem/Map here - they may still be in use
        // They will be released in free()
    }
    return Stop(provider, SUPERDISPATCH);
}

// RPC method callable from ASFWAudioDriver (different process)
kern_return_t IMPL(ASFWAudioNub, CopyTransmitQueueMemory)
{
    ASFW_LOG(Audio, "ASFWAudioNub: CopyTransmitQueueMemory called");

    if (!outMemory || !outBytes) {
        ASFW_LOG(Audio, "ASFWAudioNub: CopyTransmitQueueMemory: bad arguments");
        return kIOReturnBadArgument;
    }

    if (!ivars) {
        ASFW_LOG(Audio, "ASFWAudioNub: CopyTransmitQueueMemory: no ivars");
        return kIOReturnNotReady;
    }

    // Ensure TX queue exists
    if (const kern_return_t kr = CreateTxQueue(ivars); kr != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioNub: CopyTransmitQueueMemory: CreateTxQueue failed: 0x%x", kr);
        return kr;
    }

    // Return retained reference - caller must release
    ivars->txQueueMem->retain();
    *outMemory = ivars->txQueueMem;
    *outBytes = ivars->txQueueBytes;

    ASFW_LOG(Audio, "ASFWAudioNub: CopyTransmitQueueMemory: returning mem=%p bytes=%llu",
             ivars->txQueueMem, ivars->txQueueBytes);

    // Audio driver calls this during Start(); treat that as the post-create hook.
    AutoStartStreamsIfNeeded(this, ivars);

    return kIOReturnSuccess;
}

// LOCALONLY: Get parent driver pointer (same process)
ASFWDriver* ASFWAudioNub::GetParentDriver() const
{
    return ivars ? OSDynamicCast(ASFWDriver, ivars->parentDriver) : nullptr;
}

// LOCALONLY: Get local mapping base address for IT context
uint8_t* ASFWAudioNub::GetTxQueueLocalMapping() const
{
    if (!ivars || !ivars->txQueueMap) {
        return nullptr;
    }
    return reinterpret_cast<uint8_t*>(ivars->txQueueMap->GetAddress());
}

// LOCALONLY: Get TX queue size
uint64_t ASFWAudioNub::GetTxQueueBytes() const
{
    return ivars ? ivars->txQueueBytes : 0;
}

// ============================================================================
// ZERO-COPY: Output Audio Buffer for IOUserAudioStream AND IT DMA
// ============================================================================

// Helper to create the shared output audio buffer
static kern_return_t CreateOutputAudioBuffer(ASFWAudioNub_IVars* iv)
{
    if (!iv) return kIOReturnBadArgument;

    // Already created?
    if (iv->outputAudioMem && iv->outputAudioMap) {
        return kIOReturnSuccess;
    }

    if (iv->channelCount == 0 || iv->channelCount > 16) {
        ASFW_LOG(Audio, "ASFWAudioNub: CreateOutputAudioBuffer: invalid channelCount=%u (SetChannelCount not called?)",
                 iv->channelCount);
        return kIOReturnNotReady;
    }

    const uint32_t bytesPerFrame = iv->channelCount * sizeof(int32_t);
    const uint64_t bufferBytes = uint64_t(kOutputAudioBufferFrames) * bytesPerFrame;

    // Create IOBufferMemoryDescriptor for shared audio buffer
    IOBufferMemoryDescriptor* mem = nullptr;
    kern_return_t kr = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionInOut,  // CoreAudio writes, IT DMA reads
        bufferBytes,
        64,  // 64-byte alignment for cache coherency
        &mem);

    if (kr != kIOReturnSuccess || !mem) {
        ASFW_LOG(Audio, "ASFWAudioNub: Failed to create output audio buffer: 0x%x", kr);
        return kr ? kr : kIOReturnNoMemory;
    }

    kr = mem->SetLength(bufferBytes);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioNub: Output audio SetLength failed: 0x%x", kr);
        mem->release();
        return kr;
    }

    // Create local mapping for IT DMA access
    IOMemoryMap* map = nullptr;
    kr = mem->CreateMapping(
        kIOMemoryMapCacheModeDefault,
        0,    // offset
        0,    // address (kernel chooses)
        0,    // length (entire descriptor)
        0,    // options
        &map);

    if (kr != kIOReturnSuccess || !map) {
        ASFW_LOG(Audio, "ASFWAudioNub: Output audio CreateMapping failed: 0x%x", kr);
        mem->release();
        return kr ? kr : kIOReturnNoMemory;
    }

    // Zero the buffer initially
    auto* base = reinterpret_cast<void*>(map->GetAddress());
    memset(base, 0, bufferBytes);

    iv->outputAudioMem = mem;    // retained
    iv->outputAudioMap = map;    // retained
    iv->outputAudioBytes = bufferBytes;
    iv->outputAudioFrameCapacity = kOutputAudioBufferFrames;

    ASFW_LOG(Audio, "ASFWAudioNub: ZERO-COPY output audio buffer created: %llu bytes, %u frames (%u ch), base=%p",
             bufferBytes, kOutputAudioBufferFrames, iv->channelCount, base);

    return kIOReturnSuccess;
}

// RPC callable from ASFWAudioDriver to get shared output audio buffer
kern_return_t IMPL(ASFWAudioNub, CopyOutputAudioMemory)
{
    ASFW_LOG(Audio, "ASFWAudioNub: CopyOutputAudioMemory called");

    if (!outMemory || !outBytes) {
        ASFW_LOG(Audio, "ASFWAudioNub: CopyOutputAudioMemory: bad arguments");
        return kIOReturnBadArgument;
    }

    if (!ivars) {
        ASFW_LOG(Audio, "ASFWAudioNub: CopyOutputAudioMemory: no ivars");
        return kIOReturnNotReady;
    }

    // Ensure output audio buffer exists
    if (const kern_return_t kr = CreateOutputAudioBuffer(ivars); kr != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioNub: CopyOutputAudioMemory: CreateOutputAudioBuffer failed: 0x%x", kr);
        return kr;
    }

    // Return retained reference - caller must release
    ivars->outputAudioMem->retain();
    *outMemory = ivars->outputAudioMem;
    *outBytes = ivars->outputAudioBytes;

    ASFW_LOG(Audio, "ASFWAudioNub: CopyOutputAudioMemory: returning mem=%p bytes=%llu frames=%u",
             ivars->outputAudioMem, ivars->outputAudioBytes, ivars->outputAudioFrameCapacity);

    // Also trigger from ZERO-COPY path (if used).
    AutoStartStreamsIfNeeded(this, ivars);

    return kIOReturnSuccess;
}

// LOCALONLY: Get local mapping for IT DMA access (ZERO-COPY read)
uint8_t* ASFWAudioNub::GetOutputAudioLocalMapping() const
{
    if (!ivars || !ivars->outputAudioMap) {
        return nullptr;
    }
    return reinterpret_cast<uint8_t*>(ivars->outputAudioMap->GetAddress());
}

// LOCALONLY: Get output audio buffer size
uint64_t ASFWAudioNub::GetOutputAudioBytes() const
{
    return ivars ? ivars->outputAudioBytes : 0;
}

// LOCALONLY: Get output audio frame capacity
uint32_t ASFWAudioNub::GetOutputAudioFrameCapacity() const
{
    return ivars ? ivars->outputAudioFrameCapacity : 0;
}

// LOCALONLY: Update write position (called by ASFWAudioDriver after CoreAudio write)
void ASFWAudioNub::UpdateOutputWritePosition(uint32_t newWriteFrame)
{
    if (ivars) {
        ivars->outputAudioWriteFrame.store(newWriteFrame, std::memory_order_release);
    }
}

// LOCALONLY: Get current write position (called by IT DMA for sync)
uint32_t ASFWAudioNub::GetOutputWritePosition() const
{
    return ivars ? ivars->outputAudioWriteFrame.load(std::memory_order_acquire) : 0;
}

// LOCALONLY: Set channel count directly from AVCDiscovery (MusicSubunitController data)
void ASFWAudioNub::SetChannelCount(uint32_t channels)
{
    if (!ivars) return;
    ivars->channelCount = channels;
    ASFW_LOG(Audio, "ASFWAudioNub: Channel count set to %u (from MusicSubunit)", channels);
}

// LOCALONLY: Get channel count
uint32_t ASFWAudioNub::GetChannelCount() const
{
    return ivars ? ivars->channelCount : 0;
}

void ASFWAudioNub::SetGuid(uint64_t guid)
{
    if (!ivars) {
        return;
    }
    ivars->guid = guid;
    ASFW_LOG(Audio, "ASFWAudioNub: GUID set to 0x%016llx", guid);
}

uint64_t ASFWAudioNub::GetGuid() const
{
    return ivars ? ivars->guid : 0;
}

void ASFWAudioNub::SetStreamMode(uint32_t modeRaw)
{
    if (!ivars) return;
    ivars->streamModeRaw = (modeRaw == 1u) ? 1u : 0u;
    ASFW_LOG(Audio, "ASFWAudioNub: Stream mode set to %{public}s",
             ivars->streamModeRaw == 1u ? "blocking" : "non-blocking");
}

uint32_t ASFWAudioNub::GetStreamMode() const
{
    return ivars ? ivars->streamModeRaw : 0u;
}

// ============================================================================
// RX Shared Memory Queue (for audio input from FireWire IR context)
// ============================================================================

// LOCALONLY: Ensure the RX queue exists (idempotent, called before IR start)
void ASFWAudioNub::EnsureRxQueueCreated()
{
    if (!ivars) return;
    if (const kern_return_t kr = CreateRxQueue(ivars); kr != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioNub: EnsureRxQueueCreated failed: 0x%x", kr);
    }
}

// RPC: AudioDriver calls this to get shared RX queue memory (mirrors CopyTransmitQueueMemory)
kern_return_t IMPL(ASFWAudioNub, CopyRxQueueMemory)
{
    ASFW_LOG(Audio, "ASFWAudioNub: CopyRxQueueMemory called");

    if (!outMemory || !outBytes) {
        ASFW_LOG(Audio, "ASFWAudioNub: CopyRxQueueMemory: bad arguments");
        return kIOReturnBadArgument;
    }

    if (!ivars) {
        ASFW_LOG(Audio, "ASFWAudioNub: CopyRxQueueMemory: no ivars");
        return kIOReturnNotReady;
    }

    // Ensure RX queue exists (lazy creation)
    if (const kern_return_t kr = CreateRxQueue(ivars); kr != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioNub: CopyRxQueueMemory: CreateRxQueue failed: 0x%x", kr);
        return kr;
    }

    // Return retained reference - caller must release
    ivars->rxQueueMem->retain();
    *outMemory = ivars->rxQueueMem;
    *outBytes = ivars->rxQueueBytes;

    ASFW_LOG(Audio, "ASFWAudioNub: CopyRxQueueMemory: returning mem=%p bytes=%llu",
             ivars->rxQueueMem, ivars->rxQueueBytes);

    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWAudioNub, GetProtocolBooleanControl)
{
    if (!ivars || !outValue) {
        return kIOReturnBadArgument;
    }

    ProtocolRuntimeBinding binding{};
    if (kern_return_t status = ResolveProtocolRuntimeBinding(ivars, binding);
        status != kIOReturnSuccess) {
        return status;
    }

    if (!binding.protocol->SupportsBooleanControl(classIdFourCC, element)) {
        return kIOReturnUnsupported;
    }

    auto* transport = binding.avcDiscovery->GetFCPTransportForNodeID(binding.device->nodeId);
    if (!transport) {
        return kIOReturnNotReady;
    }

    binding.protocol->UpdateRuntimeContext(binding.device->nodeId, transport);

    bool value = false;
    const kern_return_t status = binding.protocol->GetBooleanControlValue(classIdFourCC, element, value);
    if (status == kIOReturnSuccess) {
        *outValue = value;
    }
    return status;
}

kern_return_t IMPL(ASFWAudioNub, SetProtocolBooleanControl)
{
    if (!ivars) {
        return kIOReturnNotReady;
    }

    ProtocolRuntimeBinding binding{};
    if (kern_return_t status = ResolveProtocolRuntimeBinding(ivars, binding);
        status != kIOReturnSuccess) {
        return status;
    }

    if (!binding.protocol->SupportsBooleanControl(classIdFourCC, element)) {
        return kIOReturnUnsupported;
    }

    auto* transport = binding.avcDiscovery->GetFCPTransportForNodeID(binding.device->nodeId);
    if (!transport) {
        return kIOReturnNotReady;
    }

    binding.protocol->UpdateRuntimeContext(binding.device->nodeId, transport);
    return binding.protocol->SetBooleanControlValue(classIdFourCC, element, value);
}

// LOCALONLY: Get local mapping base address for IR context
uint8_t* ASFWAudioNub::GetRxQueueLocalMapping() const
{
    if (!ivars || !ivars->rxQueueMap) {
        return nullptr;
    }
    return reinterpret_cast<uint8_t*>(ivars->rxQueueMap->GetAddress());
}

// LOCALONLY: Get RX queue size
uint64_t ASFWAudioNub::GetRxQueueBytes() const
{
    return ivars ? ivars->rxQueueBytes : 0;
}
