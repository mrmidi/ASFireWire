//
// ASFWAudioNub.cpp
// ASFWDriver
//
// Implementation of audio nub published by ASFWDriver
// Manages shared memory for cross-process TX audio queue
//

#include "ASFWAudioNub.h"
#include "ASFWDriver.h"
#include "../Core/AudioRuntimeRegistry.hpp"
#include "Runtime/DirectAudioBindingSource.hpp"
#include "../../Controller/ControllerCore.hpp"
#include "../../Discovery/DeviceRegistry.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Logging/LogConfig.hpp"
#include "../Core/AudioCoordinator.hpp"
#include "../../Protocols/AVC/IAVCDiscovery.hpp"
#include "../../Protocols/Audio/IDeviceProtocol.hpp"
#include "../../Service/DriverContext.hpp"
#include "../../Isoch/Config/AudioConstants.hpp"

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSSharedPtr.h>

#include <algorithm>
#include <cstring>
#include <new>

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
    // `protocolOwner` keeps the protocol alive for the lifetime of the binding (the
    // caller's stack frame); `protocol` is the borrowed view used by the call sites.
    std::shared_ptr<ASFW::Audio::IDeviceProtocol> protocolOwner{};
    ASFW::Audio::IDeviceProtocol* protocol{nullptr};
    ASFW::Protocols::AVC::IAVCDiscovery* avcDiscovery{nullptr};
};

static kern_return_t ResolveProtocolRuntimeBinding(const ASFWAudioNub_IVars* iv,
                                                   ProtocolRuntimeBinding& outBinding);

struct OutputAudioBufferGeometry {
    uint32_t outputChannels{0};
    uint32_t bytesPerFrame{0};
    uint64_t bufferBytes{0};
};

static uint32_t ClampAudioChannels(uint32_t channels) {
    if (channels == 0) {
        return 0;
    }
    return (channels > ASFW::Isoch::Config::kMaxPcmChannels)
        ? ASFW::Isoch::Config::kMaxPcmChannels
        : channels;
}





static void RefreshChannelCountsFromProperties(ASFWAudioNub* self, ASFWAudioNub_IVars* iv) {
    if (!self || !iv) {
        return;
    }

    OSDictionary* propsRaw = nullptr;
    if (self->CopyProperties(&propsRaw) != kIOReturnSuccess || !propsRaw) {
        return;
    }

    OSSharedPtr<OSDictionary> props(propsRaw, OSNoRetain);
    uint32_t aggregate = iv->channelCount;
    uint32_t input = iv->inputChannelCount;
    uint32_t output = iv->outputChannelCount;
    uint32_t sampleRate = iv->currentSampleRateHz ? iv->currentSampleRateHz : 48000;

    if (auto* count = OSDynamicCast(OSNumber, props->getObject("ASFWChannelCount"))) {
        aggregate = ClampAudioChannels(count->unsigned32BitValue());
    }
    if (auto* inputCount = OSDynamicCast(OSNumber, props->getObject("ASFWInputChannelCount"))) {
        input = ClampAudioChannels(inputCount->unsigned32BitValue());
    }
    if (auto* outputCount = OSDynamicCast(OSNumber, props->getObject("ASFWOutputChannelCount"))) {
        output = ClampAudioChannels(outputCount->unsigned32BitValue());
    }
    if (auto* currentRate = OSDynamicCast(OSNumber, props->getObject("ASFWCurrentSampleRate"))) {
        sampleRate = currentRate->unsigned32BitValue();
    }

    if (input == 0) {
        input = aggregate;
    }
    if (output == 0) {
        output = aggregate;
    }
    aggregate = std::max(input, output);

    if (aggregate == 0 || input == 0 || output == 0) {
        return;
    }

    if (iv->channelCount != aggregate ||
        iv->inputChannelCount != input ||
        iv->outputChannelCount != output) {
        ASFW_LOG(Audio,
                 "ASFWAudioNub: Refreshed channel counts from properties agg=%u in=%u out=%u rate=%u",
                 aggregate,
                 input,
                 output,
                 sampleRate);
    }

    iv->channelCount = aggregate;
    iv->inputChannelCount = input;
    iv->outputChannelCount = output;
    iv->currentSampleRateHz = sampleRate ? sampleRate : 48000;
}

