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
#include "../Devices/AudioIdentity.hpp"
#include "../Model/AudioPropertyKeys.hpp"
#include "../../Controller/ControllerCore.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Logging/LogConfig.hpp"
#include "../Core/AudioCoordinator.hpp"
#include "../../Service/DriverContext.hpp"
#include "../../Audio/Wire/AMDTP/AmdtpRateGeometry.hpp"
#include "../Shared/AudioRuntimeTuningStore.hpp"

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSSharedPtr.h>

#include <algorithm>
#include <new>

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
    if (!iv || iv->endpointId == 0) {
        return nullptr;
    }
    auto* runtime = GetAudioRuntimeRegistry(iv);
    return runtime
        ? runtime->FindEndpointRuntime(
              ASFW::Audio::Devices::AudioEndpointId{iv->endpointId})
        : nullptr;
}

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

    ivars->tuningStore = new (std::nothrow) ASFW::Audio::Shared::RuntimeTuningStore;
    if (!ivars->tuningStore || !ivars->tuningStore->IsValid()) {
        delete ivars->tuningStore;
        ivars->tuningStore = nullptr;
        return false;
    }
    ivars->parentDriver = nullptr;
    ivars->endpointId = 0;
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
        if (ivars->txTransportFaultAction) {
            ivars->txTransportFaultAction->release();
            ivars->txTransportFaultAction = nullptr;
        }
        if (ivars->ztsAnchorAction) {
            ivars->ztsAnchorAction->release();
            ivars->ztsAnchorAction = nullptr;
        }
        if (ivars->deviceClockChangedAction) {
            ivars->deviceClockChangedAction->release();
            ivars->deviceClockChangedAction = nullptr;
        }
        (void)RegisterRuntimeTuningRequestedAction(nullptr);
        if (ivars->deviceConfigurationRequestedAction) {
            ivars->deviceConfigurationRequestedAction->release();
            ivars->deviceConfigurationRequestedAction = nullptr;
        }
        delete ivars->tuningStore;
        ivars->tuningStore = nullptr;
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
        if (auto* coordinator = GetAudioCoordinator(ivars)) {
            coordinator->SetTxPreparationCallback({});
            coordinator->SetTxTransportFaultDispatch({});
            coordinator->SetClockAnchorReadyCallback({});
        }
        if (ivars->txPreparationAction) {
            ivars->txPreparationAction->release();
            ivars->txPreparationAction = nullptr;
        }
        if (ivars->txTransportFaultAction) {
            ivars->txTransportFaultAction->release();
            ivars->txTransportFaultAction = nullptr;
        }
        if (ivars->ztsAnchorAction) {
            ivars->ztsAnchorAction->release();
            ivars->ztsAnchorAction = nullptr;
        }
        if (ivars->deviceClockChangedAction) {
            ivars->deviceClockChangedAction->release();
            ivars->deviceClockChangedAction = nullptr;
        }
        (void)RegisterRuntimeTuningRequestedAction(nullptr);
        if (ivars->deviceConfigurationRequestedAction) {
            ivars->deviceConfigurationRequestedAction->release();
            ivars->deviceConfigurationRequestedAction = nullptr;
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

kern_return_t IMPL(ASFWAudioNub, GetTopologyRevision)
{
    if (!outTopologyRevision) {
        return kIOReturnBadArgument;
    }
    *outTopologyRevision = 0;
    if (!ivars) {
        return kIOReturnNotReady;
    }
    auto endpoint = FindEndpointRuntime(ivars);
    if (!endpoint) {
        return kIOReturnNotReady;
    }
    *outTopologyRevision = endpoint->CopyTopologyRevision();
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWAudioNub, RegisterTxPreparationAction)
{
    if (!ivars) {
        return kIOReturnNotReady;
    }

    auto* coordinator = GetAudioCoordinator(ivars);
    if (!coordinator) {
        return kIOReturnNotReady;
    }

    if (action) {
        action->retain();
    }
    OSAction* oldAction = ivars->txPreparationAction;
    ivars->txPreparationAction = action;

    if (action) {
        coordinator->SetTxPreparationCallback(
            [this](uint64_t generation) {
                if (ivars && ivars->txPreparationAction) {
                    TxPreparationReady(
                        ivars->txPreparationAction, generation);
                }
            });
    } else {
        coordinator->SetTxPreparationCallback({});
    }

    if (oldAction) {
        oldAction->release();
    }
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWAudioNub, RegisterTxTransportFaultAction)
{
    if (!ivars) {
        return kIOReturnNotReady;
    }

    auto* coordinator = GetAudioCoordinator(ivars);
    if (!coordinator) {
        return kIOReturnNotReady;
    }

    if (action) {
        action->retain();
    }
    OSAction* oldAction = ivars->txTransportFaultAction;
    ivars->txTransportFaultAction = action;

    if (action) {
        // Runs on the isoch watchdog/poll thread. Firing the action is the
        // whole body on purpose: it must not block that thread.
        coordinator->SetTxTransportFaultDispatch(
            [this](uint32_t statusRaw, uint64_t streamGeneration) {
                if (ivars && ivars->txTransportFaultAction) {
                    TxTransportFaultReady(ivars->txTransportFaultAction,
                                          statusRaw, streamGeneration);
                }
            });
    } else {
        coordinator->SetTxTransportFaultDispatch({});
    }

    if (oldAction) {
        oldAction->release();
    }
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWAudioNub, RecoverAudioStreamingAfterTxFault)
{
    if (!ivars || ivars->endpointId == 0) {
        return kIOReturnNotReady;
    }

    auto* coordinator = GetAudioCoordinator(ivars);
    if (!coordinator) {
        return kIOReturnNotReady;
    }

    coordinator->RecoverAfterTxTransportFault(
        ASFW::Audio::Devices::AudioEndpointId{ivars->endpointId});
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWAudioNub, RequestTxPreparation)
{
    if (!ivars || !ivars->txPreparationAction) {
        return kIOReturnNotReady;
    }
    TxPreparationReady(ivars->txPreparationAction, generation);
    return kIOReturnSuccess;
}

void IMPL(ASFWAudioNub, TxPreparationReady)
{
    (void)action;
    (void)generation;
}

// Required by the generated dispatch table. The nub only originates this
// action; ASFWAudioDriver's override is what actually handles it.
void IMPL(ASFWAudioNub, TxTransportFaultReady)
{
    (void)action;
    (void)statusRaw;
    (void)streamGeneration;
}

kern_return_t IMPL(ASFWAudioNub, RegisterZtsAnchorAction)
{
    if (!ivars) {
        return kIOReturnNotReady;
    }

    auto* coordinator = GetAudioCoordinator(ivars);
    if (!coordinator) {
        return kIOReturnNotReady;
    }

    if (action) {
        action->retain();
    }
    OSAction* oldAction = ivars->ztsAnchorAction;
    ivars->ztsAnchorAction = action;

    if (action) {
        coordinator->SetClockAnchorReadyCallback(
            [this](uint64_t generation) {
                if (ivars && ivars->ztsAnchorAction) {
                    ZtsAnchorReady(
                        ivars->ztsAnchorAction, generation);
                }
            });
    } else {
        coordinator->SetClockAnchorReadyCallback({});
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
             "ASFWAudioNub: NotifyDeviceClockChanged %u Hz endpoint=%llu",
             nominalRateHz, ivars->endpointId);
    DeviceClockChanged(ivars->deviceClockChangedAction, nominalRateHz);
}

kern_return_t IMPL(ASFWAudioNub, RegisterDeviceConfigurationRequestedAction)
{
    if (!ivars) return kIOReturnNotReady;
    if (action) action->retain();
    OSAction* oldAction = ivars->deviceConfigurationRequestedAction;
    ivars->deviceConfigurationRequestedAction = action;
    if (oldAction) oldAction->release();
    return kIOReturnSuccess;
}

void IMPL(ASFWAudioNub, DeviceConfigurationRequested)
{
    (void)action;
    (void)sampleRateHz;
    (void)opticalInput;
    (void)opticalOutput;
}

bool ASFWAudioNub::NotifyDeviceConfigurationRequested(
    uint32_t sampleRateHz, uint32_t opticalInput, uint32_t opticalOutput)
{
    if (!ivars || !ivars->deviceConfigurationRequestedAction) return false;
    ASFW_LOG(Audio,
             "[AudioConfig] control request endpoint=%llu rate=%u opticalIn=%u opticalOut=%u",
             ivars->endpointId, sampleRateHz, opticalInput, opticalOutput);
    DeviceConfigurationRequested(ivars->deviceConfigurationRequestedAction,
                                 sampleRateHz, opticalInput, opticalOutput);
    return true;
}

void IMPL(ASFWAudioNub, RuntimeTuningRequested) {
    (void)action; (void)requestId;
}

kern_return_t IMPL(ASFWAudioNub, RegisterRuntimeTuningRequestedAction) {
    if (!ivars || !ivars->tuningStore) return kIOReturnNotReady;
    // Retain the incoming action and adopt it under the same lock that guards
    // the transaction state, so a concurrent Notify never pairs one request's
    // action with another's state. Retain before release, so re-registering the
    // same action cannot drop the last reference mid-swap.
    OSAction* previous = ivars->runtimeTuningRequestedAction;
    if (action) action->retain();
    ivars->tuningStore->WithLock([&](auto& state) {
        previous = ivars->runtimeTuningRequestedAction;
        ivars->runtimeTuningRequestedAction = action;
        if (!action) state.Disconnect(kIOReturnNotReady);
    });
    if (previous) previous->release();
    return kIOReturnSuccess;
}

kern_return_t ASFWAudioNub::NotifyRuntimeTuningRequested(
    const ASFW::Audio::Shared::AudioRuntimeTuning& candidate, uint32_t groups) {
    using namespace ASFW::Audio::Shared;
    if (!ivars || !ivars->tuningStore) return kIOReturnNotReady;
    OSAction* target = nullptr;
    uint32_t id = 0;
    auto result = ivars->tuningStore->WithLock([&](auto& state) -> kern_return_t {
        if (!ivars->runtimeTuningRequestedAction) return kIOReturnNotReady;
        const auto submitted = state.Submit(candidate, groups);
        if (!submitted) {
            switch (submitted.error()) {
                case TuningRejection::kBusy: return kIOReturnBusy;
                case TuningRejection::kNotReady: return kIOReturnNotReady;
                case TuningRejection::kUnsupportedGroups: return kIOReturnUnsupported;
                default: return kIOReturnBadArgument;
            }
        }
        id = *submitted;
        if (id) { target = ivars->runtimeTuningRequestedAction; target->retain(); }
        return kIOReturnSuccess;
    });
    if (target) { RuntimeTuningRequested(target, id); target->release(); }
    return result;
}

bool ASFWAudioNub::BeginRuntimeTuningWindow(uint32_t id) {
    return ivars && ivars->tuningStore && ivars->tuningStore->WithLock(
        [&](auto& state) { return state.RequestWindow(id); });
}

bool ASFWAudioNub::CommitRuntimeTuning(uint32_t id,
    ASFW::Audio::Shared::AudioRuntimeTuning& active) {
    if (!ivars || !ivars->tuningStore) return false;
    return ivars->tuningStore->WithLock([&](auto& state) {
        const auto candidate = state.BeginApply(id);
        if (!candidate) return false;
        active = *candidate;
        return state.Finish(id, 0);
    });
}

void ASFWAudioNub::CompleteRuntimeTuning(uint32_t id, uint32_t error, bool aborted) {
    if (ivars && ivars->tuningStore) ivars->tuningStore->WithLock(
        [&](auto& state) { (void)state.Finish(id, error, aborted); });
}

void ASFWAudioNub::PublishRuntimeTuningGraph(
    const ASFW::Audio::Shared::AudioRuntimeTuning& active,
    uint32_t rate, uint32_t inputs, uint32_t outputs) {
    if (ivars && ivars->tuningStore) ivars->tuningStore->WithLock(
        [&](auto& state) { state.PublishGraph(active, rate, inputs, outputs); });
}

bool ASFWAudioNub::CopyRuntimeTuningSnapshot(
    ASFW::Audio::Shared::RuntimeTuningSnapshot& out) const {
    if (!ivars || !ivars->tuningStore) return false;
    out = ivars->tuningStore->WithLock([](auto& state) { return state.Copy(); });
    // Success means "there is a snapshot", NOT "the graph is ready". Gating on
    // readiness here made the snapshot's own `ready` field unreachable and hid
    // the outcome of a request exactly when it mattered: Disconnect() aborts a
    // parked request and clears ready in the same call, so refusing the read
    // threw away the only record that the abort had happened. The caller reads
    // `out.ready` and presents a disabled panel instead.
    return true;
}

void ASFWAudioNub::SetAudioIoRunning(bool running, uint32_t rate) {
    if (ivars && ivars->tuningStore) ivars->tuningStore->WithLock(
        [&](auto& state) { state.SetStreaming(running, rate); });
}

uint32_t ASFWAudioNub::CurrentRuntimeTuningWindow() const {
    ASFW::Audio::Shared::RuntimeTuningSnapshot snapshot{};
    (void)CopyRuntimeTuningSnapshot(snapshot);
    return snapshot.requestId;
}

uint32_t ASFWAudioNub::GetCurrentSampleRateHz() const
{
    return ivars ? ivars->currentSampleRateHz : 0;
}

ASFWDriver* ASFWAudioNub::GetParentDriver() const
{
    return ivars ? OSDynamicCast(ASFWDriver, ivars->parentDriver) : nullptr;
}

void ASFWAudioNub::RegisterTxLatencySession(void* session)
{
    if (auto endpoint = FindEndpointRuntime(ivars)) {
        if (session) {
            auto* pShared = reinterpret_cast<std::shared_ptr<ASFW::Audio::Runtime::TxLatencySession>*>(session);
            endpoint->RegisterTxLatencySession(*pShared);
        }
    }
}

void ASFWAudioNub::UnregisterTxLatencySession(void* session)
{
    if (auto endpoint = FindEndpointRuntime(ivars)) {
        if (session) {
            auto* pShared = reinterpret_cast<std::shared_ptr<ASFW::Audio::Runtime::TxLatencySession>*>(session);
            endpoint->UnregisterTxLatencySession(*pShared);
        } else {
            endpoint->UnregisterTxLatencySession(nullptr);
        }
    }
}


kern_return_t IMPL(ASFWAudioNub, StartAudioStreaming)
{
    if (!ivars || ivars->endpointId == 0) {
        return kIOReturnNotReady;
    }

    auto endpoint = FindEndpointRuntime(ivars);
    if (!endpoint) {
        ASFW_LOG(DirectAudio,
                 "ADK FATAL StartAudioStreaming missing endpoint runtime endpoint=%llu",
                 ivars->endpointId);
        return kIOReturnNotReady;
    }

    if (!endpoint->HasCompleteDirectAudioMemory()) {
        ASFW_LOG(DirectAudio,
                 "ADK FATAL StartAudioStreaming direct memory not ready endpoint=%llu",
                 ivars->endpointId);
        return kIOReturnNotReady;
    }

    // Auto-start gating (Info.plist + runtime), useful for debugging discovery without streams.
    if (!ASFW::LogConfig::Shared().IsAudioAutoStartEnabled()) {
        ASFW_LOG(Audio,
                 "ASFWAudioNub: StartAudioStreaming skipped (auto-start disabled) endpoint=%llu",
                 ivars->endpointId);
        return kIOReturnSuccess;
    }

    auto* coordinator = GetAudioCoordinator(ivars);
    if (!coordinator) {
        ASFW_LOG(Audio, "ASFWAudioNub: StartAudioStreaming: missing AudioCoordinator");
        return kIOReturnNotReady;
    }

    const IOReturn kr = coordinator->StartStreaming(
        ASFW::Audio::Devices::AudioEndpointId{ivars->endpointId});
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Audio,
                 "ASFWAudioNub: StartAudioStreaming failed endpoint=%llu kr=0x%x",
                 ivars->endpointId, kr);
    } else {
        endpoint->MarkStreaming(true);
    }
    return kr;
}

