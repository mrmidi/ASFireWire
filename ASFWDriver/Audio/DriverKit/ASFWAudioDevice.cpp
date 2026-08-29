//
// ASFWAudioDevice.cpp
// ASFWDriver
//
// IOUserAudioDevice subclass implementing StartIO/StopIO for transport lifecycle.
//
#include <array>
#include <new>

#include "ASFWAudioDevice.h"
#include "ASFWAudioDriverPrivate.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Common/DriverKitOwnership.hpp"
#include "../../Isoch/Core/IsochTxQueue.hpp"

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>

struct ASFWAudioDevice_IVars {
    ASFWAudioDriver_IVars* driverIvars{nullptr};
    IOLock* configurationLock{nullptr};
    ASFW::Configuration::Machine configurationMachine{};
    bool configurationEnabled{false};
    // Host-owned ADK configuration lifecycle observability. These tokens do
    // not control the reducer; they only correlate request/StopIO/Perform/
    // StartIO across the two service queues.
    std::atomic<uint64_t> configurationInFlightToken{0};
    std::atomic<uint64_t> configurationResumeToken{0};
};

bool ASFWAudioDevice::init(IOUserAudioDriver* in_driver,
                           bool in_supports_prewarming,
                           OSString* in_device_uid,
                           OSString* in_model_uid,
                           OSString* in_manufacturer_uid,
                           uint32_t in_zero_timestamp_period) {
    if (!super::init(in_driver, in_supports_prewarming, in_device_uid,
                     in_model_uid, in_manufacturer_uid, in_zero_timestamp_period)) {
        ASFW_LOG(Audio, "ASFWAudioDevice::init - super::init failed");
        return false;
    }
    ivars = IONewZero(ASFWAudioDevice_IVars, 1);
    if (!ivars) {
        ASFW_LOG(Audio, "ASFWAudioDevice::init - failed to allocate ivars");
        return false;
    }
    ivars->configurationLock = IOLockAlloc();
    if (!ivars->configurationLock) {
        IOSafeDeleteNULL(ivars, ASFWAudioDevice_IVars, 1);
        return false;
    }
    return true;
}

void ASFWAudioDevice::free() {
    if (ivars) {
        ivars->driverIvars = nullptr;
        if (ivars->configurationLock) {
            IOLockFree(ivars->configurationLock);
            ivars->configurationLock = nullptr;
        }
        IOSafeDeleteNULL(ivars, ASFWAudioDevice_IVars, 1);
    }
    super::free();
}

void ASFWAudioDevice::SetDriverIvars(ASFWAudioDriver_IVars* ivars) {
    if (!this->ivars || !this->ivars->configurationLock) return;
    this->ivars->driverIvars = ivars;
    this->ivars->configurationEnabled = false;
    this->ivars->configurationMachine = {};
    if (!ivars || ivars->device.endpointId == 0 ||
        ivars->resolvedProfile.Value().configurationCapabilityCount == 0) {
        return;
    }

    const auto& profile = ivars->resolvedProfile.Value();
    const ASFW::Audio::Devices::ConfigurationCapabilityRecord* initial = nullptr;
    for (uint8_t i = 0; i < profile.configurationCapabilityCount; ++i) {
        const auto& candidate = profile.configurationCapabilities[i];
        if (candidate.configuration.sampleRate ==
                static_cast<uint32_t>(ivars->device.currentSampleRate) &&
            candidate.runtimeCaps.hostInputPcmChannels == ivars->device.inputChannelCount &&
            candidate.runtimeCaps.hostOutputPcmChannels == ivars->device.outputChannelCount) {
            initial = &candidate;
            break;
        }
    }
    if (!initial) {
        ASFW_LOG_ERROR(Audio,
                       "[AudioConfig] endpoint=%llu no initial capability for rate=%.0f in=%u out=%u",
                       ivars->device.endpointId, ivars->device.currentSampleRate,
                       ivars->device.inputChannelCount, ivars->device.outputChannelCount);
        return;
    }
    this->ivars->configurationMachine = {
        .state = ASFW::Configuration::Idle{
            .committed = {
                .endpointId = ivars->device.endpointId,
                .routeGeneration = ivars->device.deviceInstanceId,
                .revision = 1,
                .configuration = initial->configuration,
            },
        },
        .nextToken = 1,
    };
    this->ivars->configurationEnabled = true;
    ASFW_LOG(Audio,
             "[AudioConfig] coordinator ready endpoint=%llu rate=%u in=%u out=%u",
             ivars->device.endpointId, initial->configuration.sampleRate,
             initial->runtimeCaps.hostInputPcmChannels,
             initial->runtimeCaps.hostOutputPcmChannels);
}

