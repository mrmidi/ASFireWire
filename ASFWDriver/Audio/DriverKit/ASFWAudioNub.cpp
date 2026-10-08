#include "../Runtime/RateValidation.hpp"
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
#include "../../Common/TimingUtils.hpp"
#include "../../Logging/LogConfig.hpp"
#include "../Core/AudioCoordinator.hpp"
#include "../Protocols/AVCStartReadiness.hpp"
#include "../Protocols/DeviceProtocolChoice.hpp"
#include "../../DeviceProfiles/Audio/ResolvedDevicePolicy.hpp"
#include "../Protocols/DICE/Core/DICETypes.hpp"
#include "../Protocols/Duplex/DuplexControlTypes.hpp"
#include "../../Protocols/AVC/IAVCDiscovery.hpp"
#include "../../Protocols/AVC/AVCUnit.hpp"
#include "../../Protocols/AVC/AVCDiscovery.hpp"
#include "../Model/AvcVolumeMapping.hpp"
#include "../Model/RateConfiguration.hpp"
#include "../Model/DiscoveredRuntimeCaps.hpp"
#include "../Protocols/AVC/AvcDuplexClockObservation.hpp"
#include "../Protocols/AVC/AvcFeatureControl.hpp"
#include "../Protocols/Backends/SyncAsyncBridge.hpp"
#include "../Protocols/IDeviceProtocol.hpp"
#include "../../Service/DriverContext.hpp"
#include "../../Audio/Wire/AMDTP/AmdtpRateGeometry.hpp"

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSSharedPtr.h>