kern_return_t IMPL(ASFWAudioNub, StopAudioStreaming)
{
    if (!ivars || ivars->endpointId == 0) {
        return kIOReturnNotReady;
    }

    auto* coordinator = GetAudioCoordinator(ivars);
    if (!coordinator) {
        return kIOReturnNotReady;
    }

    const IOReturn kr = coordinator->StopStreaming(
        ASFW::Audio::Devices::AudioEndpointId{ivars->endpointId});
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Audio,
                 "ASFWAudioNub: StopAudioStreaming failed endpoint=%llu kr=0x%x",
                 ivars->endpointId, kr);
    }
    if (auto endpoint = FindEndpointRuntime(ivars)) {
        endpoint->MarkStreaming(false);
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
                 "ADK DBG MEM copy failed missing_endpoint_runtime endpoint=%llu",
                 ivars->endpointId);
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

// Producer-to-transport doorbell. The content side has published one or more
// replacement payload images; bind what is still reachable now rather than at
// the next completion interrupt, which arrives after the hardware has passed
// the packets the offers were made for.
//
// Servicing deliberately does not advance the finality frontier: finality is
// paced by descriptor recycling, and sealing here would freeze packets sooner
// than the cadence implies and lose more content, not less.
//
// Synchronous rather than an async OSAction hop. Every core-to-audio callback
// (TxPreparationReady, TxTransportFaultReady, ZtsAnchorReady) is already an
// async action, so there is no synchronous back-edge to invert against and no
// deadlock order to establish. It also keeps the sweep inside the caller's
// stream lifetime instead of handing a queued block state that can outlive
// teardown -- the FW-60 shape. The cost is that the audio preparation queue
// waits out one cross-service call per fill batch; measure that on hardware
// before assuming it is free.
kern_return_t IMPL(ASFWAudioNub, ServiceLatePayloadOffers)
{
    if (!ivars) {
        return kIOReturnNotReady;
    }
    ASFWDriver* parent = GetParentASFWDriver(ivars);
    auto* ctx = parent ? static_cast<ServiceContext*>(parent->GetServiceContext()) : nullptr;
    if (!ctx) {
        return kIOReturnNotReady;
    }

    auto* transmit = ctx->isoch.TransmitContext(streamIndex);
    if (!transmit) {
        return kIOReturnNoDevice;
    }

    // Re-checks its own running state and takes the refill gate; a stream that
    // stopped between the offer and this call services nothing.
    transmit->ServiceLatePayloadOffers();
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWAudioNub, CopyMidiTransportMemory)
{
    if (!ivars || ivars->midiTransportMemory == nullptr) return kIOReturnNotReady;
    if (outMemory == nullptr) return kIOReturnBadArgument;
    ivars->midiTransportMemory->retain();
    *outMemory = ivars->midiTransportMemory;
    if (outStreamEpoch) *outStreamEpoch = ivars->midiStreamEpoch;
    if (outSlotIndex) *outSlotIndex = ivars->midiSlotIndex;
    if (outDbs) *outDbs = ivars->midiDbs;
    if (outPortCount) *outPortCount = ivars->midiPortCount;
    if (outDbcAligned) *outDbcAligned = ivars->midiDbcAligned;
    return kIOReturnSuccess;
}

