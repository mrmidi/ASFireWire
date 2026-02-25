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
#include "../../Logging/LogConfig.hpp"
#include "../../Audio/AudioCoordinator.hpp"
#include "../../Protocols/AVC/IAVCDiscovery.hpp"
#include "../../Protocols/Audio/IDeviceProtocol.hpp"
#include "../../Shared/TxSharedQueue.hpp"
#include "../../Service/DriverContext.hpp"
#include "../Config/AudioConstants.hpp"

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSSharedPtr.h>

#include <algorithm>

// TX queue capacity: Config::kTxQueueCapacityFrames frames = ~85ms @ 48kHz.
// ZERO-COPY: Output audio buffer size matches Config::kAudioIoPeriodFrames.
// RX queue capacity: Config::kRxQueueCapacityFrames frames = ~85ms @ 48kHz (matches AudioRingBuffer).

static ASFWDriver* GetParentASFWDriver(const ASFWAudioNub_IVars* iv)
{
    if (!iv || !iv->parentDriver) {
        return nullptr;
    }
    return OSDynamicCast(ASFWDriver, iv->parentDriver);
}

static ASFW::Audio::AudioCoordinator* GetAudioCoordinator(const ASFWAudioNub_IVars* iv) noexcept {
    ASFWDriver* parent = GetParentASFWDriver(iv);
    if (!parent) {
        return nullptr;
    }
    auto* ctx = static_cast<ServiceContext*>(parent->GetServiceContext());
    if (!ctx || !ctx->audioCoordinator) {
        return nullptr;
    }
    return ctx->audioCoordinator.get();
}

struct ProtocolRuntimeBinding {
    ASFW::Discovery::DeviceRecord* device{nullptr};
    ASFW::Audio::IDeviceProtocol* protocol{nullptr};
    ASFW::Protocols::AVC::IAVCDiscovery* avcDiscovery{nullptr};
};

static kern_return_t ResolveProtocolRuntimeBinding(const ASFWAudioNub_IVars* iv,
                                                   ProtocolRuntimeBinding& outBinding);

static uint32_t ClampAudioChannels(uint32_t channels) {
    if (channels == 0) {
        return 0;
    }
    return (channels > ASFW::Isoch::Config::kMaxPcmChannels)
        ? ASFW::Isoch::Config::kMaxPcmChannels
        : channels;
}

static uint32_t FallbackInputChannels(const ASFWAudioNub_IVars* iv) {
    if (!iv) {
        return 0;
    }
    return ClampAudioChannels(iv->inputChannelCount ? iv->inputChannelCount : iv->channelCount);
}

static uint32_t FallbackOutputChannels(const ASFWAudioNub_IVars* iv) {
    if (!iv) {
        return 0;
    }
    return ClampAudioChannels(iv->outputChannelCount ? iv->outputChannelCount : iv->channelCount);
}

static bool TryResolveRuntimeAudioChannels(ASFWAudioNub_IVars* iv,
                                           uint32_t& outInputChannels,
                                           uint32_t& outOutputChannels)
{
    if (!iv) {
        return false;
    }

    ProtocolRuntimeBinding binding{};
    if (ResolveProtocolRuntimeBinding(iv, binding) != kIOReturnSuccess || !binding.protocol) {
        return false;
    }

    ASFW::Audio::AudioStreamRuntimeCaps caps{};
    if (!binding.protocol->GetRuntimeAudioStreamCaps(caps)) {
        return false;
    }

    if (caps.hostInputPcmChannels > ASFW::Isoch::Config::kMaxPcmChannels ||
        caps.hostOutputPcmChannels > ASFW::Isoch::Config::kMaxPcmChannels) {
        ASFW_LOG_WARNING(Audio,
                         "ASFWAudioNub: Clamping protocol PCM channels in=%u out=%u to max=%u",
                         caps.hostInputPcmChannels,
                         caps.hostOutputPcmChannels,
                         ASFW::Isoch::Config::kMaxPcmChannels);
    }

    const uint32_t inputCh = ClampAudioChannels(caps.hostInputPcmChannels);
    const uint32_t outputCh = ClampAudioChannels(caps.hostOutputPcmChannels);
    if (inputCh == 0 || outputCh == 0) {
        return false;
    }

    iv->inputChannelCount = inputCh;
    iv->outputChannelCount = outputCh;
    iv->channelCount = (inputCh > outputCh) ? inputCh : outputCh;

    outInputChannels = inputCh;
    outOutputChannels = outputCh;
    return true;
}

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