#include <algorithm>
#include <optional>
#include "../Runtime/RemoteDeviceStopResult.hpp"

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
    std::optional<ASFW::Discovery::DeviceRecord> device{};
    ASFW::Discovery::DeviceRegistry* registry{nullptr};
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
    return (channels > ASFW::Encoding::kMaxPcmChannels)
        ? ASFW::Encoding::kMaxPcmChannels
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
    bool hasInputCountProperty = false;
    bool hasOutputCountProperty = false;

    namespace Keys = ASFW::Audio::Model::PropertyKeys;

    if (auto* count = OSDynamicCast(OSNumber, props->getObject(Keys::kChannelCount))) {
        aggregate = ClampAudioChannels(count->unsigned32BitValue());
    }
    if (auto* inputCount = OSDynamicCast(OSNumber, props->getObject(Keys::kInputChannelCount))) {
        input = ClampAudioChannels(inputCount->unsigned32BitValue());
        hasInputCountProperty = true;
    }
    if (auto* outputCount = OSDynamicCast(OSNumber, props->getObject(Keys::kOutputChannelCount))) {
        output = ClampAudioChannels(outputCount->unsigned32BitValue());
        hasOutputCountProperty = true;
    }
    if (auto* currentRate = OSDynamicCast(OSNumber, props->getObject(Keys::kCurrentSampleRate))) {
        sampleRate = currentRate->unsigned32BitValue();
    }

    if (!hasInputCountProperty && input == 0) {
        input = aggregate;
    }
    if (!hasOutputCountProperty && output == 0) {
        output = aggregate;
    }
    aggregate = std::max(input, output);

    if (aggregate == 0) {
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

    auto device = registry->SnapshotByGuid(iv->guid);
    if (!device.has_value()) {
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

    outBinding.device = std::move(device);
    outBinding.registry = registry;
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
        if (ivars->txPreparationAction) {
            ivars->txPreparationAction->release();
            ivars->txPreparationAction = nullptr;
        }
        if (ivars->ztsAnchorAction) {
            ivars->ztsAnchorAction->release();
            ivars->ztsAnchorAction = nullptr;
        }
        if (ivars->deviceClockChangedAction) {
            ivars->deviceClockChangedAction->release();
            ivars->deviceClockChangedAction = nullptr;
        }
        if (ivars->ioRestartRequiredAction) {
            ivars->ioRestartRequiredAction->release();
            ivars->ioRestartRequiredAction = nullptr;
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
        ASFWDriver* parent = GetParentASFWDriver(ivars);
        auto* ctx =
            parent
                ? static_cast<ServiceContext*>(parent->GetServiceContext())
                : nullptr;
        if (ctx) {
            ctx->isoch.SetTxPreparationCallback({});
            ctx->isoch.SetZtsAnchorReadyCallback({});
        }
        if (ivars->txPreparationAction) {
            ivars->txPreparationAction->release();
            ivars->txPreparationAction = nullptr;
        }
        if (ivars->ztsAnchorAction) {
            ivars->ztsAnchorAction->release();
            ivars->ztsAnchorAction = nullptr;
        }
        if (ivars->deviceClockChangedAction) {
            ivars->deviceClockChangedAction->release();
            ivars->deviceClockChangedAction = nullptr;
        }
        if (ivars->ioRestartRequiredAction) {
            ivars->ioRestartRequiredAction->release();
            ivars->ioRestartRequiredAction = nullptr;
        }
        ivars->parentDriver = nullptr;
    }
    return Stop(provider, SUPERDISPATCH);
}

// Queries the synchronized host time and physical cycle timer snapshot.
kern_return_t IMPL(ASFWAudioNub, GetCycleTimePair)
{
    if (!outHostTimeMid || !outCycleTimer) {
        return kIOReturnBadArgument;
    }

    *outHostTimeMid = 0;
    *outCycleTimer = 0;

    if (!ivars) {
        return kIOReturnNotReady;
    }

    ASFWDriver* parent = GetParentASFWDriver(ivars);
    auto* ctx = parent ? static_cast<ServiceContext*>(parent->GetServiceContext()) : nullptr;
    if (!ctx || !ctx->deps.hardware) {
        return kIOReturnNotReady;
    }

    return ctx->isoch.GetCycleTimePair(outHostTimeMid, outCycleTimer, *ctx->deps.hardware);
}

kern_return_t IMPL(ASFWAudioNub, RegisterTxPreparationAction)
{
    if (!ivars) {
        return kIOReturnNotReady;
    }

    ASFWDriver* parent = GetParentASFWDriver(ivars);
    auto* ctx =
        parent ? static_cast<ServiceContext*>(parent->GetServiceContext())
               : nullptr;
    if (!ctx) {
        return kIOReturnNotReady;
    }

    if (action) {
        action->retain();
    }
    OSAction* oldAction = ivars->txPreparationAction;
    ivars->txPreparationAction = action;

    if (action) {
        ctx->isoch.SetTxPreparationCallback(
            [this](uint64_t generation) {
                if (ivars && ivars->txPreparationAction) {
                    TxPreparationReady(
                        ivars->txPreparationAction, generation);
                }
            });
    } else {
        ctx->isoch.SetTxPreparationCallback({});
    }

    if (oldAction) {
        oldAction->release();
    }
    return kIOReturnSuccess;
}



void IMPL(ASFWAudioNub, RequestTimingRecovery)
{
    const auto endpoint = FindEndpointRuntime(ivars);
    auto* coordinator = GetAudioCoordinator(ivars);
    if (!endpoint || !coordinator || !endpoint->IsCurrentStreamingRxEpoch(rxEpoch))
        return;
    // Only enqueue here: never wait for recovery on the packet preparation queue.
    // This seam is family-neutral. The coordinator routes the current loss to
    // the backend that owns this GUID (MOTU, AV/C, DICE, or a later profile).
    coordinator->HandleHostTimingLoss(ivars->guid);
}

void IMPL(ASFWAudioNub, TxPreparationReady)
{
    (void)action;
    (void)generation;
}

kern_return_t IMPL(ASFWAudioNub, RegisterZtsAnchorAction)
{
    if (!ivars) {
        return kIOReturnNotReady;
    }

    ASFWDriver* parent = GetParentASFWDriver(ivars);
    auto* ctx =
        parent ? static_cast<ServiceContext*>(parent->GetServiceContext())
               : nullptr;
    if (!ctx) {
        return kIOReturnNotReady;
    }

    if (action) {
        action->retain();
    }
    OSAction* oldAction = ivars->ztsAnchorAction;
    ivars->ztsAnchorAction = action;

    if (action) {
        ctx->isoch.SetZtsAnchorReadyCallback(
            [this](uint64_t generation) {
                if (ivars && ivars->ztsAnchorAction) {
                    ZtsAnchorReady(
                        ivars->ztsAnchorAction, generation);
                }
            });
    } else {
        ctx->isoch.SetZtsAnchorReadyCallback({});
    }

    if (oldAction) {
        oldAction->release();
    }
    return kIOReturnSuccess;
}

void IMPL(ASFWAudioNub, ZtsAnchorReady)
{
    (void)action;
    (void)generation;
}

kern_return_t IMPL(ASFWAudioNub, RegisterDeviceClockChangedAction)
{
    if (!ivars) {
        return kIOReturnNotReady;
    }

    if (action) {
        action->retain();
    }
    OSAction* oldAction = ivars->deviceClockChangedAction;
    ivars->deviceClockChangedAction = action;
    if (oldAction) {
        oldAction->release();
    }
    return kIOReturnSuccess;
}

void IMPL(ASFWAudioNub, DeviceClockChanged)
{
    (void)action;
    (void)nominalRateHz;
}

void ASFWAudioNub::NotifyDeviceClockChanged(uint32_t nominalRateHz)
{
    if (!ivars || !ivars->deviceClockChangedAction) {
        return;
    }
    ASFW_LOG(Audio,
             "ASFWAudioNub: NotifyDeviceClockChanged %u Hz guid=0x%016llx",
             nominalRateHz, ivars->guid);
    DeviceClockChanged(ivars->deviceClockChangedAction, nominalRateHz);
}

kern_return_t IMPL(ASFWAudioNub, RegisterIoRestartRequiredAction)
{
    if (!ivars) {
        return kIOReturnNotReady;
    }

    if (action) {
        action->retain();
    }
    OSAction* oldAction = ivars->ioRestartRequiredAction;
    ivars->ioRestartRequiredAction = action;
    if (oldAction) {
        oldAction->release();
    }
    return kIOReturnSuccess;
}

void IMPL(ASFWAudioNub, IoRestartRequired)
{
    (void)action;
    (void)reason;
}

bool ASFWAudioNub::NotifyIoRestartRequired(uint32_t reason)
{
    if (!ivars || !ivars->ioRestartRequiredAction) {
        return false;
    }
    ASFW_LOG(Audio,
             "ASFWAudioNub: NotifyIoRestartRequired reason=%u guid=0x%016llx",
             reason, ivars->guid);
    IoRestartRequired(ivars->ioRestartRequiredAction, reason);
    return true;
}

uint32_t ASFWAudioNub::GetCurrentSampleRateHz() const
{
    return ivars ? ivars->currentSampleRateHz : 0;
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

    // A bus reset invalidates the registry node mapping before its delayed ROM
    // scan republishes the current route. Hold AV/C starts in that interval:
    // Linux resets FCP before its OXFW stream restart (oxfw.c:279-287), and
    // Apple likewise waits for its resumed state before reconnecting. DICE's
    // hardcoded-nub path does not use FCP and remains independent.
    ProtocolRuntimeBinding binding{};
    const kern_return_t bindingStatus = ResolveProtocolRuntimeBinding(ivars, binding);
    if (bindingStatus == kIOReturnSuccess && binding.device.has_value()) {
        // Only an AV/C-driven device goes through the rebind gate below. MOTU is
        // the reason this has to be asked from the unit directory rather than
        // from (vendor, model): its root model_id is 0, so a flattened lookup
        // never matched it, it fell into the AV/C gate, and protocol v2 can
        // never pass that gate -- it is register-based and has no FCP
        // transport, so StartAudioStreaming returned kIOReturnNotReady on every
        // StartIO. The catalog matches MOTU from the unit directory, so asking
        // it is enough.
        const auto* audioPolicy =
            ASFW::DeviceProfiles::Audio::CurrentAudioPolicy(*binding.device);
        const auto backend = audioPolicy && binding.registry &&
                                     binding.registry->IsCurrent(audioPolicy->route)
            ? ASFW::Audio::ChooseAudioBackend(audioPolicy->plan)
            : std::nullopt;
        if (!backend.has_value()) {
            ASFW_LOG(Audio,
                     "ASFWAudioNub: refusing stream start without current resolved audio policy GUID=0x%016llx",
                     ivars->guid);
            return kIOReturnNotReady;
        }
        if (*backend == ASFW::Audio::AudioBackendKind::Avc) {
            auto avcUnit = binding.avcDiscovery ? binding.avcDiscovery->LiveUnit(binding.device->guid) : nullptr;
            if (!ASFW::Audio::HasReadyAVCStartRoute(binding.device->nodeId, avcUnit != nullptr)) {
                ASFW_LOG(Audio,
                         "ASFWAudioNub: deferring AV/C stream start until route is rebound GUID=0x%016llx node=%u",
                         ivars->guid,
                         binding.device->nodeId);
                return kIOReturnNotReady;
            }
            binding.protocol->UpdateRuntimeContext(ASFW::Discovery::DeviceRouteToken{
                                                    .guid = binding.device->guid,
                                                    .deviceIncarnation = binding.device->deviceIncarnation,
                                                    .routeEpoch = binding.device->routeEpoch,
                                                    .generation = binding.device->gen,
                                                    .nodeId = binding.device->nodeId}, std::move(avcUnit));
            ASFW_LOG(Audio,
                     "ASFWAudioNub: refreshed AV/C protocol route GUID=0x%016llx node=%u",
                     ivars->guid,
                     binding.device->nodeId);
        }
    }

    auto* coordinator = GetAudioCoordinator(ivars);
    if (!coordinator) {
        ASFW_LOG(Audio, "ASFWAudioNub: StartAudioStreaming: missing AudioCoordinator");
        return kIOReturnNotReady;
    }

    ASFW_LOG(Audio, "[SessionClock] HAL start GUID=%llx preparedRate=%u",
             ivars->guid, sampleRateHz);
    const IOReturn kr = coordinator->StartStreaming(ivars->guid,
        ASFW::Audio::AudioClockConfig{.sampleRateHz = sampleRateHz});
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioNub: StartAudioStreaming failed GUID=0x%016llx kr=0x%x (%{public}s)", ivars->guid, kr, ASFW::Logging::IOReturnName(kr));
    } else {
        endpoint->MarkStreaming(true);
    }
    return kr;
}