void ASFWAudioNub::SetMidiTransportSource(IOMemoryDescriptor* memory,
                                          uint64_t streamEpoch,
                                          uint32_t slotIndex, uint32_t dbs,
                                          uint32_t portCount,
                                          uint32_t dbcAligned) {
    if (!ivars) return;
    // Raw retained pointer: the ivars struct is declared in the .iig, which has
    // no OSSharedPtr. Release the previous one before taking the new reference.
    if (ivars->midiTransportMemory != nullptr) {
        ivars->midiTransportMemory->release();
        ivars->midiTransportMemory = nullptr;
    }
    if (memory != nullptr) {
        memory->retain();
        ivars->midiTransportMemory = memory;
    }
    ivars->midiStreamEpoch = streamEpoch;
    ivars->midiSlotIndex = slotIndex;
    ivars->midiDbs = dbs;
    ivars->midiPortCount = portCount;
    ivars->midiDbcAligned = dbcAligned;
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

namespace {

[[nodiscard]] std::optional<ASFW::Configuration::OpticalMode>
DecodeOpticalMode(uint32_t raw) noexcept {
    switch (raw) {
    case 1:
        return ASFW::Configuration::OpticalMode::Adat;
    case 2:
        return ASFW::Configuration::OpticalMode::Spdif;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] bool BuildDeviceConfiguration(
    uint32_t sampleRateHz, uint32_t opticalInput, uint32_t opticalOutput,
    ASFW::Configuration::DeviceConfiguration& out) noexcept {
    if (sampleRateHz == 0 || opticalInput > 2U || opticalOutput > 2U) {
        return false;
    }
    out = {
        .sampleRate = sampleRateHz,
        .opticalInput = DecodeOpticalMode(opticalInput),
        .opticalOutput = DecodeOpticalMode(opticalOutput),
    };
    return true;
}

} // namespace

// IIG dispatch across queues, not across processes: it runs on the nub side,
// where parent -> ServiceContext -> AudioCoordinator is valid. Only bounded
// scalar configuration crosses this seam; the ADK service remains the sole
// owner of AudioDriverKit objects and descriptor mappings.
kern_return_t IMPL(ASFWAudioNub, ApplyDeviceConfiguration)
{
    if (outInputChannels) *outInputChannels = 0;
    if (outOutputChannels) *outOutputChannels = 0;
    if (!ivars || !outInputChannels || !outOutputChannels) {
        return kIOReturnBadArgument;
    }
    auto* coordinator = GetAudioCoordinator(ivars);
    ASFW::Configuration::DeviceConfiguration desired{};
    if (!coordinator || !BuildDeviceConfiguration(sampleRateHz, opticalInput,
                                                  opticalOutput, desired)) {
        return kIOReturnUnsupported;
    }

    ASFW::Audio::AudioConfigurationApplyResult result{};
    const kern_return_t kr = coordinator->ApplyDeviceConfiguration(
        ASFW::Audio::Devices::AudioEndpointId{ivars->endpointId}, desired, result);
    if (kr != kIOReturnSuccess) {
        return kr;
    }
    *outInputChannels = result.runtimeCaps.hostInputPcmChannels;
    *outOutputChannels = result.runtimeCaps.hostOutputPcmChannels;
    ASFW_LOG(Audio,
             "[AudioConfig] hardware accepted endpoint=%llu rate=%u in=%u out=%u",
             ivars->endpointId, sampleRateHz, *outInputChannels, *outOutputChannels);
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWAudioNub, CommitDeviceConfiguration)
{
    if (!ivars) {
        return kIOReturnNotReady;
    }
    auto* coordinator = GetAudioCoordinator(ivars);
    ASFW::Configuration::DeviceConfiguration confirmed{};
    if (!coordinator || !BuildDeviceConfiguration(sampleRateHz, opticalInput,
                                                  opticalOutput, confirmed)) {
        return kIOReturnUnsupported;
    }
    // Runtime caps come from the immutable profile; callers cannot invent a
    // geometry by choosing selector values. This lookup stays on the core side.
    auto* registry = GetAudioRuntimeRegistry(ivars);
    if (!registry) {
        return kIOReturnNotReady;
    }
    const auto resolved = registry->FindProfile(
        ASFW::Audio::Devices::AudioEndpointId{ivars->endpointId});
    const auto* capability = resolved ? resolved->ConfigurationFor(confirmed) : nullptr;
    if (!capability) {
        return kIOReturnUnsupported;
    }
    const kern_return_t kr = coordinator->CommitDeviceConfiguration(
        ASFW::Audio::Devices::AudioEndpointId{ivars->endpointId},
        {.configuration = confirmed, .runtimeCaps = capability->runtimeCaps});
    if (kr == kIOReturnSuccess) {
        ivars->currentSampleRateHz = sampleRateHz;
        ivars->inputChannelCount = capability->runtimeCaps.hostInputPcmChannels;
        ivars->outputChannelCount = capability->runtimeCaps.hostOutputPcmChannels;
        ivars->channelCount = std::max(ivars->inputChannelCount,
                                       ivars->outputChannelCount);
        ASFW_LOG(Audio,
                 "[AudioConfig] runtime committed endpoint=%llu rate=%u in=%u out=%u",
                 ivars->endpointId, sampleRateHz, ivars->inputChannelCount,
                 ivars->outputChannelCount);
    }
    return kr;
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

void ASFWAudioNub::SetAudioGeometry(uint32_t inputChannels,
                                    uint32_t outputChannels,
                                    uint32_t sampleRateHz,
                                    uint32_t streamModeRaw)
{
    if (!ivars) return;
    ivars->inputChannelCount = ClampAudioChannels(inputChannels);
    ivars->outputChannelCount = ClampAudioChannels(outputChannels);
    ivars->channelCount = std::max(ivars->inputChannelCount,
                                   ivars->outputChannelCount);
    ivars->currentSampleRateHz = sampleRateHz != 0 ? sampleRateHz : 48000U;
    ivars->streamModeRaw = streamModeRaw == 1U ? 1U : 0U;
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

void ASFWAudioNub::SetEndpointId(uint64_t endpointId)
{
    if (!ivars) {
        return;
    }
    ivars->endpointId = endpointId;
    ASFW_LOG(Audio, "ASFWAudioNub: endpoint set to %llu", endpointId);
}

uint64_t ASFWAudioNub::GetEndpointId() const
{
    return ivars ? ivars->endpointId : 0;
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
    (void)classIdFourCC;
    (void)element;
    if (!ivars || !outValue) {
        return kIOReturnBadArgument;
    }
    *outValue = false;
    return kIOReturnUnsupported;
}

kern_return_t IMPL(ASFWAudioNub, SetProtocolBooleanControl)
{
    (void)classIdFourCC;
    (void)element;
    (void)value;
    if (!ivars) {
        return kIOReturnNotReady;
    }
    return kIOReturnUnsupported;
}
