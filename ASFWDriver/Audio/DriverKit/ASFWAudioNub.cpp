//
// ASFWAudioNub.cpp
// ASFWDriver
//
// Implementation of audio nub published by ASFWDriver.
// Direct audio memory/control ownership lives in AudioEndpointRuntime.
//

#include "ASFWAudioNub.h"
#include "ASFWDriver.h"
#include "../Core/AudioEndpointRuntime.hpp"
#include "../Core/AudioRuntimeRegistry.hpp"
#include "../Model/AudioPropertyKeys.hpp"
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
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSSharedPtr.h>

#include <algorithm>

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

[[nodiscard]] static ASFW::Audio::AudioRuntimeRegistry* GetAudioRuntimeRegistry(
    const ASFWAudioNub_IVars* iv) noexcept {
    const ASFWDriver* parent = GetParentASFWDriver(iv);
    if (!parent) {
        return nullptr;
    }
    const auto* controllerCore =
        static_cast<ASFW::Driver::ControllerCore*>(parent->GetControllerCore());
    if (!controllerCore) {
        return nullptr;
    }
    return controllerCore->GetAudioRuntimeRegistry();
}

[[nodiscard]] static std::shared_ptr<ASFW::Audio::AudioEndpointRuntime> FindEndpointRuntime(
    const ASFWAudioNub_IVars* iv) noexcept {
    if (!iv || iv->guid == 0) {
        return nullptr;
    }
    auto* runtime = GetAudioRuntimeRegistry(iv);
    return runtime ? runtime->FindEndpointRuntime(iv->guid) : nullptr;
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

    namespace Keys = ASFW::Audio::Model::PropertyKeys;

    if (auto* count = OSDynamicCast(OSNumber, props->getObject(Keys::kChannelCount))) {
        aggregate = ClampAudioChannels(count->unsigned32BitValue());
    }
    if (auto* inputCount = OSDynamicCast(OSNumber, props->getObject(Keys::kInputChannelCount))) {
        input = ClampAudioChannels(inputCount->unsigned32BitValue());
    }
    if (auto* outputCount = OSDynamicCast(OSNumber, props->getObject(Keys::kOutputChannelCount))) {
        output = ClampAudioChannels(outputCount->unsigned32BitValue());
    }
    if (auto* currentRate = OSDynamicCast(OSNumber, props->getObject(Keys::kCurrentSampleRate))) {
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
    }
    return Stop(provider, SUPERDISPATCH);
}

ASFWDriver* ASFWAudioNub::GetParentDriver() const
{
    return ivars ? OSDynamicCast(ASFWDriver, ivars->parentDriver) : nullptr;
}


kern_return_t IMPL(ASFWAudioNub, StartAudioStreaming)
{
    if (!ivars || ivars->guid == 0) {
        return kIOReturnNotReady;
    }

    auto endpoint = FindEndpointRuntime(ivars);
    if (!endpoint) {
        ASFW_LOG(DirectAudio,
                 "ADK FATAL StartAudioStreaming missing endpoint runtime guid=0x%016llx",
                 ivars->guid);
        return kIOReturnNotReady;
    }

    if (!endpoint->HasCompleteDirectAudioMemory()) {
        ASFW_LOG(DirectAudio,
                 "ADK FATAL StartAudioStreaming direct memory not ready guid=0x%016llx",
                 ivars->guid);
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

    const IOReturn kr = coordinator->StartStreaming(ivars->guid);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioNub: StartAudioStreaming failed GUID=0x%016llx kr=0x%x", ivars->guid, kr);
    } else {
        endpoint->MarkStreaming(true);
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
    if (auto endpoint = FindEndpointRuntime(ivars)) {
        endpoint->MarkStreaming(false);
    }
    return kr;
}

kern_return_t IMPL(ASFWAudioNub, RequestTxPayloadPreparation)
{
    if (!ivars || requestGeneration == 0) {
        return kIOReturnBadArgument;
    }

    ASFWDriver* parent = GetParentASFWDriver(ivars);
    auto* ctx = parent
        ? static_cast<ServiceContext*>(parent->GetServiceContext())
        : nullptr;
    if (!ctx || !ctx->workQueue || ctx->stopping.load(std::memory_order_acquire)) {
        return kIOReturnNotReady;
    }

    ctx->workQueue->DispatchAsync(^{
        if (ctx->stopping.load(std::memory_order_acquire)) {
            return;
        }
        if (auto* transmit = ctx->isoch.TransmitContext()) {
            transmit->RequestPayloadPreparation(requestGeneration);
        }
    });
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWAudioNub, CopyDirectAudioMemory)
{
    if (outOutputMemory) { *outOutputMemory = nullptr; }
    if (outInputMemory) { *outInputMemory = nullptr; }
    if (outControlMemory) { *outControlMemory = nullptr; }
    if (outOutputFrames) { *outOutputFrames = 0; }
    if (outOutputChannels) { *outOutputChannels = 0; }
    if (outInputFrames) { *outInputFrames = 0; }
    if (outInputChannels) { *outInputChannels = 0; }
    if (outSampleRateHz) { *outSampleRateHz = 0; }
    if (outGeneration) { *outGeneration = 0; }

    if (!ivars || !outOutputMemory || !outInputMemory || !outControlMemory ||
        !outOutputFrames || !outOutputChannels || !outInputFrames || !outInputChannels ||
        !outSampleRateHz || !outGeneration) {
        ASFW_LOG(DirectAudio, "ADK DBG MEM copy failed bad_args");
        return kIOReturnBadArgument;
    }

    auto endpoint = FindEndpointRuntime(ivars);
    if (!endpoint) {
        ASFW_LOG(DirectAudio,
                 "ADK DBG MEM copy failed missing_endpoint_runtime guid=0x%016llx",
                 ivars->guid);
        return kIOReturnNotReady;
    }

    return endpoint->CopyDirectAudioMemory(outOutputMemory,
                                           outInputMemory,
                                           outControlMemory,
                                           outOutputFrames,
                                           outOutputChannels,
                                           outInputFrames,
                                           outInputChannels,
                                           outSampleRateHz,
                                           outGeneration);
}


void ASFWAudioNub::SetChannelCount(uint32_t channels)
{
    if (!ivars) return;
    const uint32_t clamped = ClampAudioChannels(channels);
    ivars->channelCount = clamped;
    ivars->inputChannelCount = clamped;
    ivars->outputChannelCount = clamped;
    ASFW_LOG(Audio, "ASFWAudioNub: Channel count set to aggregate=%u", clamped);
}

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