void ASFWAudioNub::RecordRemoteDeviceStopResult(kern_return_t status) {
    if (ivars) ASFW::Audio::Runtime::RemoteDeviceStopResult::Publish(ivars->remoteStopResult, status);
}

kern_return_t ASFWAudioNub::StopAudioStreamingOrRemoteResult() {
    // After Terminate, RPC dispatch itself may return kIOReturnIPCError before
    // StopAudioStreaming's handler executes. Consult the nub-owned proof first.
    if (ivars) {
        if (const auto terminal = ASFW::Audio::Runtime::RemoteDeviceStopResult::Read(ivars->remoteStopResult)) {
            ASFW_LOG(Audio, "[StopTrace] owner=nub guid=%016llx phase=late-remote-stop kr=0x%x (%{public}s)", ivars->guid, *terminal, ASFW::Logging::IOReturnName(*terminal));
            return *terminal;
        }
    }
    return StopAudioStreaming();
}

kern_return_t IMPL(ASFWAudioNub, StopAudioStreaming)
{
    if (!ivars || ivars->guid == 0) {
        return kIOReturnNotReady;
    }

    // Remote-loss cleanup precedes nub termination. Its result survives
    // Stop() clearing parentDriver and is specific to this old nub instance.
    if (const auto terminal = ASFW::Audio::Runtime::RemoteDeviceStopResult::Read(ivars->remoteStopResult)) {
        ASFW_LOG(Audio, "[StopTrace] owner=nub guid=%016llx phase=late-remote-stop kr=0x%x (%{public}s)", ivars->guid, *terminal, ASFW::Logging::IOReturnName(*terminal));
        return *terminal;
    }
    auto* coordinator = GetAudioCoordinator(ivars);
    if (!coordinator) {
        return kIOReturnNotReady;
    }

    const IOReturn kr = coordinator->StopStreaming(ivars->guid);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioNub: StopAudioStreaming failed GUID=0x%016llx kr=0x%x (%{public}s)", ivars->guid, kr, ASFW::Logging::IOReturnName(kr));
    }
    if (kr == kIOReturnSuccess) {
        if (auto endpoint = FindEndpointRuntime(ivars)) endpoint->MarkStreaming(false);
    }
    return kr;
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

