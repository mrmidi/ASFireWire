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

    if (auto* count = OSDynamicCast(OSNumber, props->getObject("ASFWChannelCount"))) {
        aggregate = ClampAudioChannels(count->unsigned32BitValue());
    }
    if (auto* inputCount = OSDynamicCast(OSNumber, props->getObject("ASFWInputChannelCount"))) {
        input = ClampAudioChannels(inputCount->unsigned32BitValue());
    }
    if (auto* outputCount = OSDynamicCast(OSNumber, props->getObject("ASFWOutputChannelCount"))) {
        output = ClampAudioChannels(outputCount->unsigned32BitValue());
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
                 "ASFWAudioNub: Refreshed channel counts from properties agg=%u in=%u out=%u",
                 aggregate,
                 input,
                 output);
    }

    iv->channelCount = aggregate;
    iv->inputChannelCount = input;
    iv->outputChannelCount = output;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static bool TryResolveRuntimeAudioChannels(ASFWAudioNub_IVars* iv,
                                           uint32_t& outInputChannels, // NOLINT(bugprone-easily-swappable-parameters)
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
    ivars->directBindingSource = nullptr;

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
        if (ivars->bindingLock) {
            IOLockFree(ivars->bindingLock);
            ivars->bindingLock = nullptr;
        }

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
    ivars->directGeneration++;
    IOLockUnlock(ivars->bindingLock);

    ASFW_LOG(Audio,
             "ASFWAudioNub: SetDirectAudioBinding outBase=%p outFrames=%u outCh=%u inBase=%p inFrames=%u inCh=%u control=%p rate=%u dev=%p gen=%llu",
             static_cast<const void*>(outputBase), outputFrames, outputChannels,
             static_cast<void*>(inputBase), inputFrames, inputChannels,
             static_cast<const void*>(control), sampleRateHz, static_cast<void*>(audioDevice), ivars->directGeneration);
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
    ivars->directGeneration++;
    IOLockUnlock(ivars->bindingLock);

    ASFW_LOG(Audio, "ASFWAudioNub: ClearDirectAudioBinding gen=%llu", ivars->directGeneration);
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
        return false;
    }
    IOLockLock(ivars->bindingLock);
    if (!ivars->directControl || ivars->directSampleRateHz == 0) {
        IOLockUnlock(ivars->bindingLock);
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