kern_return_t ASFWAudioDevice::StartIO(IOUserAudioStartStopFlags in_flags) {
    if (!ivars || !ivars->driverIvars) {
        ASFW_LOG(Audio, "ASFWAudioDevice: StartIO failed - no driver ivars");
        return kIOReturnNotReady;
    }

    const uint64_t resumeToken = this->ivars->configurationResumeToken.load(
        std::memory_order_acquire);
    ASFW_LOG(DirectAudio, "ASFWAudioDevice: StartIO flags=0x%llx",
             static_cast<uint64_t>(in_flags));
    if (resumeToken != 0) {
        ASFW_LOG(Audio,
                 "[AudioConfig] host restart entered endpoint=%llu token=%llu",
                 this->ivars->driverIvars->device.endpointId, resumeToken);
    }

    auto& ivars = *this->ivars->driverIvars;
    __block kern_return_t kr = kIOReturnSuccess;

    ivars.workQueue->DispatchSync(^{
        bool streamingStarted = false;
        bool txResourcesAllocated = false;

        const auto releaseTxResources = [&]() noexcept {
            ivars.txPayloadMap = nullptr;
            ivars.txMetadataMap = nullptr;
            ivars.txControlMap = nullptr;
            ivars.txPayloadBuffer = nullptr;
            ivars.txMetadataBuffer = nullptr;
            ivars.txControlBuffer = nullptr;
            ivars.runtime.txSlotProvider.payloadBase = nullptr;
            ivars.runtime.txSlotProvider.metadataRing = nullptr;
            ivars.runtime.txSlotProvider.queueControl = nullptr;
            ivars.runtime.txSlotProvider.audioControl = nullptr;
            ivars.runtime.txSlotProvider.numSlots = 0;
            ivars.runtime.txExecutionTimeline.queueControl = nullptr;
            ivars.runtime.txStreamEngine.BindPcmSource(nullptr);

            // Secondary playback stream resources.
            ivars.txPayloadMapSecondary = nullptr;
            ivars.txMetadataMapSecondary = nullptr;
            ivars.txControlMapSecondary = nullptr;
            ivars.txPayloadBufferSecondary = nullptr;
            ivars.txMetadataBufferSecondary = nullptr;
            ivars.txControlBufferSecondary = nullptr;
            ivars.runtime.txSlotProviderSecondary.payloadBase = nullptr;
            ivars.runtime.txSlotProviderSecondary.metadataRing = nullptr;
            ivars.runtime.txSlotProviderSecondary.queueControl = nullptr;
            ivars.runtime.txSlotProviderSecondary.audioControl = nullptr;
            ivars.runtime.txSlotProviderSecondary.numSlots = 0;
            ivars.runtime.txStreamEngineSecondary.BindPcmSource(nullptr);
            ivars.runtime.txSecondaryActive = false;

            if (txResourcesAllocated && ivars.device.audioNub) {
                ivars.device.audioNub->FreeTxIsochResources();
            }
            txResourcesAllocated = false;
        };

        const auto failStart =
            [&](kern_return_t status, const char* stage) noexcept
                -> kern_return_t {
            const kern_return_t result =
                status == kIOReturnSuccess ? kIOReturnError : status;
            ivars.runtime.isRunning.store(false, std::memory_order_release);
            ivars.runtime.txActive.store(false, std::memory_order_release);
            if (ivars.txPreparationQueue) {
                ivars.txPreparationQueue->DispatchSync(^{ });
            }
            ivars.runtime.mAudioPresentationObserver.Disarm();
            ivars.runtime.mAudioInternalTxTiming.Disarm();
            if (streamingStarted && ivars.device.audioNub) {
                const kern_return_t stopKr =
                    ivars.device.audioNub->StopAudioStreaming();
                if (stopKr != kIOReturnSuccess) {
                    ASFW_LOG(
                        Audio,
                        "ASFWAudioDevice: StopAudioStreaming failed while unwinding %{public}s: 0x%x",
                        stage,
                        stopKr);
                }
            }
            releaseTxResources();
            ASFW_LOG(Audio,
                     "ASFWAudioDevice: StartIO failed at %{public}s: 0x%x",
                     stage,
                     result);
            return result;
        };

        // --- Reset IO state ---
        ivars.runtime.ioDebugCallbacks.store(0, std::memory_order_relaxed);
        ivars.runtime.ioCallbacksOutsideRun.store(0, std::memory_order_relaxed);
        ivars.runtime.isRunning.store(false, std::memory_order_release);
        ASFW_LOG(DirectAudio, "ADK DBG StartIO running=0 while arming transport");

        auto* control = ivars.runtime.directAudioGraph.control;
        if (!control) {
            ASFW_LOG(Audio, "ASFWAudioDevice: StartIO failed - no direct audio control");
            kr = failStart(kIOReturnNotReady, "ResolveDirectAudioControl");
            return;
        }
        control->ResetForStart();
        ivars.runtime.txPlanBusTicksValid = false;
        ivars.runtime.lastTxPlanBusTicks = 0;
        ivars.runtime.txObservationBusTicksValid = false;
        ivars.runtime.lastTxObservationBusTicks = 0;
        ivars.runtime.txNoCycleAnchorEvents = 0;
        ivars.runtime.txNoPresentationOriginEvents = 0;
        ivars.runtime.txReplayResyncs = 0;
        ivars.runtime.mAudioPresentationObserver.Disarm();
        ivars.runtime.mAudioInternalTxTiming.Disarm();
        ivars.runtime.lastHalZeroTimestampGeneration.store(0, std::memory_order_release);
        ivars.runtime.lastHalZeroTimestampSampleFrame.store(0, std::memory_order_release);
        ivars.runtime.lastHalZeroTimestampHostTicks.store(0, std::memory_order_release);

        // --- Allocate and map shared TX isoch resources ---
        uint32_t initialClockAnchorTimeoutMs = 500;
        bool useMAudioTxClock = false;
        {
            const auto* profile = &ivars.resolvedProfile;
            if (!profile->IsValid()) {
                ASFW_LOG(Audio, "ASFWAudioDevice: StartIO failed - resolved profile invalid");
                kr = failStart(kIOReturnError, "ResolveProfile");
                return;
            }
            initialClockAnchorTimeoutMs = profile->InitialClockAnchorTimeoutMs();
            useMAudioTxClock =
                ASFW::Audio::Families::BeBoB::MAudio::UsesSpecialDuplexPolicy(
                    profile->Value().profileBuilder);

            ASFW::Isoch::Audio::AudioStreamConfig txConfig{};
            if (!profile->BuildDefaultTxStreamConfig(txConfig)) {
                ASFW_LOG(Audio, "ASFWAudioDevice: StartIO failed - BuildDefaultTxStreamConfig failed");
                kr = failStart(kIOReturnError, "BuildDefaultTxStreamConfig");
                return;
            }
            const uint32_t numSlots =
                ASFW::Audio::Shared::AudioTimingGeometry::kTxSharedSlotPackets;
            const uint32_t maxPacketBytes =
                8u + static_cast<uint32_t>(txConfig.framesPerDataPacket) * txConfig.dbs * 4u;
            const uint32_t interruptInterval =
                ASFW::Audio::Shared::AudioTimingGeometry::kTimingGroupPackets;

            IOMemoryDescriptor* rawPayload = nullptr;
            IOMemoryDescriptor* rawMetadata = nullptr;
            IOMemoryDescriptor* rawControl = nullptr;

            kern_return_t allocKr = ivars.device.audioNub->AllocateTxIsochResources(
                0, numSlots, maxPacketBytes, interruptInterval,
                &rawPayload, &rawMetadata, &rawControl
            );
            if (allocKr != kIOReturnSuccess) {
                ASFW_LOG(Audio, "ASFWAudioDevice: AllocateTxIsochResources failed: 0x%x", allocKr);
                kr = failStart(allocKr, "AllocateTxIsochResources");
                return;
            }
            txResourcesAllocated = true;

            ivars.txPayloadBuffer = ASFW::Common::AdoptRetained(rawPayload);
            ivars.txMetadataBuffer = ASFW::Common::AdoptRetained(rawMetadata);
            ivars.txControlBuffer = ASFW::Common::AdoptRetained(rawControl);

            allocKr = ASFW::Common::CreateSharedMapping(ivars.txPayloadBuffer, ivars.txPayloadMap);
            if (allocKr != kIOReturnSuccess) {
                kr = failStart(allocKr, "MapTxPayload");
                return;
            }
            allocKr = ASFW::Common::CreateSharedMapping(ivars.txMetadataBuffer, ivars.txMetadataMap);
            if (allocKr != kIOReturnSuccess) {
                kr = failStart(allocKr, "MapTxMetadata");
                return;
            }
            allocKr = ASFW::Common::CreateSharedMapping(ivars.txControlBuffer, ivars.txControlMap);
            if (allocKr != kIOReturnSuccess) {
                kr = failStart(allocKr, "MapTxControl");
                return;
            }

            uint8_t* payloadBase = reinterpret_cast<uint8_t*>(ivars.txPayloadMap->GetAddress());
            auto* metadataRing = reinterpret_cast<ASFW::Isoch::IsochTxPacketMeta*>(ivars.txMetadataMap->GetAddress());
            auto* queueControl = reinterpret_cast<ASFW::Isoch::IsochTxQueueControl*>(ivars.txControlMap->GetAddress());

            // Clear stale runtime cursors before prefill: the shared slab can be
            // reused across StartIO/StopIO probes (CoreAudio re-probes on a
            // sample-rate change), and a carried-over committed cursor fails the IT
            // prime ("committed prefill > slots").
            queueControl->ResetProducerForStart();

            ivars.runtime.txSlotProvider.payloadBase = payloadBase;
            ivars.runtime.txSlotProvider.metadataRing = metadataRing;
            ivars.runtime.txSlotProvider.queueControl = queueControl;
            ivars.runtime.txSlotProvider.audioControl = control;
            ivars.runtime.txSlotProvider.numSlots = numSlots;
            ivars.runtime.txSlotProvider.slotStrideBytes = maxPacketBytes;

            ivars.runtime.txExecutionTimeline.queueControl = queueControl;

            if (!ivars.runtime.txStreamEngine.Configure(*profile, txConfig)) {
                ASFW_LOG(Audio, "ASFWAudioDevice: txStreamEngine Configure failed");
                kr = failStart(kIOReturnError, "ConfigureTxStreamEngine");
                return;
            }
            ASFW_LOG(Audio,
                     "[AudioConfig] packetizer prepared endpoint=%llu rate=%u pcm=%u dbs=%u fdf=0x%02x syt=%u bytes=%u",
                     ivars.device.endpointId, txConfig.sampleRate, txConfig.pcmChannels,
                     txConfig.dbs, txConfig.fdf, txConfig.framesPerDataPacket,
                     maxPacketBytes);
            ivars.runtime.pcmPublicationCache.BindTelemetry(
                &control->pcmPublicationTelemetry);
            if (!ivars.runtime.pcmPublicationCache.Configure(
                    ivars.runtime.directAudioGraph.memory.outputChannels,
                    ASFW::Audio::Shared::AudioTimingGeometry::
                        kPcmPublicationCacheFrames)) {
                ASFW_LOG(
                    Audio,
                    "ASFWAudioDevice: PCM publication cache allocation failed channels=%u frames=%u",
                    ivars.runtime.directAudioGraph.memory.outputChannels,
                    ASFW::Audio::Shared::AudioTimingGeometry::
                        kPcmPublicationCacheFrames);
                kr = failStart(kIOReturnNoMemory, "ConfigurePcmPublicationCache");
                return;
            }
            const auto timelineSource =
                (useMAudioTxClock || ivars.device.inputChannelCount == 0)
                    ? ASFW::Audio::Runtime::HardwareTimelineSource::Transmit
                    : ASFW::Audio::Runtime::HardwareTimelineSource::Receive;
            const uint64_t timelineEpoch = control->hardwareTimeline.BeginEpoch(
                timelineSource,
                ASFW::Audio::Runtime::HardwareTimelineDiscontinuity::StartIO,
                txConfig.sampleRate,
                0);
            if (timelineEpoch == 0) {
                kr = failStart(kIOReturnUnsupported, "BeginHardwareTimeline");
                return;
            }
            ASFW_LOG(
                DirectAudio,
                "[TimelineEpoch] epoch=%llu reason=start-io source=%u base=0 rate=%u",
                timelineEpoch, static_cast<uint32_t>(timelineSource),
                txConfig.sampleRate);
            ivars.runtime.pcmPublicationCache.BeginEpoch(timelineEpoch);
            ivars.runtime.txStreamEngine.BindSlotProvider(&ivars.runtime.txSlotProvider);
            ivars.runtime.txStreamEngine.BindPcmSource(
                &ivars.runtime.pcmPublicationCache);
            ivars.runtime.txStreamEngine.ResetForStart(0);
            ivars.runtime.txReplayReader.Reset();

            const uint32_t timingRateHz =
                ivars.device.currentSampleRate > 0
                    ? static_cast<uint32_t>(ivars.device.currentSampleRate)
                    : 48000u;
            control->rxTransferDelayTicks.store(
                profile->RxTransferDelayTicks(ivars.device.currentSampleRate),
                std::memory_order_relaxed);
            control->txTransferDelayTicks.store(
                profile->TxTransferDelayTicks(ivars.device.currentSampleRate),
                std::memory_order_relaxed);

            ASFW_LOG(Audio,
                     "ASFWAudioDevice: Allocated & configured TX isoch resources channel=%u rxTransferDelay=%u txTransferDelay=%u (rate=%u)",
                     txConfig.sid,
                     control->rxTransferDelayTicks.load(std::memory_order_relaxed),
                     control->txTransferDelayTicks.load(std::memory_order_relaxed),
                     timingRateHz);

        // --- Secondary playback stream (multi-stream DICE, e.g. Venice F32 = 2×16) ---
        // Allocate/map/configure the second host IT pipeline. It shadows the
        // master's per-packet timing in lockstep and encodes host output channels
        // [pcmChannels, 2×pcmChannels). The matching secondary IT hardware context
        // is created + wired to this slab by the duplex bringup
        // (PrepareTransmitStream), which runs after this allocation.
        if (profile->TxStreamCount() > 1) {
            ASFW::Isoch::Audio::AudioStreamConfig txConfig2{};
            if (!profile->BuildDefaultTxStreamConfig(txConfig2)) {
                kr = failStart(kIOReturnError, "BuildDefaultTxStreamConfig2");
                return;
            }
            txConfig2.sourceChannelOffset = txConfig2.pcmChannels;

            const uint32_t numSlots2 =
                ASFW::Audio::Shared::AudioTimingGeometry::kTxSharedSlotPackets;
            const uint32_t maxPacketBytes2 =
                8u + static_cast<uint32_t>(txConfig2.framesPerDataPacket) * txConfig2.dbs * 4u;
            const uint32_t interruptInterval2 =
                ASFW::Audio::Shared::AudioTimingGeometry::kTimingGroupPackets;

            IOMemoryDescriptor* rawPayload2 = nullptr;
            IOMemoryDescriptor* rawMetadata2 = nullptr;
            IOMemoryDescriptor* rawControl2 = nullptr;
            kern_return_t allocKr2 = ivars.device.audioNub->AllocateTxIsochResources(
                1, numSlots2, maxPacketBytes2, interruptInterval2,
                &rawPayload2, &rawMetadata2, &rawControl2);
            if (allocKr2 != kIOReturnSuccess) {
                kr = failStart(allocKr2, "AllocateTxIsochResources2");
                return;
            }
            ivars.txPayloadBufferSecondary = ASFW::Common::AdoptRetained(rawPayload2);
            ivars.txMetadataBufferSecondary = ASFW::Common::AdoptRetained(rawMetadata2);
            ivars.txControlBufferSecondary = ASFW::Common::AdoptRetained(rawControl2);

            allocKr2 = ASFW::Common::CreateSharedMapping(ivars.txPayloadBufferSecondary, ivars.txPayloadMapSecondary);
            if (allocKr2 != kIOReturnSuccess) { kr = failStart(allocKr2, "MapTxPayload2"); return; }
            allocKr2 = ASFW::Common::CreateSharedMapping(ivars.txMetadataBufferSecondary, ivars.txMetadataMapSecondary);
            if (allocKr2 != kIOReturnSuccess) { kr = failStart(allocKr2, "MapTxMetadata2"); return; }
            allocKr2 = ASFW::Common::CreateSharedMapping(ivars.txControlBufferSecondary, ivars.txControlMapSecondary);
            if (allocKr2 != kIOReturnSuccess) { kr = failStart(allocKr2, "MapTxControl2"); return; }

            uint8_t* payloadBase2 = reinterpret_cast<uint8_t*>(ivars.txPayloadMapSecondary->GetAddress());
            auto* metadataRing2 = reinterpret_cast<ASFW::Isoch::IsochTxPacketMeta*>(ivars.txMetadataMapSecondary->GetAddress());
            auto* queueControl2 = reinterpret_cast<ASFW::Isoch::IsochTxQueueControl*>(ivars.txControlMapSecondary->GetAddress());

            queueControl2->ResetProducerForStart();

            ivars.runtime.txSlotProviderSecondary.payloadBase = payloadBase2;
            ivars.runtime.txSlotProviderSecondary.metadataRing = metadataRing2;
            ivars.runtime.txSlotProviderSecondary.queueControl = queueControl2;
            ivars.runtime.txSlotProviderSecondary.audioControl = control;
            ivars.runtime.txSlotProviderSecondary.numSlots = numSlots2;
            ivars.runtime.txSlotProviderSecondary.slotStrideBytes = maxPacketBytes2;

            if (!ivars.runtime.txStreamEngineSecondary.Configure(*profile, txConfig2)) {
                kr = failStart(kIOReturnError, "ConfigureTxStreamEngine2");
                return;
            }
            ivars.runtime.txStreamEngineSecondary.BindSlotProvider(&ivars.runtime.txSlotProviderSecondary);
            ivars.runtime.txStreamEngineSecondary.BindPcmSource(
                &ivars.runtime.pcmPublicationCache);
            ivars.runtime.txStreamEngineSecondary.ResetForStart(0);
            ivars.runtime.txSecondaryActive = true;

            ASFW_LOG(Audio,
                     "ASFWAudioDevice: Allocated & configured SECONDARY TX stream offset=%u dbs=%u slots=%u slotSize=%u rate=%u",
                     txConfig2.sourceChannelOffset, txConfig2.dbs, numSlots2, maxPacketBytes2,
                     txConfig2.sampleRate);
        }
        }

        // --- Prefill TX ring ---
        ASFW::Audio::DriverKit::PrefillTxRingBeforeStart(ivars);

        auto* prefillControl = ivars.runtime.txSlotProvider.queueControl;
        const uint64_t prefillExpose =
            prefillControl
                ? prefillControl->committedEnd.load(std::memory_order_acquire)
                : 0;
        const uint32_t expectedPrefill =
            ivars.runtime.txSlotProvider.numSlots;
        if (!prefillControl || prefillExpose != expectedPrefill) {
            ASFW_LOG(
                Audio,
                "ASFWAudioDevice: StartIO failed - ValidateTxPrefill control=%u expose=%llu expected=%u",
                prefillControl != nullptr,
                prefillExpose,
                expectedPrefill);
            kr = failStart(kIOReturnNotReady, "ValidateTxPrefill");
            return;
        }

        // --- Start hardware streaming ---
        if (useMAudioTxClock) {
            const auto startEpoch =
                ASFW::Audio::Families::BeBoB::MAudio::StartEpoch{
                    .value = ++ivars.runtime.mAudioTxClockEpoch};
            const auto& txStreamConfig =
                ivars.runtime.txStreamEngine.StreamConfig();
            if (!ivars.runtime.mAudioPresentationObserver.Arm(
                    startEpoch,
                    control->hardwareTimeline.Epoch(),
                    txStreamConfig.sampleRate,
                    ASFW::Audio::Families::BeBoB::MAudio::
                        InternalTxTransferDelayTicks(
                            txStreamConfig.sampleRate,
                            txStreamConfig.framesPerDataPacket))) {
                ASFW_LOG(Audio,
                         "ASFWAudioDevice: StartIO failed - M-Audio presentation observer arm failed rate=%u sytInterval=%u",
                         txStreamConfig.sampleRate,
                         txStreamConfig.framesPerDataPacket);
                kr = failStart(kIOReturnNotReady, "ArmMAudioTxClock");
                return;
            }
            // The M-Audio adapter retains completion-cycle anchoring, while
            // the cadence itself is the shared rational IEC 61883-6 engine.
            // Arm it from the freshly projected rate/geometry rather than
            // retaining any previous-rate packet schedule.
            if (!ivars.runtime.mAudioInternalTxTiming.Arm(
                    startEpoch,
                    txStreamConfig.sampleRate,
                    txStreamConfig.framesPerDataPacket)) {
                ASFW_LOG(Audio,
                         "ASFWAudioDevice: StartIO failed - M-Audio TX timing rejected rate=%u frames=%u",
                         txStreamConfig.sampleRate,
                         txStreamConfig.framesPerDataPacket);
                kr = failStart(kIOReturnUnsupported,
                               "ArmMAudioInternalTxTiming");
                return;
            }
            ASFW_LOG(Audio,
                     "[AudioConfig] M-Audio TX cadence armed endpoint=%llu rate=%u syt=%u "
                     "delay=%u source=rational-blocking",
                     ivars.device.endpointId, txStreamConfig.sampleRate,
                     txStreamConfig.framesPerDataPacket,
                     ivars.runtime.mAudioInternalTxTiming.TransferDelayTicks());
            // Arm the one-shot SYT seed trace for this stream. It prints the
            // seed and the next few increments once the transmit anchor lands,
            // then goes quiet; see AudioDriverRuntimeState.
            ivars.runtime.sytSeedTraceRemaining =
                ivars.runtime.kSytSeedTracePackets;
            ivars.runtime.sytSeedTraceHavePrev = false;
        }
        ivars.runtime.txActive.store(true, std::memory_order_release);
        // A failed start can still leave a partially-started backend. Mark the
        // attempt before the call so every non-success result is unwound with
        // StopAudioStreaming rather than trusting failure to be side-effect free.
        streamingStarted = true;
        const kern_return_t startKr =
            ivars.device.audioNub->StartAudioStreaming();
        if (startKr != kIOReturnSuccess) {
            ASFW_LOG(Audio,
                     "ASFWAudioDevice: StartAudioStreaming failed: 0x%x",
                     startKr);
            kr = failStart(startKr, "StartAudioStreaming");
            return;
        }

        // StartAudioStreaming initializes the shared transport control block.
        // Validate it immediately afterward; failStart stops the partially
        // started stream before returning any mismatch to AudioDriverKit.
        auto* txControl = ivars.runtime.txSlotProvider.queueControl;
        if (!txControl ||
            txControl->abiVersion != ASFW::Isoch::kTxQueueAbiVersion ||
            txControl->numSlots != ASFW::Audio::Shared::AudioTimingGeometry::kTxSharedSlotPackets ||
            txControl->slotStrideBytes != ivars.runtime.txSlotProvider.slotStrideBytes ||
            txControl->maxPacketBytes != ivars.runtime.txSlotProvider.slotStrideBytes ||
            txControl->interruptInterval != ASFW::Audio::Shared::AudioTimingGeometry::kTxPacketsPerGroup) {
            ASFW_LOG(Audio,
                     "ASFWAudioDevice: TX queue ABI/geometry mismatch abi=%u slots=%u stride=%u max=%u group=%u",
                     txControl ? txControl->abiVersion : 0,
                     txControl ? txControl->numSlots : 0,
                     txControl ? txControl->slotStrideBytes : 0,
                     txControl ? txControl->maxPacketBytes : 0,
                     txControl ? txControl->interruptInterval : 0);
            kr = failStart(
                kIOReturnUnsupported, "ValidateTxTransportGeometry");
            return;
        }

        // AudioDriverKit needs a valid clock anchor when StartIO transitions
        // the device into the running state. The profile-owned hardware anchor
        // publisher runs on an independent RX or TX preparation queue, so it
        // can publish while this work queue waits. Generic BeBoB devices can
        // spend ~1 s in CIP NO-DATA after input starts (Linux
        // bebob_stream.c:661-666); the M-Audio path instead seeds from TX.
        uint32_t ztsWaitMs = 0;
        while (ivars.runtime.lastHalZeroTimestampHostTicks.load(
                   std::memory_order_acquire) == 0 &&
               ztsWaitMs < initialClockAnchorTimeoutMs) {
            IOSleep(1);
            ++ztsWaitMs;
        }
        const uint64_t initialZtsHostTicks =
            ivars.runtime.lastHalZeroTimestampHostTicks.load(
                std::memory_order_acquire);
        if (initialZtsHostTicks == 0) {
            ASFW_LOG(
                Audio,
                "ASFWAudioDevice: initial hardware ZTS timed out after %u ms",
                ztsWaitMs);
            kr = failStart(kIOReturnTimeout, "WaitForInitialHardwareZts");
            return;
        }
        ASFW_LOG(
            DirectAudio,
            "ADK DBG StartIO initial hardware ZTS sampleFrame=%llu hostTicks=%llu waitMs=%u",
            ivars.runtime.lastHalZeroTimestampSampleFrame.load(
                std::memory_order_acquire),
            initialZtsHostTicks,
            ztsWaitMs);

        // Hardware-specific setup must finish before super::StartIO updates
        // ADK's IO state. Open the RT gate first so callbacks arriving as part
        // of that transition never observe a half-started transport.
        ivars.runtime.isRunning.store(true, std::memory_order_release);
        kr = super::StartIO(in_flags);
        if (kr != kIOReturnSuccess) {
            kr = failStart(kr, "super::StartIO");
            return;
        }
        ASFW_LOG(
            DirectAudio,
            "ADK DBG StartIO super::StartIO ok callbacks=%llu outsideRun=%llu",
            ivars.runtime.ioDebugCallbacks.load(std::memory_order_relaxed),
            ivars.runtime.ioCallbacksOutsideRun.load(std::memory_order_relaxed));

        ASFW_LOG(DirectAudio,
                 "ADK DBG StartIO transport and hardware clock ready");
        ASFW_LOG(DirectAudio,
                 "ADK DBG DUPLEX ready endpoint=%llu rxStarted=1 txStarted=1 bindValid=%d hasIn=%d hasOut=%d audioDevice=%p",
                 ivars.device.endpointId,
                 ivars.runtime.directAudioSkeletonBound.load(std::memory_order_acquire),
                 ivars.runtime.directAudioGraph.HasInput(),
                 ivars.runtime.directAudioGraph.HasOutput(),
                 static_cast<void*>(ivars.runtime.directAudioGraph.audioDevice));
    });

    if (kr == kIOReturnSuccess) {
        if (resumeToken != 0) {
            uint64_t expected = resumeToken;
            (void)this->ivars->configurationResumeToken.compare_exchange_strong(
                expected, 0, std::memory_order_acq_rel);
            ASFW_LOG(Audio,
                     "[AudioConfig] host restart complete endpoint=%llu token=%llu",
                     ivars.device.endpointId, resumeToken);
        }
        const auto inputFormat = ivars.inputStream
            ? ivars.inputStream->GetCurrentStreamFormat()
            : IOUserAudioStreamBasicDescription{};
        const auto outputFormat = ivars.outputStream
            ? ivars.outputStream->GetCurrentStreamFormat()
            : IOUserAudioStreamBasicDescription{};
        const bool inputActive =
            ivars.inputStream && ivars.inputStream->GetStreamIsActive();
        const bool outputActive =
            ivars.outputStream && ivars.outputStream->GetStreamIsActive();
        const size_t inputFormatCount = ivars.inputStream
            ? ivars.inputStream->GetNumberAvailableStreamFormats()
            : 0;
        const size_t outputFormatCount = ivars.outputStream
            ? ivars.outputStream->GetNumberAvailableStreamFormats()
            : 0;

        uint64_t inputClientSample = 0;
        uint64_t inputClientHost = 0;
        uint64_t outputClientSample = 0;
        uint64_t outputClientHost = 0;
        GetCurrentClientIOTime(
            true, &inputClientSample, &inputClientHost);
        GetCurrentClientIOTime(
            false, &outputClientSample, &outputClientHost);

        uint64_t ztsSample = 0;
        uint64_t ztsHost = 0;
        GetCurrentZeroTimestamp(&ztsSample, &ztsHost);

        ASFW_LOG(
            DirectAudio,
            "ADK STATE after StartIO geometry ztsPeriod=%u inputRingFrames=%u outputRingFrames=%u maxIoFrames=%u",
            GetZeroTimestampPeriod(),
            ivars.runtime.directAudioGraph.memory.inputFrameCapacity,
            ivars.runtime.directAudioGraph.memory.outputFrameCapacity,
            ASFW::Audio::Shared::AudioTimingGeometry::kHalIoPeriodFrames);
        ASFW_LOG(
            DirectAudio,
            "ADK STATE after StartIO streams input(active=%d formats=%llu rate=%.0f flags=0x%x bytesFrame=%u channels=%u bits=%u) output(active=%d formats=%llu rate=%.0f flags=0x%x bytesFrame=%u channels=%u bits=%u)",
            inputActive,
            static_cast<uint64_t>(inputFormatCount),
            inputFormat.mSampleRate,
            static_cast<uint32_t>(inputFormat.mFormatFlags),
            inputFormat.mBytesPerFrame,
            inputFormat.mChannelsPerFrame,
            inputFormat.mBitsPerChannel,
            outputActive,
            static_cast<uint64_t>(outputFormatCount),
            outputFormat.mSampleRate,
            static_cast<uint32_t>(outputFormat.mFormatFlags),
            outputFormat.mBytesPerFrame,
            outputFormat.mChannelsPerFrame,
            outputFormat.mBitsPerChannel);
        ASFW_LOG(
            DirectAudio,
            "ADK STATE after StartIO timing input(sample=%llu host=%llu) output(sample=%llu host=%llu) zts(sample=%llu host=%llu)",
            inputClientSample,
            inputClientHost,
            outputClientSample,
            outputClientHost,
            ztsSample,
            ztsHost);
    }

    return kr;
}