// Allocates the shared payload slab, metadata ring, and control block.
kern_return_t IMPL(ASFWAudioNub, AllocateTxIsochResources)
{
    if (!ivars) {
        return kIOReturnNotReady;
    }
    // Retrieve the parent driver and its service context
    ASFWDriver* parent = GetParentASFWDriver(ivars);
    auto* ctx = parent ? static_cast<ServiceContext*>(parent->GetServiceContext()) : nullptr;
    if (!ctx) {
        return kIOReturnNotReady;
    }

    // Delegate allocation to the core IsochService
    return ctx->isoch.AllocateTxIsochResources(
        streamIndex, numSlots, maxPacketBytes, interruptInterval,
        outPayloadSlab, outMetadataRing, outControlBlock);
}

// Releases all allocated shared transmit resources.
kern_return_t IMPL(ASFWAudioNub, FreeTxIsochResources)
{
    if (!ivars) {
        return kIOReturnNotReady;
    }
    ASFWDriver* parent = GetParentASFWDriver(ivars);
    auto* ctx = parent ? static_cast<ServiceContext*>(parent->GetServiceContext()) : nullptr;
    if (!ctx) {
        return kIOReturnNotReady;
    }

    return ctx->isoch.FreeTxIsochResources();
}

// IIG dispatch across queues, not across processes (see ASFWAudioNub.iig:111
// and Info.plist IOUserServerOneProcess): runs in the nub's own
// process, so ivars and the parent -> ServiceContext -> AudioCoordinator chain
// are valid here (a LOCALONLY variant would dereference the audio side's proxy
// ivars, which are null).
kern_return_t IMPL(ASFWAudioNub, RequestSampleRateChange)
{
    if (!ivars) {
        ASFW_LOG(Audio, "ASFWAudioNub: RequestSampleRateChange not ready (ivars=null)");
        return kIOReturnNotReady;
    }
    auto* coordinator = GetAudioCoordinator(ivars);
    if (!coordinator) {
        ASFW_LOG(Audio,
                 "ASFWAudioNub: RequestSampleRateChange not ready (no coordinator) guid=0x%016llx",
                 ivars->guid);
        return kIOReturnNotReady;
    }

    // The seam is protocol-neutral: carry only the rate. The DICE adapter
    // (MakeDiceClockConfiguration) owns the CLOCK_SELECT register encoding.
    const ASFW::Audio::AudioClockConfig desired{
        .sampleRateHz = sampleRateHz,
    };
    // The duplex coordinator applies the device-policy gate, including the
    // FW-255 48 kHz limit for special M-Audio profiles.
    bool catalogRateAllowed = false;
    if (const auto endpoint = FindEndpointRuntime(ivars)) {
        ASFW::Audio::Model::ASFWAudioDevice config;
        if (endpoint->CopyConfig(config))
            for (const auto& formation : config.rateFormationCandidates)
                if (formation.sampleRateHz == sampleRateHz &&
                    ASFW::Audio::Runtime::RateEnabled(formation, config.currentSampleRate, config.usesRateFormations))
                    catalogRateAllowed = true;
    }
    if (!catalogRateAllowed && !ASFW::Audio::IsSupportedAudioClockConfig(desired) &&
        !ASFW::Audio::IsSupportedMAudioSpecialClockConfig(desired)) {
        ASFW_LOG(Audio,
                 "ASFWAudioNub: RequestSampleRateChange %u Hz refused - announced by the device "
                 "but not streamable in this build (high rates parked)",
                 sampleRateHz);
        return kIOReturnUnsupported;
    }

    ASFW_LOG(Audio,
             "ASFWAudioNub: RequestSampleRateChange %u Hz guid=0x%016llx",
             sampleRateHz, ivars->guid);
    ivars->rateObservationValid = false;
    const kern_return_t kr = coordinator->RequestClockConfig(
        ivars->guid, desired, ASFW::Audio::DuplexRestartReason::kSampleRateChange);
    if (kr == kIOReturnSuccess) {
        ivars->currentSampleRateHz = sampleRateHz;
    }
    return kr;
}