static void ReleaseDirectAudioMemory(ASFWAudioNub_IVars* iv) noexcept {
    if (!iv) {
        return;
    }
    if (iv->bindingLock) {
        IOLockLock(iv->bindingLock);
        iv->directOutputBase = nullptr;
        iv->directOutputBytes = 0;
        iv->directOutputFrames = 0;
        iv->directOutputChannels = 0;
        iv->directInputBase = nullptr;
        iv->directInputBytes = 0;
        iv->directInputFrames = 0;
        iv->directInputChannels = 0;
        iv->directControl = nullptr;
        iv->directSampleRateHz = 0;
        iv->directAudioDevice = nullptr;
        iv->directProviderBindingPublished = false;
        iv->directGeneration++;
        const auto generation = iv->directGeneration;
        IOLockUnlock(iv->bindingLock);
        ASFW_LOG(DirectAudio, "ADK DBG MEM release clear_binding gen=%llu", generation);
    }
    if (iv->directOutputMap) {
        iv->directOutputMap->release();
        iv->directOutputMap = nullptr;
    }
    if (iv->directInputMap) {
        iv->directInputMap->release();
        iv->directInputMap = nullptr;
    }
    if (iv->directControlMap) {
        iv->directControlMap->release();
        iv->directControlMap = nullptr;
    }
    if (iv->directOutputMemory) {
        iv->directOutputMemory->release();
        iv->directOutputMemory = nullptr;
    }
    if (iv->directInputMemory) {
        iv->directInputMemory->release();
        iv->directInputMemory = nullptr;
    }
    if (iv->directControlMemory) {
        iv->directControlMemory->release();
        iv->directControlMemory = nullptr;
    }
    iv->directOutputCapacityFrames = 0;
    iv->directInputCapacityFrames = 0;
}

static kern_return_t CreateMappedDirectBuffer(uint64_t bytes,
                                              uint64_t alignment,
                                              IOBufferMemoryDescriptor** outMemory,
                                              IOMemoryMap** outMap) noexcept {
    if (!outMemory || !outMap || bytes == 0) {
        return kIOReturnBadArgument;
    }

    *outMemory = nullptr;
    *outMap = nullptr;

    IOBufferMemoryDescriptor* memory = nullptr;
    kern_return_t kr = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                                        bytes,
                                                        alignment,
                                                        &memory);
    if (kr != kIOReturnSuccess || !memory) {
        return (kr == kIOReturnSuccess) ? kIOReturnNoMemory : kr;
    }

    IOMemoryMap* map = nullptr;
    kr = memory->CreateMapping(0, 0, 0, 0, 0, &map);
    if (kr != kIOReturnSuccess || !map) {
        if (map) {
            map->release();
        }
        memory->release();
        return (kr == kIOReturnSuccess) ? kIOReturnNoMemory : kr;
    }

    *outMemory = memory;
    *outMap = map;
    return kIOReturnSuccess;
}