// Stream start/stop is now orchestrated by AudioCoordinator backends.

// Helper to create and initialize the TX queue
static kern_return_t CreateTxQueue(ASFWAudioNub_IVars* iv)
{
    if (!iv) return kIOReturnBadArgument;

    // Already created?
    if (iv->txQueueMem && iv->txQueueMap) {
        return kIOReturnSuccess;
    }

    uint32_t inputChUnused = 0;
    uint32_t txChannels = FallbackOutputChannels(iv);
    (void)TryResolveRuntimeAudioChannels(iv, inputChUnused, txChannels);

    if (txChannels == 0 || txChannels > ASFW::Isoch::Config::kMaxPcmChannels) {
        ASFW_LOG(Audio, "ASFWAudioNub: CreateTxQueue: invalid outputChannelCount=%u",
                 txChannels);
        return kIOReturnNotReady;
    }

    const uint64_t bytes = ASFW::Shared::TxSharedQueueSPSC::RequiredBytes(
        ASFW::Isoch::Config::kTxQueueCapacityFrames,
        txChannels);

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
            base, bytes, ASFW::Isoch::Config::kTxQueueCapacityFrames, txChannels);
        !initOk) {
        ASFW_LOG(Audio, "ASFWAudioNub: TxSharedQueue initialization failed");
        map->release();
        mem->release();
        return kIOReturnError;
    }

    iv->txQueueMem = mem;    // retained
    iv->txQueueMap = map;    // retained
    iv->txQueueBytes = bytes;

    ASFW_LOG(Audio, "ASFWAudioNub: TX queue created: %llu bytes, %u frames capacity, ch=%u base=%p",
             bytes, ASFW::Isoch::Config::kTxQueueCapacityFrames, txChannels, base);

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

    uint32_t rxChannels = FallbackInputChannels(iv);
    uint32_t outputChUnused = 0;
    (void)TryResolveRuntimeAudioChannels(iv, rxChannels, outputChUnused);

    if (rxChannels == 0 || rxChannels > ASFW::Isoch::Config::kMaxPcmChannels) {
        ASFW_LOG(Audio, "ASFWAudioNub: CreateRxQueue: invalid inputChannelCount=%u",
                 rxChannels);
        return kIOReturnNotReady;
    }

    const uint64_t bytes = ASFW::Shared::TxSharedQueueSPSC::RequiredBytes(
        ASFW::Isoch::Config::kRxQueueCapacityFrames,
        rxChannels);

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
            base, bytes, ASFW::Isoch::Config::kRxQueueCapacityFrames, rxChannels);
        !initOk) {
        ASFW_LOG(Audio, "ASFWAudioNub: RX shared queue initialization failed");
        map->release();
        mem->release();
        return kIOReturnError;
    }

    iv->rxQueueMem = mem;    // retained
    iv->rxQueueMap = map;    // retained
    iv->rxQueueBytes = bytes;

    ASFW_LOG(Audio, "ASFWAudioNub: RX queue created: %llu bytes, %u frames capacity, ch=%u base=%p",
             bytes, ASFW::Isoch::Config::kRxQueueCapacityFrames, rxChannels, base);

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
    ivars->channelCount = 2;
    ivars->inputChannelCount = 2;
    ivars->outputChannelCount = 2;
    
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

    // Seed channel counts from properties (if available). Queue sizing may later
    // be refined from runtime protocol caps at first queue creation.
    OSDictionary* propsRaw = nullptr;
    if (CopyProperties(&propsRaw) == kIOReturnSuccess && propsRaw) {
        OSSharedPtr<OSDictionary> props(propsRaw, OSNoRetain);
        if (auto* count = OSDynamicCast(OSNumber, props->getObject("ASFWChannelCount"))) {
            ivars->channelCount = ClampAudioChannels(count->unsigned32BitValue());
        }
        if (auto* inputCount = OSDynamicCast(OSNumber, props->getObject("ASFWInputChannelCount"))) {
            ivars->inputChannelCount = ClampAudioChannels(inputCount->unsigned32BitValue());
        }
        if (auto* outputCount = OSDynamicCast(OSNumber, props->getObject("ASFWOutputChannelCount"))) {
            ivars->outputChannelCount = ClampAudioChannels(outputCount->unsigned32BitValue());
        }
        if (ivars->inputChannelCount == 0) {
            ivars->inputChannelCount = ivars->channelCount;
        }
        if (ivars->outputChannelCount == 0) {
            ivars->outputChannelCount = ivars->channelCount;
        }
        ivars->channelCount = std::max(ivars->inputChannelCount, ivars->outputChannelCount);
    }

    // TX/RX queues and audio buffer are created lazily on first RPC access.

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

    uint32_t inputChUnused = 0;
    uint32_t outputChannels = FallbackOutputChannels(iv);
    (void)TryResolveRuntimeAudioChannels(iv, inputChUnused, outputChannels);

    if (outputChannels == 0 || outputChannels > ASFW::Isoch::Config::kMaxPcmChannels) {
        ASFW_LOG(Audio, "ASFWAudioNub: CreateOutputAudioBuffer: invalid outputChannelCount=%u",
                 outputChannels);
        return kIOReturnNotReady;
    }

    const uint32_t bytesPerFrame = outputChannels * sizeof(int32_t);
    const uint64_t bufferBytes = uint64_t(ASFW::Isoch::Config::kAudioIoPeriodFrames) * bytesPerFrame;

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
    iv->outputAudioFrameCapacity = ASFW::Isoch::Config::kAudioIoPeriodFrames;

    ASFW_LOG(Audio, "ASFWAudioNub: ZERO-COPY output audio buffer created: %llu bytes, %u frames (%u ch), base=%p",
             bufferBytes, ASFW::Isoch::Config::kAudioIoPeriodFrames, outputChannels, base);

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

    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWAudioNub, StartAudioStreaming)
{
    if (!ivars || ivars->guid == 0) {
        return kIOReturnNotReady;
    }

    // Auto-start gating (Info.plist + runtime), useful for debugging discovery without streams.
    if (!ASFW::LogConfig::Shared().IsAudioAutoStartEnabled()) {
        ASFW_LOG(Audio,
                 "ASFWAudioNub: StartAudioStreaming skipped (auto-start disabled) GUID=0x%016llx",
                 ivars->guid);
        return kIOReturnSuccess;
    }

    auto* coordinator = GetAudioCoordinator(ivars);
    if (!coordinator) {
        ASFW_LOG(Audio, "ASFWAudioNub: StartAudioStreaming: missing AudioCoordinator");
        return kIOReturnNotReady;
    }

    // Ensure queues exist before starting isoch.
    (void)CreateRxQueue(ivars);
    (void)CreateTxQueue(ivars);

    const IOReturn kr = coordinator->StartStreaming(ivars->guid);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioNub: StartAudioStreaming failed GUID=0x%016llx kr=0x%x", ivars->guid, kr);
    }
    return kr;
}