// The nub waits on its control queue; queries and their callbacks run on the
// controller queue. No IO callback or transport completion queue waits here.
kern_return_t IMPL(ASFWAudioNub, ReadRateClockState) {
    if (!outIncarnation || !outRouteEpoch || !outBusGeneration || !outOutputRateHz || !outInputRateHz)
        return kIOReturnBadArgument;
    if (!ivars) return kIOReturnNotReady;
    ivars->rateObservationValid = false;
    ivars->rateHardwareObservation.reset();
    *outIncarnation = *outRouteEpoch = 0;
    *outBusGeneration = *outOutputRateHz = *outInputRateHz = 0;
    const auto endpoint = FindEndpointRuntime(ivars);
    ASFW::Audio::Model::ASFWAudioDevice config;
    if (endpoint && endpoint->CopyConfig(config) && config.usesRateFormations) {
        ProtocolRuntimeBinding binding{};
        if (ResolveProtocolRuntimeBinding(ivars, binding) != kIOReturnSuccess || !binding.registry)
            return kIOReturnNotReady;
        const auto route = binding.registry->CurrentRoute(ivars->guid);
        if (!route) return kIOReturnNotReady;
        binding.protocol->UpdateRuntimeContext(*route, nullptr);
        const auto owner = binding.protocolOwner;
        const auto result = ASFW::Audio::WaitForAsyncResult<ASFW::Audio::RateHardwareObservation>(
            [owner](auto done) { owner->ReadRateObservation(std::move(done)); }, 5000, kIOReturnTimeout);
        if (result.status != kIOReturnSuccess) return result.status;
        if (!binding.registry->IsCurrent(*route)) return kIOReturnAborted;
        const auto actual = ASFW::Audio::Model::WithRateFormation(config, result.value.caps.sampleRateHz);
        const bool known = result.value.clockConfirmed && actual &&
            ASFW::Audio::RuntimeCapsMatchConfiguration(*actual, result.value.caps);
        *outIncarnation = route->deviceIncarnation;
        *outRouteEpoch = route->routeEpoch;
        *outBusGeneration = route->generation.value;
        *outOutputRateHz = *outInputRateHz = known ? result.value.caps.sampleRateHz : 0;
        ivars->rateObservedIncarnation = *outIncarnation;
        ivars->rateObservedRouteEpoch = *outRouteEpoch;
        ivars->rateObservedGeneration = *outBusGeneration;
        ivars->rateObservedOutputRate = *outOutputRateHz;
        ivars->rateObservedInputRate = *outInputRateHz;
        ivars->rateHardwareObservation = std::make_shared<ASFW::Audio::RateHardwareObservation>(result.value);
        ivars->rateObservationValid = true;
        ASFW_LOG(Audio, "[RateTxn] phase=observe family=dice guid=%016llx gen=%u rate=%u known=%u",
            ivars->guid, *outBusGeneration, result.value.caps.sampleRateHz, known ? 1U : 0U);
        return kIOReturnSuccess;
    }
    auto* parent = GetParentASFWDriver(ivars);
    auto* context = parent ? static_cast<ServiceContext*>(parent->GetServiceContext()) : nullptr;
    if (!context || !context->workQueue || !context->deps.avcDiscovery) return kIOReturnNotReady;
    struct Observation { ASFW::Discovery::DeviceRouteToken route; ASFW::Audio::AvcDuplexClockObservation clock; };
    const auto discovery = context->deps.avcDiscovery;
    const auto queue = context->workQueue;
    const uint64_t guid = ivars->guid;
    ProtocolRuntimeBinding binding{};
    if (ResolveProtocolRuntimeBinding(ivars, binding) != kIOReturnSuccess || !binding.device)
        return kIOReturnNotReady;
    const auto* policy = ASFW::DeviceProfiles::Audio::CurrentAudioPolicy(*binding.device);
    const bool special = policy &&
        (policy->plan.profileBuilder == ASFW::DeviceProfiles::Audio::ProfileBuilderId::MAudioFireWire1814 ||
         policy->plan.profileBuilder == ASFW::DeviceProfiles::Audio::ProfileBuilderId::MAudioProjectMix);
    const auto active = std::make_shared<std::atomic<bool>>(true);
    const auto result = ASFW::Audio::WaitForAsyncResult<Observation>([=](auto done) {
        queue->DispatchAsync(^{
            const auto unit = discovery->Unit(guid);
            const auto route = unit ? unit->CurrentRoute() : std::nullopt;
            if (!route) { done(kIOReturnNotReady, {}); return; }
            // M-Audio special_get_rate reads INPUT only. Do not extend its
            // firmware's probe allowlist (Linux bebob_maudio.c:301-314).
            ASFW::Audio::AvcDuplexClockRead::Start(unit, *route, 0, 0, active,
                [done, unit, route = *route](IOReturn status, auto clock) {
                    if (status == kIOReturnSuccess && clock.inputRateHz != 0 &&
                        (clock.outputRateHz == clock.inputRateHz || clock.outputRateHz == 0))
                        unit->RememberConfirmedDuplexRate(route, clock.inputRateHz);
                    done(status, Observation{route, clock});
                }, special);
        });
    }, 5000, kIOReturnTimeout);
    active->store(false, std::memory_order_release);
    if (result.status != kIOReturnSuccess) return result.status;
    *outIncarnation = result.value.route.deviceIncarnation;
    *outRouteEpoch = result.value.route.routeEpoch;
    *outBusGeneration = result.value.route.generation.value;
    *outOutputRateHz = result.value.clock.outputRateHz;
    *outInputRateHz = result.value.clock.inputRateHz;
    ivars->rateObservedIncarnation = *outIncarnation;
    ivars->rateObservedRouteEpoch = *outRouteEpoch;
    ivars->rateObservedGeneration = *outBusGeneration;
    ivars->rateObservedOutputRate = *outOutputRateHz;
    ivars->rateObservedInputRate = *outInputRateHz;
    ivars->rateObservationValid = true;
    ASFW_LOG(Audio, "[RateTxn] phase=observe guid=%016llx gen=%u epoch=%llu output=%u input=%u",
        guid, *outBusGeneration, *outRouteEpoch, *outOutputRateHz, *outInputRateHz);
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWAudioNub, ApplyRate) {
    if (!ivars) return kIOReturnNotReady;
    ProtocolRuntimeBinding binding{};
    if (ResolveProtocolRuntimeBinding(ivars, binding) != kIOReturnSuccess || !binding.device || !binding.registry)
        return kIOReturnNotReady;
    const ASFW::Discovery::DeviceRouteToken route{ivars->guid, expectedIncarnation,
        expectedRouteEpoch, ASFW::FW::Generation{expectedBusGeneration}, binding.device->nodeId};
    if (!binding.registry->IsCurrent(route)) return kIOReturnAborted;
    const auto endpoint = FindEndpointRuntime(ivars);
    ASFW::Audio::Model::ASFWAudioDevice config;
    if (!endpoint || endpoint->IsStreaming() || !endpoint->CopyConfig(config)) return kIOReturnNotReady;
    const auto found = std::ranges::find(config.rateFormationCandidates, sampleRateHz,
        &ASFW::Audio::Runtime::RateFormation::sampleRateHz);
    if (found == config.rateFormationCandidates.end() ||
        !ASFW::Audio::Runtime::RateEnabled(*found, config.currentSampleRate, config.usesRateFormations)) return kIOReturnUnsupported;
    auto* coordinator = GetAudioCoordinator(ivars);
    if (!coordinator) return kIOReturnNotReady;
    if (config.usesRateFormations) {
        binding.protocol->UpdateRuntimeContext(route, nullptr);
        ivars->rateObservationValid = false;
        ivars->rateHardwareObservation.reset();
        const auto status = coordinator->RequestClockConfig(ivars->guid, {.sampleRateHz = sampleRateHz},
            ASFW::Audio::DuplexRestartReason::kSampleRateChange);
        return binding.registry->IsCurrent(route) ? status : kIOReturnAborted;
    }
    // The first idle rate request can precede StartAudioStreaming, which used
    // to be the only path supplying the protocol's live AV/C unit. Observation
    // can succeed through discovery while BeBoB ApplyClockConfig sees no unit.
    // Bind the same generation-checked context here before programming clocks.
    auto avcUnit = binding.avcDiscovery
        ? binding.avcDiscovery->LiveUnit(binding.device->guid) : nullptr;
    if (!ASFW::Audio::HasReadyAVCStartRoute(binding.device->nodeId, avcUnit != nullptr)) {
        ASFW_LOG(Audio, "[RateTxn] phase=bind result=not-ready guid=%016llx gen=%u epoch=%llu",
                 ivars->guid, expectedBusGeneration, expectedRouteEpoch);
        return kIOReturnNotReady;
    }
    binding.protocol->UpdateRuntimeContext(route, std::move(avcUnit));
    ASFW_LOG(Audio, "[RateTxn] phase=bind result=ready guid=%016llx gen=%u epoch=%llu",
             ivars->guid, expectedBusGeneration, expectedRouteEpoch);
    ivars->rateObservationValid = false;
    return coordinator->RequestClockConfig(ivars->guid, {.sampleRateHz = sampleRateHz},
        ASFW::Audio::DuplexRestartReason::kSampleRateChange);
}