static void PublishDirectAudioBindingFromMappedMemory(ASFWAudioNub_IVars* iv) noexcept {
    if (!iv || !iv->bindingLock || !iv->directOutputMap || !iv->directInputMap ||
        !iv->directControlMap) {
        return;
    }

    const auto* outputBase = reinterpret_cast<const int32_t*>(
        static_cast<uintptr_t>(iv->directOutputMap->GetAddress()));
    auto* inputBase = reinterpret_cast<int32_t*>(
        static_cast<uintptr_t>(iv->directInputMap->GetAddress()));
    auto* control = reinterpret_cast<ASFW::Audio::Runtime::AudioTransportControlBlock*>(
        static_cast<uintptr_t>(iv->directControlMap->GetAddress()));

    const uint64_t outputBytes = iv->directOutputMap->GetLength();
    const uint64_t inputBytes = iv->directInputMap->GetLength();

    IOLockLock(iv->bindingLock);
    iv->directOutputBase = outputBase;
    iv->directOutputBytes = outputBytes;
    iv->directOutputFrames = iv->directOutputCapacityFrames;
    iv->directOutputChannels = iv->outputChannelCount;
    iv->directInputBase = inputBase;
    iv->directInputBytes = inputBytes;
    iv->directInputFrames = iv->directInputCapacityFrames;
    iv->directInputChannels = iv->inputChannelCount;
    // Memory metadata is usable by ASFWAudioDriver immediately, but the isoch
    // binding must wait until the provider publishes the IOUserAudioDevice.
    iv->directControl = control;
    iv->directSampleRateHz = iv->currentSampleRateHz ? iv->currentSampleRateHz : 48000;
    iv->directAudioDevice = nullptr;
    iv->directProviderBindingPublished = false;
    iv->directGeneration++;
    const auto generation = iv->directGeneration;
    IOLockUnlock(iv->bindingLock);

    ASFW_LOG(DirectAudio,
             "ADK DBG BIND local_memory_ready gen=%llu outBase=%p outBytes=%llu outFrames=%u outCh=%u inBase=%p inBytes=%llu inFrames=%u inCh=%u control=%p rate=%u audioDevice=null provider=0",
             generation,
             static_cast<const void*>(outputBase),
             outputBytes,
             iv->directOutputCapacityFrames,
             iv->outputChannelCount,
             static_cast<void*>(inputBase),
             inputBytes,
             iv->directInputCapacityFrames,
             iv->inputChannelCount,
             static_cast<void*>(control),
             iv->directSampleRateHz);
}