kern_return_t IMPL(ASFWAudioNub, StopAudioStreaming)
{
    if (!ivars || ivars->guid == 0) {
        return kIOReturnNotReady;
    }

    auto* coordinator = GetAudioCoordinator(ivars);
    if (!coordinator) {
        return kIOReturnNotReady;
    }

    const IOReturn kr = coordinator->StopStreaming(ivars->guid);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioNub: StopAudioStreaming failed GUID=0x%016llx kr=0x%x", ivars->guid, kr);
    }
    return kr;
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
    const uint32_t clamped = ClampAudioChannels(channels);
    ivars->channelCount = clamped;
    ivars->inputChannelCount = clamped;
    ivars->outputChannelCount = clamped;
    ASFW_LOG(Audio, "ASFWAudioNub: Channel count set to %u (legacy aggregate)", clamped);
}

// LOCALONLY: Get channel count
uint32_t ASFWAudioNub::GetChannelCount() const
{
    return ivars ? ivars->channelCount : 0;
}

uint32_t ASFWAudioNub::GetInputChannelCount() const
{
    if (!ivars) return 0;
    return ivars->inputChannelCount ? ivars->inputChannelCount : ivars->channelCount;
}

uint32_t ASFWAudioNub::GetOutputChannelCount() const
{
    if (!ivars) return 0;
    return ivars->outputChannelCount ? ivars->outputChannelCount : ivars->channelCount;
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