kern_return_t IMPL(ASFWAudioNub, InstallRateFormation) {
    if (!ivars || !ivars->rateObservationValid ||
        ivars->rateObservedIncarnation != expectedIncarnation ||
        ivars->rateObservedRouteEpoch != expectedRouteEpoch ||
        ivars->rateObservedGeneration != expectedBusGeneration ||
        ivars->rateObservedInputRate != sampleRateHz ||
        (ivars->rateObservedOutputRate != 0 && ivars->rateObservedOutputRate != sampleRateHz))
        return kIOReturnNotReady;
    const auto endpoint = FindEndpointRuntime(ivars);
    if (!endpoint || endpoint->IsStreaming()) return kIOReturnNotReady;
    ProtocolRuntimeBinding binding{};
    if (ResolveProtocolRuntimeBinding(ivars, binding) != kIOReturnSuccess || !binding.device || !binding.registry)
        return kIOReturnNotReady;
    const ASFW::Discovery::DeviceRouteToken route{ivars->guid, expectedIncarnation,
        expectedRouteEpoch, ASFW::FW::Generation{expectedBusGeneration}, binding.device->nodeId};
    if (!binding.registry->IsCurrent(route)) return kIOReturnAborted;
    ASFW::Audio::Model::ASFWAudioDevice prior{};
    if (!endpoint->CopyConfig(prior)) return kIOReturnNotReady;
    const auto next = ASFW::Audio::Model::WithRateFormation(prior, sampleRateHz);
    if (!next) return kIOReturnUnsupported;
    if (prior.usesRateFormations && (!ivars->rateHardwareObservation ||
        !ivars->rateHardwareObservation->clockConfirmed ||
        !ASFW::Audio::RuntimeCapsMatchConfiguration(*next, ivars->rateHardwareObservation->caps)))
        return kIOReturnNotReady;
    endpoint->UpdateConfig(*next);
    const auto status = endpoint->EnsureDirectAudioMemory();
    if (status != kIOReturnSuccess) { endpoint->UpdateConfig(prior); return status; }
    ivars->currentSampleRateHz = sampleRateHz;
    ivars->inputChannelCount = next->inputChannelCount;
    ivars->outputChannelCount = next->outputChannelCount;
    ivars->channelCount = next->channelCount;
    return kIOReturnSuccess;
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
    return ivars->inputChannelCount;
}