static kern_return_t EnsureDirectAudioMemory(ASFWAudioNub* self, ASFWAudioNub_IVars* iv) noexcept {
    if (!self || !iv || !iv->bindingLock) {
        return kIOReturnNotReady;
    }

    RefreshChannelCountsFromProperties(self, iv);

    const uint32_t outputChannels = iv->outputChannelCount ? iv->outputChannelCount : iv->channelCount;
    const uint32_t inputChannels = iv->inputChannelCount ? iv->inputChannelCount : iv->channelCount;
    // Output ring is several IO periods deep so the isoch consumer can trail the
    // HAL by a stable lead without starving (FW-26 #6/#8). Input stays one period.
    const uint32_t outputFrames = ASFW::Isoch::Config::kAudioOutputRingFrames;
    const uint32_t inputFrames = ASFW::Isoch::Config::kAudioIoPeriodFrames;

    if (outputChannels == 0 || inputChannels == 0) {
        ASFW_LOG(DirectAudio,
                 "ADK DBG MEM ensure failed bad_channels agg=%u in=%u out=%u",
                 iv->channelCount,
                 inputChannels,
                 outputChannels);
        return kIOReturnBadArgument;
    }

    if (iv->directOutputMemory && iv->directInputMemory && iv->directControlMemory &&
        iv->directOutputMap && iv->directInputMap && iv->directControlMap &&
        iv->directOutputCapacityFrames == outputFrames &&
        iv->directInputCapacityFrames == inputFrames &&
        iv->directOutputChannels == outputChannels &&
        iv->directInputChannels == inputChannels &&
        iv->directSampleRateHz == (iv->currentSampleRateHz ? iv->currentSampleRateHz : 48000)) {
        ASFW_LOG(DirectAudio,
                 "ADK DBG MEM ensure reuse gen=%llu outFrames=%u outCh=%u inFrames=%u inCh=%u rate=%u",
                 iv->directGeneration,
                 outputFrames,
                 outputChannels,
                 inputFrames,
                 inputChannels,
                 iv->currentSampleRateHz ? iv->currentSampleRateHz : 48000);
        return kIOReturnSuccess;
    }

    ReleaseDirectAudioMemory(iv);

    const uint64_t outputBytes = static_cast<uint64_t>(outputFrames) * outputChannels * sizeof(int32_t);
    const uint64_t inputBytes = static_cast<uint64_t>(inputFrames) * inputChannels * sizeof(int32_t);
    const uint64_t controlBytes = sizeof(ASFW::Audio::Runtime::AudioTransportControlBlock);

    ASFW_LOG(DirectAudio,
             "ADK DBG MEM allocate outBytes=%llu outFrames=%u outCh=%u inBytes=%llu inFrames=%u inCh=%u controlBytes=%llu rate=%u",
             outputBytes,
             outputFrames,
             outputChannels,
             inputBytes,
             inputFrames,
             inputChannels,
             controlBytes,
             iv->currentSampleRateHz ? iv->currentSampleRateHz : 48000);

    kern_return_t kr = CreateMappedDirectBuffer(outputBytes, 64, &iv->directOutputMemory, &iv->directOutputMap);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(DirectAudio, "ADK DBG MEM allocate output failed kr=0x%x", kr);
        ReleaseDirectAudioMemory(iv);
        return kr;
    }
    kr = CreateMappedDirectBuffer(inputBytes, 64, &iv->directInputMemory, &iv->directInputMap);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(DirectAudio, "ADK DBG MEM allocate input failed kr=0x%x", kr);
        ReleaseDirectAudioMemory(iv);
        return kr;
    }
    kr = CreateMappedDirectBuffer(controlBytes,
                                  alignof(ASFW::Audio::Runtime::AudioTransportControlBlock),
                                  &iv->directControlMemory,
                                  &iv->directControlMap);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(DirectAudio, "ADK DBG MEM allocate control failed kr=0x%x", kr);
        ReleaseDirectAudioMemory(iv);
        return kr;
    }

    iv->directOutputCapacityFrames = outputFrames;
    iv->directInputCapacityFrames = inputFrames;

    std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(iv->directOutputMap->GetAddress())),
                0,
                static_cast<size_t>(iv->directOutputMap->GetLength()));
    std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(iv->directInputMap->GetAddress())),
                0,
                static_cast<size_t>(iv->directInputMap->GetLength()));
    auto* control = reinterpret_cast<ASFW::Audio::Runtime::AudioTransportControlBlock*>(
        static_cast<uintptr_t>(iv->directControlMap->GetAddress()));
    new (control) ASFW::Audio::Runtime::AudioTransportControlBlock();
    control->ResetForStart();

    PublishDirectAudioBindingFromMappedMemory(iv);
    return kIOReturnSuccess;
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

    const auto* controllerCore =
        static_cast<ASFW::Driver::ControllerCore*>(parent->GetControllerCore());
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

    auto* runtime = controllerCore->GetAudioRuntimeRegistry();
    if (!runtime) {
        return kIOReturnNotReady;
    }
    auto protocol = runtime->FindShared(iv->guid);
    if (!protocol) {
        return kIOReturnUnsupported;
    }

    auto* avcDiscovery = controllerCore->GetAVCDiscovery();
    if (!avcDiscovery) {
        return kIOReturnNotReady;
    }

    outBinding.device = device;
    outBinding.protocolOwner = std::move(protocol);
    outBinding.protocol = outBinding.protocolOwner.get();
    outBinding.avcDiscovery = avcDiscovery;
    return kIOReturnSuccess;
}





// Stream start/stop is now orchestrated by AudioCoordinator backends.

// Helper to create and initialize the TX queue




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

    ivars->bindingLock = IOLockAlloc();
    if (!ivars->bindingLock) {
        ASFW_LOG(Audio, "ASFWAudioNub: Failed to allocate binding lock");
        IOSafeDeleteNULL(ivars, ASFWAudioNub_IVars, 1);
        return false;
    }
    ivars->directGeneration = 0;
    ivars->directOutputBase = nullptr;
    ivars->directOutputBytes = 0;
    ivars->directOutputFrames = 0;
    ivars->directOutputChannels = 0;
    ivars->directInputBase = nullptr;
    ivars->directInputBytes = 0;
    ivars->directInputFrames = 0;
    ivars->directInputChannels = 0;
    ivars->directControl = nullptr;
    ivars->directSampleRateHz = 0;
    ivars->directAudioDevice = nullptr;
    ivars->directProviderBindingPublished = false;
    ivars->directBindingSource = nullptr;
    ivars->directOutputMemory = nullptr;
    ivars->directInputMemory = nullptr;
    ivars->directControlMemory = nullptr;
    ivars->directOutputMap = nullptr;
    ivars->directInputMap = nullptr;
    ivars->directControlMap = nullptr;
    ivars->directOutputCapacityFrames = 0;
    ivars->directInputCapacityFrames = 0;

    ivars->parentDriver = nullptr;
    ivars->guid = 0;
    ivars->channelCount = 2;
    ivars->inputChannelCount = 2;
    ivars->outputChannelCount = 2;
    ivars->currentSampleRateHz = 48000;
    ivars->streamModeRaw = 0;

    ASFW_LOG(Audio, "ASFWAudioNub: init() succeeded");
    return true;
}