kern_return_t ASFWAudioDevice::StopIO(IOUserAudioStartStopFlags in_flags) {
    if (!ivars || !ivars->driverIvars) {
        ASFW_LOG(Audio, "ASFWAudioDevice: StopIO failed - no driver ivars");
        return kIOReturnNotReady;
    }

    const uint64_t inFlightToken = this->ivars->configurationInFlightToken.load(
        std::memory_order_acquire);
    ASFW_LOG(DirectAudio, "ASFWAudioDevice: StopIO flags=0x%llx",
             static_cast<uint64_t>(in_flags));
    if (inFlightToken != 0) {
        ASFW_LOG(Audio,
                 "[AudioConfig] host quiesce entered endpoint=%llu token=%llu",
                 this->ivars->driverIvars->device.endpointId, inFlightToken);
    }

    auto& ivars = *this->ivars->driverIvars;
    __block kern_return_t kr = kIOReturnSuccess;

    ivars.workQueue->DispatchSync(^{
        ivars.runtime.isRunning.store(false, std::memory_order_release);
        ivars.runtime.txActive.store(false, std::memory_order_release);

        // TxPreparation owns packetizer cursors and dereferences both the
        // staging source and shared TX mappings. Drain an action that passed
        // its txActive gate before releasing either side of that seam.
        if (ivars.txPreparationQueue) {
            ivars.txPreparationQueue->DispatchSync(^{ });
        }
        ivars.runtime.mAudioPresentationObserver.Disarm();
        ivars.runtime.mAudioInternalTxTiming.Disarm();

        if (ivars.runtime.directAudioGraph.control) {
            const auto* control = ivars.runtime.directAudioGraph.control;
            ASFW_LOG(DirectAudio,
                     "ADK DBG STOPIO endpoint=%llu callbacks=%llu outsideRun=%llu zts=%llu rxZts=%llu rxAdk=%llu beginRead=%llu writeEnd=%llu writtenEndFrame=%llu txPackets=%llu txSilence=%llu txUnderruns=%llu",
                     ivars.device.endpointId,
                     ivars.runtime.ioDebugCallbacks.load(std::memory_order_relaxed),
                     ivars.runtime.ioCallbacksOutsideRun.load(std::memory_order_relaxed),
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

        if (ivars.device.audioNub) {
            const kern_return_t stopKr = ivars.device.audioNub->StopAudioStreaming();
            if (stopKr != kIOReturnSuccess) {
                ASFW_LOG(Audio, "ASFWAudioDevice: StopAudioStreaming failed: 0x%x", stopKr);
            }
        }

        ivars.txPayloadMap = nullptr;
        ivars.txMetadataMap = nullptr;
        ivars.txControlMap = nullptr;
        ivars.txPayloadBuffer = nullptr;
        ivars.txMetadataBuffer = nullptr;
        ivars.txControlBuffer = nullptr;
        ivars.runtime.txSlotProvider.payloadBase = nullptr;
        ivars.runtime.txSlotProvider.metadataRing = nullptr;
        ivars.runtime.txSlotProvider.queueControl = nullptr;
        ivars.runtime.txSlotProvider.audioControl = nullptr;
        ivars.runtime.txSlotProvider.numSlots = 0;
        ivars.runtime.txExecutionTimeline.queueControl = nullptr;
        ivars.runtime.txStreamEngine.BindPcmSource(nullptr);

        // Secondary playback stream teardown. Drop txSecondaryActive first so the
        // RT pump/IO paths stop touching the secondary engine before its mapped
        // slab is released.
        ivars.runtime.txSecondaryActive = false;
        ivars.txPayloadMapSecondary = nullptr;
        ivars.txMetadataMapSecondary = nullptr;
        ivars.txControlMapSecondary = nullptr;
        ivars.txPayloadBufferSecondary = nullptr;
        ivars.txMetadataBufferSecondary = nullptr;
        ivars.txControlBufferSecondary = nullptr;
        ivars.runtime.txSlotProviderSecondary.payloadBase = nullptr;
        ivars.runtime.txSlotProviderSecondary.metadataRing = nullptr;
        ivars.runtime.txSlotProviderSecondary.queueControl = nullptr;
        ivars.runtime.txSlotProviderSecondary.audioControl = nullptr;
        ivars.runtime.txSlotProviderSecondary.numSlots = 0;
        ivars.runtime.txStreamEngineSecondary.BindPcmSource(nullptr);
        ivars.runtime.pcmPublicationCache.BeginEpoch(0);

        if (ivars.device.audioNub) {
            ivars.device.audioNub->FreeTxIsochResources();
        }

        kr = super::StopIO(in_flags);
        if (inFlightToken != 0) {
            ASFW_LOG(Audio,
                     "[AudioConfig] host quiesce complete endpoint=%llu token=%llu kr=0x%x",
                     ivars.device.endpointId, inFlightToken, kr);
        }
    });

    return kr;
}

namespace {

[[nodiscard]] kern_return_t StateMachineErrorToIOReturn(
    ASFW::Configuration::StateMachineError error) noexcept {
    return error == ASFW::Configuration::StateMachineError::Busy
        ? kIOReturnBusy : kIOReturnBadArgument;
}

[[nodiscard]] uint32_t OpticalModeWire(
    const std::optional<ASFW::Configuration::OpticalMode>& mode) noexcept {
    if (!mode) return 0;
    return *mode == ASFW::Configuration::OpticalMode::Adat ? 1U : 2U;
}

[[nodiscard]] std::optional<ASFW::Configuration::OpticalMode>
OpticalModeFromWire(uint32_t raw) noexcept {
    switch (raw) {
    case 1: return ASFW::Configuration::OpticalMode::Adat;
    case 2: return ASFW::Configuration::OpticalMode::Spdif;
    default: return std::nullopt;
    }
}

[[nodiscard]] kern_return_t ApplyPreferredChannelLayouts(
    ASFWAudioDevice& device, uint32_t inputChannels, uint32_t outputChannels) noexcept {
    if (inputChannels == 0 || outputChannels == 0 ||
        inputChannels > ASFW::Encoding::kMaxPcmChannels ||
        outputChannels > ASFW::Encoding::kMaxPcmChannels) {
        return kIOReturnBadArgument;
    }
    std::array<IOUserAudioChannelLabel, ASFW::Encoding::kMaxPcmChannels> input{};
    std::array<IOUserAudioChannelLabel, ASFW::Encoding::kMaxPcmChannels> output{};
    for (uint32_t channel = 0; channel < inputChannels; ++channel) {
        input[channel] = static_cast<IOUserAudioChannelLabel>(
            static_cast<uint32_t>(IOUserAudioChannelLabel::Discrete_0) + channel);
    }
    for (uint32_t channel = 0; channel < outputChannels; ++channel) {
        output[channel] = static_cast<IOUserAudioChannelLabel>(
            static_cast<uint32_t>(IOUserAudioChannelLabel::Discrete_0) + channel);
    }
    kern_return_t kr = device.SetPreferredOutputChannelLayout(output.data(), outputChannels);
    if (kr == kIOReturnSuccess) {
        kr = device.SetPreferredInputChannelLayout(input.data(), inputChannels);
    }
    return kr;
}

[[nodiscard]] kern_return_t ApplyADKConfigurationProjection(
    ASFWAudioDevice& device, ASFWAudioDriver_IVars& driverIvars,
    const ASFW::Configuration::DeviceConfiguration& configuration,
    uint32_t inputChannels, uint32_t outputChannels) noexcept {
    const auto* capability = driverIvars.resolvedProfile.Value().ConfigurationFor(configuration);
    if (!capability || inputChannels != capability->runtimeCaps.hostInputPcmChannels ||
        outputChannels != capability->runtimeCaps.hostOutputPcmChannels ||
        capability->runtimeCaps.sampleRateHz != configuration.sampleRate ||
        !ASFW::Audio::Shared::AudioTimingGeometry::IsV3SampleRate(
            configuration.sampleRate) ||
        !ASFW::Encoding::AmdtpRateGeometryForSampleRate(configuration.sampleRate).has_value() ||
        driverIvars.runtime.isRunning.load(std::memory_order_acquire)) {
        return kIOReturnBadArgument;
    }

    const double targetRate = static_cast<double>(configuration.sampleRate);
    const uint32_t priorRate = static_cast<uint32_t>(device.GetSampleRate());
    kern_return_t kr = device.SetSampleRate(targetRate);
    if (kr != kIOReturnSuccess) return kr;
    if (priorRate != configuration.sampleRate) {
        if (driverIvars.outputStream &&
            (kr = driverIvars.outputStream->DeviceSampleRateChanged(targetRate)) != kIOReturnSuccess) {
            return kr;
        }
        if (driverIvars.inputStream &&
            (kr = driverIvars.inputStream->DeviceSampleRateChanged(targetRate)) != kIOReturnSuccess) {
            return kr;
        }
    }
    if ((kr = ApplyPreferredChannelLayouts(device, inputChannels, outputChannels)) != kIOReturnSuccess) {
        return kr;
    }

    std::array<IOUserAudioStreamBasicDescription,
               ASFW::Audio::Devices::kMaxConfigurationCapabilities> inputFormats{};
    std::array<IOUserAudioStreamBasicDescription,
               ASFW::Audio::Devices::kMaxConfigurationCapabilities> outputFormats{};
    uint32_t formatCount = 0;
    const auto& profile = driverIvars.resolvedProfile.Value();
    for (uint8_t i = 0; i < profile.configurationCapabilityCount; ++i) {
        const auto& candidate = profile.configurationCapabilities[i];
        if (candidate.configuration.opticalInput != configuration.opticalInput ||
            candidate.configuration.opticalOutput != configuration.opticalOutput ||
            candidate.runtimeCaps.hostInputPcmChannels != inputChannels ||
            candidate.runtimeCaps.hostOutputPcmChannels != outputChannels ||
            !ASFW::Audio::Shared::AudioTimingGeometry::IsV3SampleRate(
                candidate.configuration.sampleRate) ||
            formatCount == inputFormats.size()) {
            continue;
        }
        ASFW::Audio::DriverKit::FillFloat32Format(
            inputFormats[formatCount], candidate.configuration.sampleRate, inputChannels);
        ASFW::Audio::DriverKit::FillFloat32Format(
            outputFormats[formatCount], candidate.configuration.sampleRate, outputChannels);
        ++formatCount;
    }
    if (formatCount == 0) return kIOReturnBadArgument;
    IOUserAudioStreamBasicDescription inputCurrent{};
    IOUserAudioStreamBasicDescription outputCurrent{};
    ASFW::Audio::DriverKit::FillFloat32Format(inputCurrent, targetRate, inputChannels);
    ASFW::Audio::DriverKit::FillFloat32Format(outputCurrent, targetRate, outputChannels);
    if (driverIvars.outputStream &&
        ((kr = driverIvars.outputStream->SetAvailableStreamFormats(
              outputFormats.data(), formatCount)) != kIOReturnSuccess ||
         (kr = driverIvars.outputStream->SetCurrentStreamFormat(&outputCurrent)) != kIOReturnSuccess)) {
        return kr;
    }
    if (driverIvars.inputStream &&
        ((kr = driverIvars.inputStream->SetAvailableStreamFormats(
              inputFormats.data(), formatCount)) != kIOReturnSuccess ||
         (kr = driverIvars.inputStream->SetCurrentStreamFormat(&inputCurrent)) != kIOReturnSuccess)) {
        return kr;
    }

    driverIvars.device.currentSampleRate = targetRate;
    driverIvars.device.inputChannelCount = inputChannels;
    driverIvars.device.outputChannelCount = outputChannels;
    driverIvars.device.channelCount = std::max(inputChannels, outputChannels);
    if (!driverIvars.resolvedProfile.ApplyRuntimeConfiguration(capability->runtimeCaps)) {
        return kIOReturnBadArgument;
    }
    const auto packetGeometry = ASFW::Encoding::AmdtpRateGeometryForSampleRate(
        configuration.sampleRate);
    ASFW_LOG(Audio,
             "[AudioConfig] stream profile projected endpoint=%llu rate=%u rx=%u/%u tx=%u/%u fdf=0x%02x syt=%u",
             driverIvars.device.endpointId, configuration.sampleRate,
             capability->runtimeCaps.deviceToHostStreams[0].pcmChannels,
             capability->runtimeCaps.deviceToHostStreams[0].am824Slots,
             capability->runtimeCaps.hostToDeviceStreams[0].pcmChannels,
             capability->runtimeCaps.hostToDeviceStreams[0].am824Slots,
             packetGeometry->fdf, packetGeometry->sytIntervalFrames);
    if (!ASFW::Audio::DriverKit::UpdateDirectAudioGeometry(
            driverIvars,
            {.inputFrames = driverIvars.runtime.directAudioGraph.memory.inputFrameCapacity,
             .outputFrames = driverIvars.runtime.directAudioGraph.memory.outputFrameCapacity,
             .inputChannels = inputChannels,
             .outputChannels = outputChannels})) {
        return kIOReturnError;
    }
    return kIOReturnSuccess;
}

} // namespace

kern_return_t ASFWAudioDevice::HandleChangeSampleRate(double in_sample_rate) {
    if (!ivars || !ivars->driverIvars || !ivars->configurationLock ||
        !ivars->configurationEnabled) {
        return kIOReturnUnsupported;
    }
    auto& driverIvars = *ivars->driverIvars;
    if (driverIvars.runtime.isRunning.load(std::memory_order_acquire)) {
        ASFW_LOG(Audio, "[AudioConfig] CoreAudio rate %.0f rejected while IO is active",
                 in_sample_rate);
        return kIOReturnBusy;
    }
    if (!driverIvars.device.audioNub || in_sample_rate <= 0.0 ||
        static_cast<double>(static_cast<uint32_t>(in_sample_rate)) != in_sample_rate) {
        return kIOReturnBadArgument;
    }

    const uint32_t rateHz = static_cast<uint32_t>(in_sample_rate);
    ASFW::Configuration::TransitionResult transition{};
    auto dispatch = [&](const ASFW::Configuration::ConfigurationEvent& event)
        -> kern_return_t {
        IOLockLock(ivars->configurationLock);
        const auto result = ASFW::Configuration::Reduce(ivars->configurationMachine, event);
        if (!result) {
            IOLockUnlock(ivars->configurationLock);
            return StateMachineErrorToIOReturn(result.error());
        }
        ivars->configurationMachine = result->next;
        transition = *result;
        IOLockUnlock(ivars->configurationLock);
        return kIOReturnSuccess;
    };

    kern_return_t kr = dispatch(ASFW::Configuration::CoreAudioRateIntent{
        .endpointId = driverIvars.device.endpointId,
        .routeGeneration = driverIvars.device.deviceInstanceId,
        .sampleRate = rateHz,
    });
    if (kr != kIOReturnSuccess ||
        transition.disposition == ASFW::Configuration::TransitionDisposition::NoOp) {
        return kr;
    }
    if (transition.effects.size() != 1 ||
        !std::holds_alternative<ASFW::Configuration::ResolveCandidateEffect>(transition.effects[0])) {
        return kIOReturnError;
    }
    const auto resolve = std::get<ASFW::Configuration::ResolveCandidateEffect>(transition.effects[0]);
    if (!driverIvars.resolvedProfile.Value().ConfigurationFor(resolve.requested)) {
        (void)dispatch(ASFW::Configuration::CandidateRejected{.identity = resolve.identity});
        return kIOReturnUnsupported;
    }
    if ((kr = dispatch(ASFW::Configuration::CandidateAccepted{
             .identity = resolve.identity, .candidate = resolve.requested})) != kIOReturnSuccess ||
        transition.effects.size() != 1 ||
        !std::holds_alternative<ASFW::Configuration::ApplyHardwareEffect>(transition.effects[0])) {
        return kr == kIOReturnSuccess ? kIOReturnError : kr;
    }
    const auto apply = std::get<ASFW::Configuration::ApplyHardwareEffect>(transition.effects[0]);
    uint32_t inputChannels = 0;
    uint32_t outputChannels = 0;
    kr = driverIvars.device.audioNub->ApplyDeviceConfiguration(
        apply.transition.candidate.sampleRate,
        OpticalModeWire(apply.transition.candidate.opticalInput),
        OpticalModeWire(apply.transition.candidate.opticalOutput),
        &inputChannels, &outputChannels);
    if (kr != kIOReturnSuccess) {
        (void)dispatch(ASFW::Configuration::HardwareCompleted{
            .identity = apply.transition.identity,
            // The 1814 vendor selector is write-only and may have been
            // accepted before a later signal-format operation failed. Treat a
            // failed apply as unknown, never as "unchanged", so the reducer
            // cannot publish the prior geometry as truth.
            .outcome = ASFW::Configuration::HardwareUnknown{},
        });
        ASFW_LOG_ERROR(Audio,
                       "[AudioConfig] hardware state uncertain endpoint=%llu token=%llu kr=0x%x",
                       driverIvars.device.endpointId, apply.transition.identity.token, kr);
        return kr;
    }
    if ((kr = dispatch(ASFW::Configuration::HardwareCompleted{
             .identity = apply.transition.identity,
             .outcome = ASFW::Configuration::HardwareConfirmedRequested{
                 .confirmed = {.configuration = apply.transition.candidate},
             },
         })) != kIOReturnSuccess || transition.effects.size() != 1 ||
        !std::holds_alternative<ASFW::Configuration::ProjectADKEffect>(transition.effects[0])) {
        return kr == kIOReturnSuccess ? kIOReturnError : kr;
    }
    const auto project = std::get<ASFW::Configuration::ProjectADKEffect>(transition.effects[0]);
    const kern_return_t mutation = ApplyADKConfigurationProjection(
        *this, driverIvars, project.plan.confirmed.configuration,
        inputChannels, outputChannels);
    const kern_return_t committed = mutation == kIOReturnSuccess
        ? driverIvars.device.audioNub->CommitDeviceConfiguration(
              project.plan.confirmed.configuration.sampleRate,
              OpticalModeWire(project.plan.confirmed.configuration.opticalInput),
              OpticalModeWire(project.plan.confirmed.configuration.opticalOutput))
        : mutation;
    const kern_return_t finished = dispatch(ASFW::Configuration::ProjectionFinished{
        .identity = project.plan.identity,
        .customProjectionSucceeded = mutation == kIOReturnSuccess && committed == kIOReturnSuccess,
        // HandleChangeSampleRate is the ADK callback. There is no separate
        // superclass projection step in this callback path.
        .superclassSucceeded = true,
    });
    const kern_return_t result = mutation != kIOReturnSuccess ? mutation
        : committed != kIOReturnSuccess ? committed : finished;
    ASFW_LOG(Audio,
             "[AudioConfig] CoreAudio rate result endpoint=%llu token=%llu rate=%u in=%u out=%u kr=0x%x",
             driverIvars.device.endpointId, project.plan.identity.token,
             project.plan.confirmed.configuration.sampleRate, inputChannels,
             outputChannels, result);
    return result;
}

kern_return_t ASFWAudioDevice::RequestControlConfiguration(
    uint32_t sampleRateHz, uint32_t opticalInput, uint32_t opticalOutput) {
    if (!ivars || !ivars->driverIvars || !ivars->configurationLock ||
        !ivars->configurationEnabled) {
        return kIOReturnUnsupported;
    }
    auto& driverIvars = *ivars->driverIvars;
    if (opticalInput > 2U || opticalOutput > 2U || sampleRateHz == 0 ||
        !driverIvars.device.audioNub) {
        return kIOReturnBadArgument;
    }
    const ASFW::Configuration::DeviceConfiguration requested{
        .sampleRate = sampleRateHz,
        .opticalInput = OpticalModeFromWire(opticalInput),
        .opticalOutput = OpticalModeFromWire(opticalOutput),
    };
    if (!driverIvars.resolvedProfile.Value().ConfigurationFor(requested)) {
        return kIOReturnUnsupported;
    }

    ASFW::Configuration::TransitionResult transition{};
    auto dispatch = [&](const ASFW::Configuration::ConfigurationEvent& event)
        -> kern_return_t {
        IOLockLock(ivars->configurationLock);
        const auto result = ASFW::Configuration::Reduce(ivars->configurationMachine, event);
        if (!result) {
            IOLockUnlock(ivars->configurationLock);
            return StateMachineErrorToIOReturn(result.error());
        }
        ivars->configurationMachine = result->next;
        transition = *result;
        IOLockUnlock(ivars->configurationLock);
        return kIOReturnSuccess;
    };
    kern_return_t kr = dispatch(ASFW::Configuration::ControlIntent{
        .endpointId = driverIvars.device.endpointId,
        .routeGeneration = driverIvars.device.deviceInstanceId,
        .requested = requested,
    });
    if (kr != kIOReturnSuccess ||
        transition.disposition == ASFW::Configuration::TransitionDisposition::NoOp) {
        return kr;
    }
    if (transition.effects.size() != 1 ||
        !std::holds_alternative<ASFW::Configuration::ResolveCandidateEffect>(transition.effects[0])) {
        return kIOReturnError;
    }
    const auto resolve = std::get<ASFW::Configuration::ResolveCandidateEffect>(transition.effects[0]);
    if ((kr = dispatch(ASFW::Configuration::CandidateAccepted{
             .identity = resolve.identity, .candidate = requested})) != kIOReturnSuccess ||
        transition.effects.size() != 1 ||
        !std::holds_alternative<ASFW::Configuration::RequestADKWindowEffect>(transition.effects[0])) {
        return kr == kIOReturnSuccess ? kIOReturnError : kr;
    }
    const auto window = std::get<ASFW::Configuration::RequestADKWindowEffect>(transition.effects[0]);
    ASFW_LOG(Audio,
             "[AudioConfig] requesting ADK window endpoint=%llu token=%llu rate=%u opticalIn=%u opticalOut=%u",
             driverIvars.device.endpointId, window.identity.token, sampleRateHz,
             opticalInput, opticalOutput);
    ivars->configurationInFlightToken.store(
        window.identity.token, std::memory_order_release);
    kr = RequestDeviceConfigurationChange(window.identity.token, nullptr);
    if (kr != kIOReturnSuccess) {
        (void)dispatch(ASFW::Configuration::ADKWindowRejected{.identity = window.identity});
        uint64_t expected = window.identity.token;
        (void)ivars->configurationInFlightToken.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel);
    }
    ASFW_LOG(Audio,
             "[AudioConfig] ADK window request result endpoint=%llu token=%llu kr=0x%x",
             driverIvars.device.endpointId, window.identity.token, kr);
    return kr;
}

kern_return_t ASFWAudioDevice::PerformDeviceConfigurationChange(
    uint64_t change_action, OSObject* in_change_info) {
    if (!ivars || !ivars->driverIvars || !ivars->configurationLock ||
        !ivars->configurationEnabled) {
        return super::PerformDeviceConfigurationChange(change_action, in_change_info);
    }
    auto& driverIvars = *ivars->driverIvars;
    ASFW::Configuration::ConfigurationIdentity identity{};
    IOLockLock(ivars->configurationLock);
    if (const auto* pending = std::get_if<ASFW::Configuration::AwaitingADKPerform>(
            &ivars->configurationMachine.state);
        pending && pending->transition.identity.token == change_action) {
        identity = pending->transition.identity;
    }
    IOLockUnlock(ivars->configurationLock);
    if (identity.token == 0) {
        return super::PerformDeviceConfigurationChange(change_action, in_change_info);
    }

    ASFW_LOG(Audio,
             "[AudioConfig] host Perform granted endpoint=%llu token=%llu running=%d",
             driverIvars.device.endpointId, identity.token,
             driverIvars.runtime.isRunning.load(std::memory_order_acquire));

    ASFW::Configuration::TransitionResult transition{};
    auto dispatch = [&](const ASFW::Configuration::ConfigurationEvent& event)
        -> kern_return_t {
        IOLockLock(ivars->configurationLock);
        const auto result = ASFW::Configuration::Reduce(ivars->configurationMachine, event);
        if (!result) {
            IOLockUnlock(ivars->configurationLock);
            return StateMachineErrorToIOReturn(result.error());
        }
        ivars->configurationMachine = result->next;
        transition = *result;
        IOLockUnlock(ivars->configurationLock);
        return kIOReturnSuccess;
    };
    kern_return_t kr = dispatch(ASFW::Configuration::ADKPerformGranted{.identity = identity});
    if (kr != kIOReturnSuccess || transition.effects.size() != 1 ||
        !std::holds_alternative<ASFW::Configuration::ApplyHardwareEffect>(transition.effects[0])) {
        const kern_return_t superKr = super::PerformDeviceConfigurationChange(change_action, in_change_info);
        uint64_t expected = identity.token;
        (void)ivars->configurationInFlightToken.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel);
        return kr != kIOReturnSuccess ? kr : superKr;
    }
    const auto apply = std::get<ASFW::Configuration::ApplyHardwareEffect>(transition.effects[0]);
    uint32_t inputChannels = 0;
    uint32_t outputChannels = 0;
    const kern_return_t hardwareKr = driverIvars.device.audioNub->ApplyDeviceConfiguration(
        apply.transition.candidate.sampleRate,
        OpticalModeWire(apply.transition.candidate.opticalInput),
        OpticalModeWire(apply.transition.candidate.opticalOutput),
        &inputChannels, &outputChannels);
    if (hardwareKr != kIOReturnSuccess) {
        (void)dispatch(ASFW::Configuration::HardwareCompleted{
            .identity = apply.transition.identity,
            .outcome = ASFW::Configuration::HardwareUnknown{},
        });
        (void)super::PerformDeviceConfigurationChange(change_action, in_change_info);
        uint64_t expected = identity.token;
        (void)ivars->configurationInFlightToken.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel);
        ASFW_LOG_ERROR(Audio,
                       "[AudioConfig] ADK Perform hardware failure token=%llu kr=0x%x",
                       identity.token, hardwareKr);
        return hardwareKr;
    }
    kr = dispatch(ASFW::Configuration::HardwareCompleted{
        .identity = apply.transition.identity,
        .outcome = ASFW::Configuration::HardwareConfirmedRequested{
            .confirmed = {.configuration = apply.transition.candidate},
        },
    });
    if (kr != kIOReturnSuccess || transition.effects.size() != 1 ||
        !std::holds_alternative<ASFW::Configuration::ProjectADKEffect>(transition.effects[0])) {
        const kern_return_t superKr = super::PerformDeviceConfigurationChange(change_action, in_change_info);
        uint64_t expected = identity.token;
        (void)ivars->configurationInFlightToken.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel);
        return kr != kIOReturnSuccess ? kr : superKr;
    }
    const auto project = std::get<ASFW::Configuration::ProjectADKEffect>(transition.effects[0]);
    const kern_return_t mutation = ApplyADKConfigurationProjection(
        *this, driverIvars, project.plan.confirmed.configuration,
        inputChannels, outputChannels);
    const kern_return_t runtimeKr = mutation == kIOReturnSuccess
        ? driverIvars.device.audioNub->CommitDeviceConfiguration(
              project.plan.confirmed.configuration.sampleRate,
              OpticalModeWire(project.plan.confirmed.configuration.opticalInput),
              OpticalModeWire(project.plan.confirmed.configuration.opticalOutput))
        : mutation;
    const kern_return_t superKr = super::PerformDeviceConfigurationChange(change_action, in_change_info);
    const kern_return_t finishKr = dispatch(ASFW::Configuration::ProjectionFinished{
        .identity = project.plan.identity,
        .customProjectionSucceeded = mutation == kIOReturnSuccess && runtimeKr == kIOReturnSuccess,
        .superclassSucceeded = superKr == kIOReturnSuccess,
    });
    const kern_return_t result = mutation != kIOReturnSuccess ? mutation
        : runtimeKr != kIOReturnSuccess ? runtimeKr
        : superKr != kIOReturnSuccess ? superKr : finishKr;
    uint64_t expected = identity.token;
    (void)ivars->configurationInFlightToken.compare_exchange_strong(
        expected, 0, std::memory_order_acq_rel);
    if (result == kIOReturnSuccess) {
        ivars->configurationResumeToken.store(identity.token, std::memory_order_release);
    }
    ASFW_LOG(Audio,
             "[AudioConfig] ADK Perform result endpoint=%llu token=%llu rate=%u in=%u out=%u kr=0x%x",
             driverIvars.device.endpointId, identity.token,
             project.plan.confirmed.configuration.sampleRate, inputChannels,
             outputChannels, result);
    return result;
}

kern_return_t ASFWAudioDevice::AbortDeviceConfigurationChange(
    uint64_t change_action, OSObject* in_change_info) {
    if (ivars && ivars->configurationLock && ivars->configurationEnabled) {
        IOLockLock(ivars->configurationLock);
        const auto pending = ASFW::Configuration::CoherentSnapshot(ivars->configurationMachine.state);
        const auto result = ASFW::Configuration::Reduce(
            ivars->configurationMachine,
            ASFW::Configuration::ConfigurationEvent{ASFW::Configuration::ADKAborted{
                .identity = {.endpointId = pending ? pending->endpointId : 0,
                             .token = change_action,
                             .routeGeneration = pending ? pending->routeGeneration : 0},
            }});
        if (result) ivars->configurationMachine = result->next;
        IOLockUnlock(ivars->configurationLock);
        uint64_t expected = change_action;
        (void)ivars->configurationInFlightToken.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel);
        ASFW_LOG(Audio, "[AudioConfig] host aborted token=%llu", change_action);
    }
    return super::AbortDeviceConfigurationChange(change_action, in_change_info);
}