uint32_t ASFWAudioNub::GetOutputChannelCount() const
{
    if (!ivars) return 0;
    return ivars->outputChannelCount;
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

    auto avcUnit = binding.avcDiscovery->LiveUnit(binding.device->guid);
    if (!avcUnit) {
        return kIOReturnNotReady;
    }

    binding.protocol->UpdateRuntimeContext(ASFW::Discovery::DeviceRouteToken{
                                            .guid = binding.device->guid,
                                            .deviceIncarnation = binding.device->deviceIncarnation,
                                            .routeEpoch = binding.device->routeEpoch,
                                            .generation = binding.device->gen,
                                            .nodeId = binding.device->nodeId}, std::move(avcUnit));

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

    auto avcUnit = binding.avcDiscovery->LiveUnit(binding.device->guid);
    if (!avcUnit) {
        return kIOReturnNotReady;
    }

    binding.protocol->UpdateRuntimeContext(ASFW::Discovery::DeviceRouteToken{
                                            .guid = binding.device->guid,
                                            .deviceIncarnation = binding.device->deviceIncarnation,
                                            .routeEpoch = binding.device->routeEpoch,
                                            .generation = binding.device->gen,
                                            .nodeId = binding.device->nodeId}, std::move(avcUnit));
    return binding.protocol->SetBooleanControlValue(classIdFourCC, element, value);
}

