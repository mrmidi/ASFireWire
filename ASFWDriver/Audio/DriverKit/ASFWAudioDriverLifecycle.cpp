//
// ASFWAudioDriverLifecycle.cpp
// ASFWDriver
//
// DriverKit lifecycle and ADK start/stop sequencing for ASFWAudioDriver.
#include "ASFWAudioDriverPrivate.hpp"
#include "ASFWAudioDevice.h"
#include "../../Logging/Logging.hpp"
#include "../Config/TimingCursorPolicy.hpp"
#include "Config/AudioProfileRegistry.hpp"
#include "../../Common/DriverKitOwnership.hpp"
#include <DriverKit/DriverKit.h>

using ASFW::Audio::DriverKit::BuildAudioGraph;
using ASFW::Audio::DriverKit::TearDownAudioGraph;

kern_return_t IMPL(ASFWAudioDriver, Start)
{
    ASFW_LOG(Audio, "ASFWAudioDriver: Start() - provider is ASFWAudioNub");

    if (!ivars) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Start() failed - ivars not initialized");
        return kIOReturnNotReady;
    }
    if (!provider) {
        ASFW_LOG(Audio, "ASFWAudioDriver: Start() failed - null provider");
        return kIOReturnBadArgument;
    }

    kern_return_t error = Start(provider, SUPERDISPATCH);
    if (error != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioDriver: super::Start() failed: %d", error);
        return error;
    }

    AudioGraphStartState graphState{};
    auto failStart = [&](kern_return_t status, const char* stage) -> kern_return_t {
        const kern_return_t result = (status == kIOReturnSuccess) ? kIOReturnError : status;
        ASFW_LOG(Audio,
                 "ASFWAudioDriver: Start() failed at %{public}s kr=0x%x - unwinding partial ADK graph",
                 stage ? stage : "unknown",
                 result);
        if (ivars->device.audioNub) {
            (void)ivars->device.audioNub->RegisterZtsAnchorAction(nullptr);
            (void)ivars->device.audioNub->RegisterTxPreparationAction(nullptr);
        }
        ivars->ztsAnchorAction.reset();
        ivars->ztsQueue.reset();
        ivars->txPreparationAction.reset();
        ivars->txPreparationQueue.reset();
        TearDownAudioGraph(*this, *ivars, &graphState);
        (void)Stop(provider, SUPERDISPATCH);
        return result;
    };

    error = BuildAudioGraph(*this, provider, *ivars, graphState);
    if (error != kIOReturnSuccess) {
        return failStart(error, "BuildAudioGraph");
    }

    IODispatchQueue* rawTxPreparationQueue = nullptr;
    error = IODispatchQueue::Create(
        "com.asfw.audio.tx-preparation", 0, 0, &rawTxPreparationQueue);
    if (error != kIOReturnSuccess || !rawTxPreparationQueue) {
        return failStart(
            error == kIOReturnSuccess ? kIOReturnNoMemory : error,
            "CreateTxPreparationDispatchQueue");
    }
    ivars->txPreparationQueue =
        ASFW::Common::AdoptRetained(rawTxPreparationQueue);
    error = SetDispatchQueue(
        "TxPreparation", ivars->txPreparationQueue.get());
    if (error != kIOReturnSuccess) {
        ivars->txPreparationQueue.reset();
        return failStart(error, "BindTxPreparationDispatchQueue");
    }

    OSAction* rawTxPreparationAction = nullptr;
    error = CreateActionTxPreparationReady(
        0, &rawTxPreparationAction);
    if (error != kIOReturnSuccess || !rawTxPreparationAction) {
        return failStart(
            error == kIOReturnSuccess ? kIOReturnNoMemory : error,
            "CreateActionTxPreparationReady");
    }
    ivars->txPreparationAction =
        ASFW::Common::AdoptRetained(rawTxPreparationAction);
    error = ivars->device.audioNub->RegisterTxPreparationAction(
        ivars->txPreparationAction.get());
    if (error != kIOReturnSuccess) {
        ivars->txPreparationAction.reset();
        return failStart(error, "RegisterTxPreparationAction");
    }

    IODispatchQueue* rawZtsQueue = nullptr;
    error = IODispatchQueue::Create(
        "com.asfw.audio.zts", 0, 0, &rawZtsQueue);
    if (error != kIOReturnSuccess || !rawZtsQueue) {
        return failStart(
            error == kIOReturnSuccess ? kIOReturnNoMemory : error,
            "CreateZtsDispatchQueue");
    }
    ivars->ztsQueue =
        ASFW::Common::AdoptRetained(rawZtsQueue);
    error = SetDispatchQueue(
        "Zts", ivars->ztsQueue.get());
    if (error != kIOReturnSuccess) {
        ivars->ztsQueue.reset();
        return failStart(error, "BindZtsDispatchQueue");
    }

    OSAction* rawZtsAnchorAction = nullptr;
    error = CreateActionZtsAnchorReady(
        0, &rawZtsAnchorAction);
    if (error != kIOReturnSuccess || !rawZtsAnchorAction) {
        return failStart(
            error == kIOReturnSuccess ? kIOReturnNoMemory : error,
            "CreateActionZtsAnchorReady");
    }
    ivars->ztsAnchorAction =
        ASFW::Common::AdoptRetained(rawZtsAnchorAction);
    error = ivars->device.audioNub->RegisterZtsAnchorAction(
        ivars->ztsAnchorAction.get());
    if (error != kIOReturnSuccess) {
        ivars->ztsAnchorAction.reset();
        return failStart(error, "RegisterZtsAnchorAction");
    }

    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWAudioDriver, Stop)
{
    ASFW_LOG(Audio, "ASFWAudioDriver: Stop()");

    if (ivars) {
        ivars->runtime.isRunning.store(false, std::memory_order_release);
        if (ivars->device.audioNub) {
            kern_return_t stopKr = ivars->device.audioNub->StopAudioStreaming();
            if (stopKr != kIOReturnSuccess) {
                ASFW_LOG(Audio, "ASFWAudioDriver: StopAudioStreaming failed in Stop(): 0x%x", stopKr);
            }
            (void)ivars->device.audioNub->RegisterTxPreparationAction(nullptr);
            (void)ivars->device.audioNub->RegisterZtsAnchorAction(nullptr);
        }
        ivars->txPreparationAction.reset();
        ivars->txPreparationQueue.reset();
        ivars->ztsAnchorAction.reset();
        ivars->ztsQueue.reset();
        ivars->device.audioNub = nullptr;
    }

    if (ivars && ivars->audioDevice) {
        RemoveObject(ivars->audioDevice.get());
    }

    return Stop(provider, SUPERDISPATCH);
}