void ASFWAudioNub::free()
{
    ASFW_LOG(Audio, "ASFWAudioNub: free()");
    if (ivars) {
        ReleaseDirectAudioMemory(ivars);

        if (ivars->bindingLock) {
            IOLockFree(ivars->bindingLock);
            ivars->bindingLock = nullptr;
        }

        if (ivars->directBindingSource) {
            delete static_cast<ASFW::Audio::Runtime::NubDirectAudioBindingSource*>(ivars->directBindingSource);
            ivars->directBindingSource = nullptr;
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
    RefreshChannelCountsFromProperties(this, ivars);

    // TX/RX queues and audio buffer are created lazily on first RPC access.

    // Register the service so ASFWAudioDriver can match on us
    error = RegisterService();
    if (error != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioNub: RegisterService() failed: %d", error);
        return error;
    }

    ivars->directBindingSource = new ASFW::Audio::Runtime::NubDirectAudioBindingSource(this);

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


// LOCALONLY: Get parent driver pointer (same process)
ASFWDriver* ASFWAudioNub::GetParentDriver() const
{
    return ivars ? OSDynamicCast(ASFWDriver, ivars->parentDriver) : nullptr;
}

// LOCALONLY: Get local mapping base address for IT context


// LOCALONLY: Get TX queue size


// ============================================================================
// ZERO-COPY: Output Audio Buffer for IOUserAudioStream AND IT DMA
// ============================================================================

// Helper to create the shared output audio buffer


// RPC callable from ASFWAudioDriver to get shared output audio buffer


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

kern_return_t IMPL(ASFWAudioNub, CopyDirectAudioMemory)
{
    if (!ivars || !outOutputMemory || !outInputMemory || !outControlMemory ||
        !outOutputFrames || !outOutputChannels || !outInputFrames || !outInputChannels ||
        !outSampleRateHz || !outGeneration) {
        ASFW_LOG(DirectAudio, "ADK DBG MEM copy failed bad_args");
        return kIOReturnBadArgument;
    }

    *outOutputMemory = nullptr;
    *outInputMemory = nullptr;
    *outControlMemory = nullptr;
    *outOutputFrames = 0;
    *outOutputChannels = 0;
    *outInputFrames = 0;
    *outInputChannels = 0;
    *outSampleRateHz = 0;
    *outGeneration = 0;

    const kern_return_t kr = EnsureDirectAudioMemory(this, ivars);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(DirectAudio, "ADK DBG MEM copy ensure_failed kr=0x%x", kr);
        return kr;
    }

    ivars->directOutputMemory->retain();
    ivars->directInputMemory->retain();
    ivars->directControlMemory->retain();

    *outOutputMemory = ivars->directOutputMemory;
    *outInputMemory = ivars->directInputMemory;
    *outControlMemory = ivars->directControlMemory;
    *outOutputFrames = ivars->directOutputCapacityFrames;
    *outOutputChannels = ivars->outputChannelCount;
    *outInputFrames = ivars->directInputCapacityFrames;
    *outInputChannels = ivars->inputChannelCount;
    *outSampleRateHz = ivars->directSampleRateHz;
    *outGeneration = ivars->directGeneration;

    ASFW_LOG(DirectAudio,
             "ADK DBG MEM copy ok gen=%llu outMem=%p outFrames=%u outCh=%u inMem=%p inFrames=%u inCh=%u controlMem=%p rate=%u",
             *outGeneration,
             static_cast<void*>(*outOutputMemory),
             *outOutputFrames,
             *outOutputChannels,
             static_cast<void*>(*outInputMemory),
             *outInputFrames,
             *outInputChannels,
             static_cast<void*>(*outControlMemory),
             *outSampleRateHz);

    return kIOReturnSuccess;
}

// LOCALONLY: Get local mapping for IT DMA access (ZERO-COPY read)


// LOCALONLY: Get output audio buffer size


// LOCALONLY: Get output audio frame capacity


// LOCALONLY: Register the direct duplex audio binding (same dext process; raw pointers).
void ASFWAudioNub::SetDirectAudioBinding(const int32_t* outputBase,
                                         uint64_t outputBytes,
                                         uint32_t outputFrames,
                                         uint32_t outputChannels,
                                         int32_t* inputBase,
                                         uint64_t inputBytes,
                                         uint32_t inputFrames,
                                         uint32_t inputChannels,
                                         ASFW::Audio::Runtime::AudioTransportControlBlock* control,
                                         uint32_t sampleRateHz,
                                         IOUserAudioDevice* audioDevice)
{
    if (!ivars || !ivars->bindingLock) {
        return;
    }
    IOLockLock(ivars->bindingLock);
    ivars->directOutputBase = outputBase;
    ivars->directOutputBytes = outputBytes;
    ivars->directOutputFrames = outputFrames;
    ivars->directOutputChannels = outputChannels;
    ivars->directInputBase = inputBase;
    ivars->directInputBytes = inputBytes;
    ivars->directInputFrames = inputFrames;
    ivars->directInputChannels = inputChannels;
    ivars->directControl = control;
    ivars->directSampleRateHz = sampleRateHz;
    ivars->directAudioDevice = audioDevice;
    ivars->directProviderBindingPublished =
        (control != nullptr && sampleRateHz != 0 && audioDevice != nullptr);
    ivars->directGeneration++;
    IOLockUnlock(ivars->bindingLock);

    ASFW_LOG(Audio,
             "ASFWAudioNub: SetDirectAudioBinding outBase=%p outFrames=%u outCh=%u inBase=%p inFrames=%u inCh=%u control=%p rate=%u dev=%p gen=%llu",
             static_cast<const void*>(outputBase), outputFrames, outputChannels,
             static_cast<void*>(inputBase), inputFrames, inputChannels,
             static_cast<const void*>(control), sampleRateHz, static_cast<void*>(audioDevice), ivars->directGeneration);
    ASFW_LOG(DirectAudio,
             "ADK DBG BIND set outBase=%p outBytes=%llu outFrames=%u outCh=%u inBase=%p inBytes=%llu inFrames=%u inCh=%u control=%p rate=%u dev=%p provider=%d gen=%llu",
             static_cast<const void*>(outputBase),
             outputBytes,
             outputFrames,
             outputChannels,
             static_cast<void*>(inputBase),
             inputBytes,
             inputFrames,
             inputChannels,
             static_cast<void*>(control),
             sampleRateHz,
             static_cast<void*>(audioDevice),
             ivars->directProviderBindingPublished,
             ivars->directGeneration);
}

// LOCALONLY: Clear the direct duplex audio binding (called from ASFWAudioDriver::Stop).
void ASFWAudioNub::ClearDirectAudioBinding()
{
    if (!ivars || !ivars->bindingLock) {
        return;
    }
    IOLockLock(ivars->bindingLock);
    ivars->directOutputBase = nullptr;
    ivars->directOutputBytes = 0;
    ivars->directOutputFrames = 0;
    ivars->directOutputChannels = 0;
    ivars->directInputBase = nullptr;
    ivars->directInputBytes = 0;
    ivars->directInputFrames = 0;
    ivars->directInputChannels = 0;
    ivars->directControl = nullptr;
    ivars->directSampleRateHz = 0;
    ivars->directAudioDevice = nullptr;
    ivars->directProviderBindingPublished = false;
    ivars->directGeneration++;
    IOLockUnlock(ivars->bindingLock);

    ASFW_LOG(Audio, "ASFWAudioNub: ClearDirectAudioBinding gen=%llu", ivars->directGeneration);
    ASFW_LOG(DirectAudio, "ADK DBG BIND clear gen=%llu", ivars->directGeneration);
}

// LOCALONLY: Fetch the direct duplex audio binding. Returns false if no valid control block is registered.
bool ASFWAudioNub::GetDirectAudioBinding(const int32_t** outOutputBase,
                                         uint64_t* outOutputBytes,
                                         uint32_t* outOutputFrames,
                                         uint32_t* outOutputChannels,
                                         int32_t** outInputBase,
                                         uint64_t* outInputBytes,
                                         uint32_t* outInputFrames,
                                         uint32_t* outInputChannels,
                                         ASFW::Audio::Runtime::AudioTransportControlBlock** outControl,
                                         uint32_t* outSampleRateHz,
                                         IOUserAudioDevice** outAudioDevice,
                                         uint64_t* outGeneration) const
{
    if (!ivars || !ivars->bindingLock) {
        // Hot-path binding diagnostic, disabled for audio stability.
        // ASFW_LOG_RL(DirectAudio, "adk_bind/no_ivars", 5000, OS_LOG_TYPE_DEFAULT,
        //             "ADK DBG BIND get failed no_ivars_or_lock");
        return false;
    }
    IOLockLock(ivars->bindingLock);
    if (!ivars->directProviderBindingPublished || !ivars->directControl ||
        ivars->directSampleRateHz == 0 || !ivars->directAudioDevice) {
        const auto gen = ivars->directGeneration;
        const auto control = ivars->directControl;
        const auto rate = ivars->directSampleRateHz;
        const auto audioDevice = ivars->directAudioDevice;
        const auto provider = ivars->directProviderBindingPublished;
        const auto outBase = ivars->directOutputBase;
        const auto outFrames = ivars->directOutputFrames;
        const auto outChannels = ivars->directOutputChannels;
        IOLockUnlock(ivars->bindingLock);
        ASFW_LOG_RL(DirectAudio, "adk_bind/not_ready_or_null_device", 1000, OS_LOG_TYPE_DEFAULT,
                    "ADK FATAL BIND get refused not_ready gen=%llu provider=%d control=%p rate=%u audioDevice=%p outBase=%p outFrames=%u outCh=%u",
                    gen,
                    provider,
                    static_cast<void*>(control),
                    rate,
                    static_cast<void*>(audioDevice),
                    static_cast<const void*>(outBase),
                    outFrames,
                    outChannels);
        return false;
    }

    if (outOutputBase) { *outOutputBase = ivars->directOutputBase; }
    if (outOutputBytes) { *outOutputBytes = ivars->directOutputBytes; }
    if (outOutputFrames) { *outOutputFrames = ivars->directOutputFrames; }
    if (outOutputChannels) { *outOutputChannels = ivars->directOutputChannels; }
    if (outInputBase) { *outInputBase = ivars->directInputBase; }
    if (outInputBytes) { *outInputBytes = ivars->directInputBytes; }
    if (outInputFrames) { *outInputFrames = ivars->directInputFrames; }
    if (outInputChannels) { *outInputChannels = ivars->directInputChannels; }
    if (outControl) { *outControl = ivars->directControl; }
    if (outSampleRateHz) { *outSampleRateHz = ivars->directSampleRateHz; }
    if (outAudioDevice) { *outAudioDevice = ivars->directAudioDevice; }
    if (outGeneration) { *outGeneration = ivars->directGeneration; }
    IOLockUnlock(ivars->bindingLock);
    return true;
}

void* ASFWAudioNub::GetDirectAudioBindingSource()
{
    return ivars ? ivars->directBindingSource : nullptr;
}

// LOCALONLY: Update write position (called by ASFWAudioDriver after CoreAudio write)


// LOCALONLY: Get current write position (called by IT DMA for sync)


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


// RPC: AudioDriver calls this to get shared RX queue memory (mirrors CopyTransmitQueueMemory)


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


// LOCALONLY: Get RX queue size