// The nub queue may wait; the controller Default queue must never wait on FCP.
// Queue-confined graph lookup and all continuations run on that controller queue.
// Callbacks own their unit and wait state, so a timeout never leaves stack borrows.
kern_return_t IMPL(ASFWAudioNub, SetAvcFeatureControl) {
    if (!ivars || !outConfirmedValue) return kIOReturnBadArgument;
    auto* parent = GetParentASFWDriver(ivars);
    auto* context = parent ? static_cast<ServiceContext*>(parent->GetServiceContext()) : nullptr;
    if (!context || !context->workQueue || !context->deps.avcDiscovery) return kIOReturnNotReady;
    const auto discovery = context->deps.avcDiscovery;
    const auto queue = context->workQueue;
    const uint64_t guid = ivars->guid;
    static std::atomic<uint64_t> nextTraceId{0};
    const uint64_t traceId = nextTraceId.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t traceBegin = mach_absolute_time();
    ASFW_LOG(Audio, "[AvcControlTrace] id=%llu phase=wait-begin queue=nub-control guid=0x%016llx token=0x%06x hostTicks=%llu", traceId, guid, token, traceBegin);
    ASFW_LOG(Audio, "[AvcControl] request guid=0x%016llx token=0x%06x mute=%u requestedRaw=%d", guid, token, muteControl, value);
    const auto active = std::make_shared<std::atomic<bool>>(true);
    const auto result = ASFW::Audio::WaitForAsyncResult<int32_t>([=](auto done) {
        queue->DispatchAsync(^{
            ASFW_LOG(Audio, "[AvcControlTrace] id=%llu phase=controller-begin queue=controller elapsedUs=%llu", traceId, ASFW::Timing::hostTicksToNanos(mach_absolute_time() - traceBegin) / 1000);
            namespace A = ASFW::AVC;
            const auto unit = discovery->Unit(guid);
            const auto route = unit ? unit->CurrentRoute() : std::nullopt;
            const auto graph = unit ? unit->GetDiscoveredGraph() : nullptr;
            if (!unit || !route || !unit->IsCurrentRoute(*route) || !graph) { done(kIOReturnNotReady, 0); return; }
            auto found = graph->featureChannels.end();
            for (auto it = graph->featureChannels.begin(); it != graph->featureChannels.end(); ++it) {
                if (ASFW::Audio::Model::AvcControlToken(it->subunit, it->block, it->channel) == token) { found = it; break; }
            }
            if (found == graph->featureChannels.end()) { done(kIOReturnUnsupported, 0); return; }
            const auto subunit = found->subunit;
            ASFW::Audio::SetAvcFeature(unit, *route, *found, muteControl, value,
                [unit, route = *route, subunit, muteControl, token, done, traceId, traceBegin](auto reply) {
                    ASFW_LOG(Audio, "[AvcControlTrace] id=%llu phase=device-complete queue=controller elapsedUs=%llu ok=%u", traceId, ASFW::Timing::hostTicksToNanos(mach_absolute_time() - traceBegin) / 1000, reply.has_value());
                    if (!reply) {
                        const auto& error = reply.error();
                        ASFW_LOG(Audio, "[AvcControl] transaction failed guid=0x%016llx token=0x%06x errorKind=%u response=%d operandOffset=%u", route.guid, token, static_cast<unsigned>(error.kind), error.response ? static_cast<int>(*error.response) : -1, error.operandOffset);
                        done(A::ToIOReturn(error), 0); return;
                    }
                    ASFW_LOG(Audio, "[AvcControl] readback guid=0x%016llx token=0x%06x mute=%u confirmedRaw=%d generation=%u", route.guid, token, muteControl, muteControl ? static_cast<int32_t>(reply->AsMute()) : reply->AsVolume().Raw(), route.generation.value);
                    unit->RememberConfirmedFeature(route, subunit, *reply);
                    done(kIOReturnSuccess, muteControl ? static_cast<int32_t>(reply->AsMute()) : reply->AsVolume().Raw());
                }, [active] { return active->load(std::memory_order_acquire); });
        });
    }, 2000, kIOReturnTimeout, nullptr, 1);
    active->store(false, std::memory_order_release);
    ASFW_LOG(Audio, "[AvcControlTrace] id=%llu phase=wait-end queue=nub-control elapsedUs=%llu kr=0x%x", traceId, ASFW::Timing::hostTicksToNanos(mach_absolute_time() - traceBegin) / 1000, result.status);
    if (result.status == kIOReturnSuccess) *outConfirmedValue = result.value;
    ASFW_LOG(Audio, "[AvcControl] request complete guid=0x%016llx token=0x%06x kr=0x%x confirmedRaw=%d", guid, token, result.status, result.value);
    return result.status;
}
