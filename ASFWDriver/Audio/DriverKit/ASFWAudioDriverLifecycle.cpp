//
// ASFWAudioDriverLifecycle.cpp
// ASFWDriver
//
// DriverKit lifecycle and ADK start/stop sequencing for ASFWAudioDriver.
#include "ASFWAudioDriverPrivate.hpp"
#include "../../Logging/Logging.hpp"
#include "../Config/TimingCursorPolicy.hpp"
#include "Config/AudioProfileRegistry.hpp"
#include "../../Common/DriverKitOwnership.hpp"
#include <DriverKit/DriverKit.h>

using ASFW::Audio::DriverKit::BuildAudioGraph;
using ASFW::Audio::DriverKit::EnsureZtsMirrorTimer;
using ASFW::Audio::DriverKit::PrimeSharedZeroTimestampToHAL;
using ASFW::Audio::DriverKit::ScheduleZtsMirrorTimer;
using ASFW::Audio::DriverKit::StopZtsMirrorTimer;
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
        TearDownAudioGraph(*this, *ivars, &graphState);
        (void)Stop(provider, SUPERDISPATCH);
        return result;
    };

    error = BuildAudioGraph(*this, provider, *ivars, graphState);
    if (error != kIOReturnSuccess) {
        return failStart(error, "BuildAudioGraph");
    }

    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWAudioDriver, Stop)
{
    ASFW_LOG(Audio, "ASFWAudioDriver: Stop()");

    if (ivars) {
        ivars->runtime.isRunning.store(false, std::memory_order_release);
        ivars->runtime.txPreparationNotificationScheduled.store(
            false, std::memory_order_release);
        ivars->runtime.ztsTimelineInitialized.store(false, std::memory_order_release);
        StopZtsMirrorTimer(*ivars);

        if (ivars->device.audioNub) {
            kern_return_t stopKr = ivars->device.audioNub->StopAudioStreaming();
            if (stopKr != kIOReturnSuccess) {
                ASFW_LOG(Audio, "ASFWAudioDriver: StopAudioStreaming failed in Stop(): 0x%x", stopKr);
            }
        }
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

    ivars->runtime.ioDebugCallbacks.store(0, std::memory_order_relaxed);
    ivars->ztsMirrorTimerTicks.store(0, std::memory_order_relaxed);
    ivars->runtime.ztsTimelineInitialized.store(false, std::memory_order_release);
    ivars->runtime.isRunning.store(false, std::memory_order_release);
    ivars->runtime.txPreparationNotificationScheduled.store(
        false, std::memory_order_release);
    ASFW_LOG(DirectAudio, "ADK DBG IO running=0 while arming transport");

    auto* control = ivars->runtime.directAudioGraph.control;
    if (control) {
        // Select RX before transport startup so the first usable RX SYT/ZTS
        // publication is accepted while IR is coming online.
        control->ztsState.selectedSource.store(
            ASFW::Audio::Runtime::ZtsAuthoritySource::RxClock,
            std::memory_order_release);
    }

    const kern_return_t superStartKr = super::StartDevice(in_object_id, in_flags);
    if (superStartKr != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioDriver: super::StartDevice failed: 0x%x", superStartKr);
        return superStartKr;
    }
    ASFW_LOG(DirectAudio, "ADK DBG IO super StartDevice ok id=%u", in_object_id);

    bool streamingStarted = false;
    const auto failStartDevice = [&](kern_return_t status, const char* stage) -> kern_return_t {
        const kern_return_t result = (status == kIOReturnSuccess) ? kIOReturnError : status;
        ivars->runtime.isRunning.store(false, std::memory_order_release);
        ivars->runtime.txPreparationNotificationScheduled.store(
            false, std::memory_order_release);
        ivars->runtime.ztsTimelineInitialized.store(false, std::memory_order_release);
        StopZtsMirrorTimer(*ivars);
        if (streamingStarted && ivars->device.audioNub) {
            const kern_return_t stopKr = ivars->device.audioNub->StopAudioStreaming();
            if (stopKr != kIOReturnSuccess) {
                ASFW_LOG(Audio,
                         "ASFWAudioDriver: StopAudioStreaming failed while unwinding StartDevice stage=%{public}s kr=0x%x",
                         stage ? stage : "unknown",
                         stopKr);
            }
        }
        ivars->txPayloadMap = nullptr;
        ivars->txMetadataMap = nullptr;
        ivars->txControlMap = nullptr;
        ivars->txPayloadBuffer = nullptr;
        ivars->txMetadataBuffer = nullptr;
        ivars->txControlBuffer = nullptr;
        if (ivars->runtime.txFloatScratchBuffer) {
            delete[] ivars->runtime.txFloatScratchBuffer;
            ivars->runtime.txFloatScratchBuffer = nullptr;
        }
        if (ivars->device.audioNub) {
            ivars->device.audioNub->FreeTxIsochResources();
        }
        (void)super::StopDevice(in_object_id, in_flags);
        ASFW_LOG(Audio,
                 "ASFWAudioDriver: StartDevice failed at %{public}s kr=0x%x",
                 stage ? stage : "unknown",
                 result);
        return result;
    };

    const kern_return_t startKr = ivars->device.audioNub->StartAudioStreaming();
    if (startKr != kIOReturnSuccess) {
        return failStartDevice(startKr, "StartAudioStreaming");
    }
    streamingStarted = true;

    // --- Allocate and map shared TX isoch resources ---
    {
        const auto* baseProfile = ASFW::Isoch::Audio::AudioProfileRegistry::FindProfile(
            ivars->device.vendorId,
            ivars->device.modelId,
            ivars->device.guid
        );
        const auto* profile = static_cast<const ASFW::Isoch::Audio::DICE::IDiceDeviceProfile*>(baseProfile);
        if (!profile) {
            ASFW_LOG(Audio, "ASFWAudioDriver: StartDevice failed - profile not found");
            return failStartDevice(kIOReturnError, "ResolveProfile");
        }

        ASFW::Isoch::Audio::DICE::DiceStreamConfig txConfig{};
        if (!profile->BuildDefaultTxStreamConfig(txConfig)) {
            ASFW_LOG(Audio, "ASFWAudioDriver: StartDevice failed - BuildDefaultTxStreamConfig failed");
            return failStartDevice(kIOReturnError, "BuildDefaultTxStreamConfig");
        }

        const uint32_t numSlots = 512;
        const uint32_t maxPacketBytes = 512;
        const uint32_t interruptInterval = 8;

        IOMemoryDescriptor* rawPayload = nullptr;
        IOMemoryDescriptor* rawMetadata = nullptr;
        IOMemoryDescriptor* rawControl = nullptr;

        kern_return_t kr = ivars->device.audioNub->AllocateTxIsochResources(
            numSlots,
            maxPacketBytes,
            interruptInterval,
            &rawPayload,
            &rawMetadata,
            &rawControl
        );
        if (kr != kIOReturnSuccess) {
            ASFW_LOG(Audio, "ASFWAudioDriver: AllocateTxIsochResources failed: 0x%x", kr);
            return failStartDevice(kr, "AllocateTxIsochResources");
        }

        ivars->txPayloadBuffer = ASFW::Common::AdoptRetained(rawPayload);
        ivars->txMetadataBuffer = ASFW::Common::AdoptRetained(rawMetadata);
        ivars->txControlBuffer = ASFW::Common::AdoptRetained(rawControl);

        kr = ASFW::Common::CreateSharedMapping(ivars->txPayloadBuffer, ivars->txPayloadMap);
        if (kr != kIOReturnSuccess) {
            return failStartDevice(kr, "CreateSharedMapping txPayload");
        }
        kr = ASFW::Common::CreateSharedMapping(ivars->txMetadataBuffer, ivars->txMetadataMap);
        if (kr != kIOReturnSuccess) {
            return failStartDevice(kr, "CreateSharedMapping txMetadata");
        }
        kr = ASFW::Common::CreateSharedMapping(ivars->txControlBuffer, ivars->txControlMap);
        if (kr != kIOReturnSuccess) {
            return failStartDevice(kr, "CreateSharedMapping txControl");
        }

        uint8_t* payloadBase = reinterpret_cast<uint8_t*>(ivars->txPayloadMap->GetAddress());
        auto* metadataRing = reinterpret_cast<ASFW::IsochTransport::TxPacketMeta*>(ivars->txMetadataMap->GetAddress());
        auto* controlBlock = reinterpret_cast<ASFW::IsochTransport::TxStreamControl*>(ivars->txControlMap->GetAddress());

        ivars->runtime.txSlotProvider.payloadBase = payloadBase;
        ivars->runtime.txSlotProvider.metadataRing = metadataRing;
        ivars->runtime.txSlotProvider.controlBlock = controlBlock;
        ivars->runtime.txSlotProvider.numSlots = numSlots;
        ivars->runtime.txSlotProvider.slotStrideBytes = maxPacketBytes;
        ivars->runtime.txSlotProvider.isoChannel = txConfig.sid;

        ivars->runtime.txCycleTimeline.controlBlock = controlBlock;
        ivars->runtime.txCycleTimeline.wrapCount = 0;
        ivars->runtime.txCycleTimeline.lastSeconds = 0;

        if (!ivars->runtime.txStreamEngine.Configure(*profile, txConfig)) {
            ASFW_LOG(Audio, "ASFWAudioDriver: txStreamEngine Configure failed");
            return failStartDevice(kIOReturnError, "txStreamEngine.Configure");
        }
        ivars->runtime.txStreamEngine.BindSlotProvider(&ivars->runtime.txSlotProvider);
        ivars->runtime.txStreamEngine.ResetForStart(0, 0);

        ASFW::Driver::TxTimingModel::Config timeConfig{};
        ivars->runtime.txTimingModel.Configure(timeConfig);

        ivars->runtime.txFloatScratchBuffer = new float[32768]();

        ASFW_LOG(Audio, "ASFWAudioDriver: Allocated & configured TX isoch resources channel=%u", txConfig.sid);
    }

    const auto policy = ASFW::Audio::TimingCursorPolicy::MakeDice48kBlocking();
    const auto policySnap = policy.Snapshot();
    ASFW_LOG(Audio,
             "TimingCursorPolicy rate=%u mode=blocking framesPerPacket=%u outCursorOffset=%u inCursorOffset=%u reportedOutLatency=%u reportedInLatency=%u outSafety=%u inSafety=%u outLead=%u inLead=%u ztsPeriod=%u",
             policySnap.sampleRateHz,
             policySnap.framesPerPacketMax,
             policySnap.outputCursorOffsetFrames,
             policySnap.inputCursorOffsetFrames,
             policySnap.reportedOutputLatencyFrames,
             policySnap.reportedInputLatencyFrames,
             policySnap.outputSafetyOffsetFrames,
             policySnap.inputSafetyOffsetFrames,
             policySnap.outputPacketLeadFrames,
             policySnap.inputPacketLeadFrames,
             policySnap.ztsPeriodFrames);

    if (EnsureZtsMirrorTimer(*this, *ivars)) {
        ASFW_LOG(DirectAudio,
                 "ADK DBG ZTS mirror pump armed guid=0x%016llx periodUsec=%llu",
                 ivars->device.guid,
                 kZtsMirrorPumpPeriodUsec);
    } else {
        return failStartDevice(kIOReturnNoResources, "EnsureZtsMirrorTimer");
    }

    if (!PrimeSharedZeroTimestampToHAL(*ivars)) {
        ASFW_LOG(DirectAudio,
                 "ADK WARN ZTS mirror prime_timeout guid=0x%016llx rxZts=%llu rxAdk=%llu",
                 ivars->device.guid,
                 control ? control->counters.ztsRxPublished.load(std::memory_order_relaxed) : 0,
                 control ? control->counters.ztsRxAdkPublished.load(std::memory_order_relaxed) : 0);
        return failStartDevice(kIOReturnNotReady, "PrimeSharedZeroTimestampToHAL");
    }

    if (!ivars->runtime.directAudioGraph.audioDevice) {
        ASFW_LOG(DirectAudio,
                 "ADK FATAL DUPLEX null_audioDevice_after_start guid=0x%016llx skeleton=%d control=%p hasIn=%d hasOut=%d",
                 ivars->device.guid,
                 ivars->runtime.directAudioSkeletonBound.load(std::memory_order_acquire),
                 static_cast<void*>(ivars->runtime.directAudioGraph.control),
                 ivars->runtime.directAudioGraph.HasInput(),
                 ivars->runtime.directAudioGraph.HasOutput());
        return failStartDevice(kIOReturnNotReady, "direct graph audioDevice");
    }
    ivars->runtime.isRunning.store(true, std::memory_order_release);
    ivars->runtime.txPreparationNotificationScheduled.store(
        false, std::memory_order_release);
    ASFW_LOG(DirectAudio, "ADK DBG IO running=1 after transport ready");
    ScheduleZtsMirrorTimer(*ivars);
    ASFW_LOG(DirectAudio,
             "ADK DBG ZTS mirror pump started guid=0x%016llx periodUsec=%llu",
             ivars->device.guid,
             kZtsMirrorPumpPeriodUsec);
    ASFW_LOG(DirectAudio,
             "ADK DBG DUPLEX ready guid=0x%016llx rxStarted=1 txStarted=1 bindValid=%d hasIn=%d hasOut=%d audioDevice=%p zts=%llu rxZts=%llu rxAdk=%llu beginRead=%llu writeEnd=%llu writtenEndFrame=%llu",
             ivars->device.guid,
             ivars->runtime.directAudioSkeletonBound.load(std::memory_order_acquire),
             ivars->runtime.directAudioGraph.HasInput(),
             ivars->runtime.directAudioGraph.HasOutput(),
             static_cast<void*>(ivars->runtime.directAudioGraph.audioDevice),
             control ? control->counters.ztsPublished.load(std::memory_order_relaxed) : 0,
             control ? control->counters.ztsRxPublished.load(std::memory_order_relaxed) : 0,
             control ? control->counters.ztsRxAdkPublished.load(std::memory_order_relaxed) : 0,
             control ? control->counters.ioBeginReadCount.load(std::memory_order_relaxed) : 0,
             control ? control->counters.ioWriteEndCount.load(std::memory_order_relaxed) : 0,
             control ? control->client.outputClientWriteEndFrame.load(std::memory_order_acquire) : 0);
    if (control && control->counters.ioWriteEndCount.load(std::memory_order_relaxed) == 0) {
        ASFW_LOG(DirectAudio,
                 "ADK WARN OUTPUT no_write_end_yet guid=0x%016llx start completed but CoreAudio has not written playback PCM",
                 ivars->device.guid);
    }

    ASFW_LOG(Audio, "ASFWAudioDriver: Device started");
    return kIOReturnSuccess;
}

kern_return_t ASFWAudioDriver::StopDevice(IOUserAudioObjectID in_object_id,
                                          IOUserAudioStartStopFlags in_flags)
{
    ASFW_LOG(Audio, "ASFWAudioDriver: StopDevice(id=%u)", in_object_id);
    if (ivars) {
        ivars->runtime.isRunning.store(false, std::memory_order_release);
        ivars->runtime.txPreparationNotificationScheduled.store(
            false, std::memory_order_release);
        ivars->runtime.ztsTimelineInitialized.store(false, std::memory_order_release);
        StopZtsMirrorTimer(*ivars);
    }
    if (ivars && ivars->runtime.directAudioGraph.control) {
        const auto* control = ivars->runtime.directAudioGraph.control;
        ASFW_LOG(DirectAudio,
                 "ADK DBG DUPLEX stopping guid=0x%016llx zts=%llu rxZts=%llu rxAdk=%llu beginRead=%llu writeEnd=%llu writtenEndFrame=%llu txPackets=%llu txSilence=%llu txUnderruns=%llu",
                 ivars->device.guid,
                 control->counters.ztsPublished.load(std::memory_order_relaxed),
                 control->counters.ztsRxPublished.load(std::memory_order_relaxed),
                 control->counters.ztsRxAdkPublished.load(std::memory_order_relaxed),
                 control->counters.ioBeginReadCount.load(std::memory_order_relaxed),
                 control->counters.ioWriteEndCount.load(std::memory_order_relaxed),
                 control->client.outputClientWriteEndFrame.load(std::memory_order_acquire),
                 control->counters.txPackets.load(std::memory_order_relaxed),
                 control->counters.txSilenceSubstitutions.load(std::memory_order_relaxed),
                 control->counters.txUnderruns.load(std::memory_order_relaxed));
    }

    if (ivars && ivars->device.audioNub) {
        const kern_return_t stopKr = ivars->device.audioNub->StopAudioStreaming();
        if (stopKr != kIOReturnSuccess) {
            ASFW_LOG(Audio, "ASFWAudioDriver: StopAudioStreaming failed: 0x%x", stopKr);
        }
    }

    if (ivars) {
        ivars->txPayloadMap = nullptr;
        ivars->txMetadataMap = nullptr;
        ivars->txControlMap = nullptr;
        ivars->txPayloadBuffer = nullptr;
        ivars->txMetadataBuffer = nullptr;
        ivars->txControlBuffer = nullptr;
        if (ivars->runtime.txFloatScratchBuffer) {
            delete[] ivars->runtime.txFloatScratchBuffer;
            ivars->runtime.txFloatScratchBuffer = nullptr;
        }
        if (ivars->device.audioNub) {
            ivars->device.audioNub->FreeTxIsochResources();
        }
    }

    const kern_return_t superStopKr = super::StopDevice(in_object_id, in_flags);
    if (superStopKr != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioDriver: super::StopDevice failed: 0x%x", superStopKr);
    }

    return superStopKr;
}

namespace ASFW::Audio::DriverKit {

void PerformLoudTeardown(ASFWAudioDriver_IVars& ivars, const char* reason) noexcept {
    // 1. Immediately clear isRunning to stop pump and block further IO
    ivars.runtime.isRunning.store(false, std::memory_order_release);
    ivars.runtime.txPreparationNotificationScheduled.store(
        false, std::memory_order_release);
    ivars.runtime.ztsTimelineInitialized.store(false, std::memory_order_release);
    
    // 2. Stop ZTS mirror timer
    StopZtsMirrorTimer(ivars);

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
        if (ivars.runtime.txFloatScratchBuffer) {
            delete[] ivars.runtime.txFloatScratchBuffer;
            ivars.runtime.txFloatScratchBuffer = nullptr;
        }
        ivars.device.audioNub->FreeTxIsochResources();
    }

    // 4. Emit final diagnostic snapshot
    DirectDiagnostics::ForceLogDirectAudioDebugSnapshot(ivars.runtime, reason);
}

} // namespace ASFW::Audio::DriverKit