kern_return_t ASFWAudioDriver::StartDevice(IOUserAudioObjectID in_object_id,
                                           IOUserAudioStartStopFlags in_flags)
{
    if (!ivars || !ivars->audioDevice) {
        ASFW_LOG(Audio, "ASFWAudioDriver: StartDevice failed - not initialized");
        return kIOReturnNotReady;
    }
    if (in_object_id != ivars->audioDevice->GetObjectID()) {
        ASFW_LOG(Audio,
                 "ASFWAudioDriver: StartDevice failed - unexpected object id=%u deviceId=%u",
                 in_object_id,
                 ivars->audioDevice->GetObjectID());
        return kIOReturnBadArgument;
    }
    if (!ivars->inputStream || !ivars->outputStream || !ivars->device.audioNub) {
        ASFW_LOG(Audio,
                 "ASFWAudioDriver: StartDevice failed - incomplete graph input=%p output=%p nub=%p",
                 static_cast<void*>(ivars->inputStream.get()),
                 static_cast<void*>(ivars->outputStream.get()),
                 static_cast<void*>(ivars->device.audioNub));
        return kIOReturnNotReady;
    }
    if (!ivars->runtime.directAudioSkeletonBound.load(std::memory_order_acquire) ||
        !ivars->runtime.directAudioGraph.control ||
        !ivars->runtime.directAudioGraph.HasInput() ||
        !ivars->runtime.directAudioGraph.HasOutput()) {
        ASFW_LOG(DirectAudio,
                 "ADK FATAL StartDevice direct graph not ready guid=0x%016llx skeleton=%d control=%p hasIn=%d hasOut=%d",
                 ivars->device.guid,
                 ivars->runtime.directAudioSkeletonBound.load(std::memory_order_acquire),
                 static_cast<void*>(ivars->runtime.directAudioGraph.control),
                 ivars->runtime.directAudioGraph.HasInput(),
                 ivars->runtime.directAudioGraph.HasOutput());
        return kIOReturnNotReady;
    }

    ASFW_LOG(Audio,
             "ASFWAudioDriver: StartDevice(id=%u flags=0x%llx deviceId=%u inStreamId=%u outStreamId=%u)",
             in_object_id,
             static_cast<uint64_t>(in_flags),
             ivars->audioDevice->GetObjectID(),
             ivars->inputStream ? ivars->inputStream->GetObjectID() : 0,
             ivars->outputStream ? ivars->outputStream->GetObjectID() : 0);

    // Transport setup (TX isoch, prefill, streaming start) is handled by
    // ASFWAudioDevice::StartIO, called by the framework via super::StartDevice.
    const kern_return_t superStartKr = super::StartDevice(in_object_id, in_flags);
    if (superStartKr != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioDriver: super::StartDevice failed: 0x%x", superStartKr);
        return superStartKr;
    }
    ASFW_LOG(DirectAudio, "ADK DBG IO super StartDevice ok id=%u", in_object_id);

    ASFW_LOG(Audio, "ASFWAudioDriver: Device started (transport via StartIO)");
    return kIOReturnSuccess;
}

kern_return_t ASFWAudioDriver::StopDevice(IOUserAudioObjectID in_object_id,
                                          IOUserAudioStartStopFlags in_flags)
{
    ASFW_LOG(Audio, "ASFWAudioDriver: StopDevice(id=%u)", in_object_id);

    // Transport teardown (stop streaming, free TX resources) is handled by
    // ASFWAudioDevice::StopIO, called by the framework via super::StopDevice.
    const kern_return_t superStopKr = super::StopDevice(in_object_id, in_flags);
    if (superStopKr != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioDriver: super::StopDevice failed: 0x%x", superStopKr);
    }

    return superStopKr;
}

namespace ASFW::Audio::DriverKit {

void PerformLoudTeardown(ASFWAudioDriver_IVars& ivars, const char* reason) noexcept {
    // 1. Immediately clear isRunning and txActive to stop pump and block further IO
    ivars.runtime.isRunning.store(false, std::memory_order_release);
    ivars.runtime.txActive.store(false, std::memory_order_release);

    ASFW_LOG(Audio, "ADK FATAL: TEARDOWN DUPLEX STREAM DUE TO: %{public}s", reason);

    // 3. Stop hardware isochronous streaming
    if (ivars.device.audioNub) {
        const kern_return_t stopKr = ivars.device.audioNub->StopAudioStreaming();
        if (stopKr != kIOReturnSuccess) {
            ASFW_LOG(Audio, "ASFWAudioDriver: StopAudioStreaming failed in loud teardown: 0x%x", stopKr);
        }
        
        ivars.txPayloadMap = nullptr;
        ivars.txMetadataMap = nullptr;
        ivars.txControlMap = nullptr;
        ivars.txPayloadBuffer = nullptr;
        ivars.txMetadataBuffer = nullptr;
        ivars.txControlBuffer = nullptr;
        ivars.runtime.txSlotProvider.payloadBase = nullptr;
        ivars.runtime.txSlotProvider.metadataRing = nullptr;
        ivars.runtime.txSlotProvider.controlBlock = nullptr;
        ivars.runtime.txSlotProvider.numSlots = 0;
        ivars.runtime.txExecutionTimeline.controlBlock = nullptr;

        // Secondary playback stream teardown (mirrors the master above).
        ivars.runtime.txSecondaryActive = false;
        ivars.txPayloadMapSecondary = nullptr;
        ivars.txMetadataMapSecondary = nullptr;
        ivars.txControlMapSecondary = nullptr;
        ivars.txPayloadBufferSecondary = nullptr;
        ivars.txMetadataBufferSecondary = nullptr;
        ivars.txControlBufferSecondary = nullptr;
        ivars.runtime.txSlotProviderSecondary.payloadBase = nullptr;
        ivars.runtime.txSlotProviderSecondary.metadataRing = nullptr;
        ivars.runtime.txSlotProviderSecondary.controlBlock = nullptr;
        ivars.runtime.txSlotProviderSecondary.numSlots = 0;

        ivars.device.audioNub->FreeTxIsochResources();
    }

    // 4. Emit final diagnostic snapshot
    DirectDiagnostics::ForceLogDirectAudioDebugSnapshot(ivars.runtime, reason);
}

} // namespace ASFW::Audio::DriverKit
