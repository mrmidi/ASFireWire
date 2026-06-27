//
// ASFWAudioDevice.cpp
// ASFWDriver
//
// IOUserAudioDevice subclass implementing StartIO/StopIO for transport lifecycle.
//
#include <new>

#include "ASFWAudioDevice.h"
#include "ASFWAudioDriverPrivate.hpp"
#include "../../Logging/Logging.hpp"
#include "../Config/TimingCursorPolicy.hpp"
#include "Config/AudioProfileRegistry.hpp"
#include "../../Common/DriverKitOwnership.hpp"
#include "../../Shared/Isoch/IsochAudioTransport.hpp"

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>

struct ASFWAudioDevice_IVars {
    ASFWAudioDriver_IVars* driverIvars{nullptr};
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
    return true;
}

void ASFWAudioDevice::free() {
    if (ivars) {
        ivars->driverIvars = nullptr;
        IOSafeDeleteNULL(ivars, ASFWAudioDevice_IVars, 1);
    }
    super::free();
}

void ASFWAudioDevice::SetDriverIvars(ASFWAudioDriver_IVars* ivars) {
    if (this->ivars) {
        this->ivars->driverIvars = ivars;
    }
}

kern_return_t ASFWAudioDevice::StartIO(IOUserAudioStartStopFlags in_flags) {
    if (!ivars || !ivars->driverIvars) {
        ASFW_LOG(Audio, "ASFWAudioDevice: StartIO failed - no driver ivars");
        return kIOReturnNotReady;
    }

    ASFW_LOG(DirectAudio, "ASFWAudioDevice: StartIO flags=0x%llx",
             static_cast<uint64_t>(in_flags));

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
            ivars.runtime.txSlotProvider.controlBlock = nullptr;
            ivars.runtime.txSlotProvider.numSlots = 0;
            ivars.runtime.txExecutionTimeline.controlBlock = nullptr;

            // Secondary playback stream resources.
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
        ivars.runtime.lastHalZeroTimestampGeneration.store(0, std::memory_order_release);
        ivars.runtime.lastHalZeroTimestampSampleFrame.store(0, std::memory_order_release);
        ivars.runtime.lastHalZeroTimestampHostTicks.store(0, std::memory_order_release);

        // --- Allocate and map shared TX isoch resources ---
        {
            const auto* baseProfile = ASFW::Isoch::Audio::AudioProfileRegistry::FindProfile(
                ivars.device.vendorId,
                ivars.device.modelId,
                ivars.device.guid
            );
            const auto* profile = static_cast<const ASFW::Isoch::Audio::DICE::IDiceDeviceProfile*>(baseProfile);
            if (!profile) {
                ASFW_LOG(Audio, "ASFWAudioDevice: StartIO failed - profile not found");
                kr = failStart(kIOReturnError, "ResolveProfile");
                return;
            }

            ASFW::Isoch::Audio::DICE::DiceStreamConfig txConfig{};
            if (!profile->BuildDefaultTxStreamConfig(txConfig)) {
                ASFW_LOG(Audio, "ASFWAudioDevice: StartIO failed - BuildDefaultTxStreamConfig failed");
                kr = failStart(kIOReturnError, "BuildDefaultTxStreamConfig");
                return;
            }

            const uint32_t numSlots =
                ASFW::IsochTransport::AudioTimingGeometry::kTxSharedSlotPackets;
            const uint32_t maxPacketBytes =
                8u + static_cast<uint32_t>(txConfig.framesPerDataPacket) * txConfig.dbs * 4u;
            const uint32_t interruptInterval =
                ASFW::IsochTransport::AudioTimingGeometry::kTimingGroupPackets;

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
            auto* metadataRing = reinterpret_cast<ASFW::IsochTransport::TxPacketMeta*>(ivars.txMetadataMap->GetAddress());
            auto* controlBlock = reinterpret_cast<ASFW::IsochTransport::TxStreamControl*>(ivars.txControlMap->GetAddress());

            ivars.runtime.txSlotProvider.payloadBase = payloadBase;
            ivars.runtime.txSlotProvider.metadataRing = metadataRing;
            ivars.runtime.txSlotProvider.controlBlock = controlBlock;
            ivars.runtime.txSlotProvider.numSlots = numSlots;
            ivars.runtime.txSlotProvider.slotStrideBytes = maxPacketBytes;
            ivars.runtime.txSlotProvider.isoChannel = txConfig.sid;

            ivars.runtime.txExecutionTimeline.controlBlock = controlBlock;

            if (!ivars.runtime.txStreamEngine.Configure(*profile, txConfig)) {
                ASFW_LOG(Audio, "ASFWAudioDevice: txStreamEngine Configure failed");
                kr = failStart(kIOReturnError, "ConfigureTxStreamEngine");
                return;
            }
            ivars.runtime.txStreamEngine.BindSlotProvider(&ivars.runtime.txSlotProvider);
            ivars.runtime.txStreamEngine.ResetForStart(0, 0);
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
            ASFW::Isoch::Audio::DICE::DiceStreamConfig txConfig2{};
            if (!profile->BuildDefaultTxStreamConfig(txConfig2)) {
                kr = failStart(kIOReturnError, "BuildDefaultTxStreamConfig2");
                return;
            }
            txConfig2.sourceChannelOffset = txConfig2.pcmChannels;

            const uint32_t numSlots2 =
                ASFW::IsochTransport::AudioTimingGeometry::kTxSharedSlotPackets;
            const uint32_t maxPacketBytes2 =
                8u + static_cast<uint32_t>(txConfig2.framesPerDataPacket) * txConfig2.dbs * 4u;
            const uint32_t interruptInterval2 =
                ASFW::IsochTransport::AudioTimingGeometry::kTimingGroupPackets;

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
            auto* metadataRing2 = reinterpret_cast<ASFW::IsochTransport::TxPacketMeta*>(ivars.txMetadataMapSecondary->GetAddress());
            auto* controlBlock2 = reinterpret_cast<ASFW::IsochTransport::TxStreamControl*>(ivars.txControlMapSecondary->GetAddress());

            ivars.runtime.txSlotProviderSecondary.payloadBase = payloadBase2;
            ivars.runtime.txSlotProviderSecondary.metadataRing = metadataRing2;
            ivars.runtime.txSlotProviderSecondary.controlBlock = controlBlock2;
            ivars.runtime.txSlotProviderSecondary.numSlots = numSlots2;
            ivars.runtime.txSlotProviderSecondary.slotStrideBytes = maxPacketBytes2;
            // isoChannel here is only a fallback; the host IT ring stamps the real
            // transmit channel (PlaybackChannel(1)) from its Configure() value.
            ivars.runtime.txSlotProviderSecondary.isoChannel = txConfig2.sid;

            if (!ivars.runtime.txStreamEngineSecondary.Configure(*profile, txConfig2)) {
                kr = failStart(kIOReturnError, "ConfigureTxStreamEngine2");
                return;
            }
            ivars.runtime.txStreamEngineSecondary.BindSlotProvider(&ivars.runtime.txSlotProviderSecondary);
            ivars.runtime.txStreamEngineSecondary.ResetForStart(0, 0);
            ivars.runtime.txSecondaryActive = true;

            ASFW_LOG(Audio,
                     "ASFWAudioDevice: Allocated & configured SECONDARY TX stream offset=%u dbs=%u slots=%u slotSize=%u",
                     txConfig2.sourceChannelOffset, txConfig2.dbs, numSlots2, maxPacketBytes2);
        }
        }

        // --- Prefill TX ring ---
        ASFW::Audio::DriverKit::PrefillTxRingBeforeStart(ivars);

        auto* prefillControl = ivars.runtime.txSlotProvider.controlBlock;
        const uint64_t prefillExpose =
            prefillControl
                ? prefillControl->exposeCursor.load(std::memory_order_acquire)
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
        auto* txControl = ivars.runtime.txSlotProvider.controlBlock;
        if (!txControl ||
            txControl->abiVersion != ASFW::IsochTransport::kTransportAbiVersion ||
            txControl->numSlots != ASFW::IsochTransport::AudioTimingGeometry::kTxSharedSlotPackets ||
            txControl->interruptInterval != ASFW::IsochTransport::AudioTimingGeometry::kTxPacketsPerGroup ||
            txControl->ztsPeriodFrames != ASFW::IsochTransport::AudioTimingGeometry::kHalZeroTimestampPeriodFrames) {
            ASFW_LOG(Audio,
                     "ASFWAudioDevice: TX geometry/ABI mismatch abi=%u slots=%u group=%u zts=%u",
                     txControl ? txControl->abiVersion : 0,
                     txControl ? txControl->numSlots : 0,
                     txControl ? txControl->interruptInterval : 0,
                     txControl ? txControl->ztsPeriodFrames : 0);
            kr = failStart(
                kIOReturnUnsupported, "ValidateTxTransportGeometry");
            return;
        }

        // AudioDriverKit needs a valid clock anchor when StartIO transitions
        // the device into the running state. The hardware ZTS action executes
        // on its dedicated queue, so it can publish while this work queue waits.
        constexpr uint32_t kInitialHardwareZtsTimeoutMs = 500;
        uint32_t ztsWaitMs = 0;
        while (ivars.runtime.lastHalZeroTimestampHostTicks.load(
                   std::memory_order_acquire) == 0 &&
               ztsWaitMs < kInitialHardwareZtsTimeoutMs) {
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

        // --- Log timing policy ---
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
                 "ADK DBG DUPLEX ready guid=0x%016llx rxStarted=1 txStarted=1 bindValid=%d hasIn=%d hasOut=%d audioDevice=%p",
                 ivars.device.guid,
                 ivars.runtime.directAudioSkeletonBound.load(std::memory_order_acquire),
                 ivars.runtime.directAudioGraph.HasInput(),
                 ivars.runtime.directAudioGraph.HasOutput(),
                 static_cast<void*>(ivars.runtime.directAudioGraph.audioDevice));
    });

    if (kr == kIOReturnSuccess) {
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
            ASFW::IsochTransport::AudioTimingGeometry::kHalIoPeriodFrames);
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

    ASFW_LOG(DirectAudio, "ASFWAudioDevice: StopIO flags=0x%llx",
             static_cast<uint64_t>(in_flags));

    auto& ivars = *this->ivars->driverIvars;
    __block kern_return_t kr = kIOReturnSuccess;

    ivars.workQueue->DispatchSync(^{
        ivars.runtime.isRunning.store(false, std::memory_order_release);
        ivars.runtime.txActive.store(false, std::memory_order_release);

        if (ivars.runtime.directAudioGraph.control) {
            const auto* control = ivars.runtime.directAudioGraph.control;
            ASFW_LOG(DirectAudio,
                     "ADK DBG STOPIO guid=0x%016llx callbacks=%llu outsideRun=%llu zts=%llu rxZts=%llu rxAdk=%llu beginRead=%llu writeEnd=%llu writtenEndFrame=%llu txPackets=%llu txSilence=%llu txUnderruns=%llu",
                     ivars.device.guid,
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
        ivars.runtime.txSlotProvider.controlBlock = nullptr;
        ivars.runtime.txSlotProvider.numSlots = 0;
        ivars.runtime.txExecutionTimeline.controlBlock = nullptr;

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
        ivars.runtime.txSlotProviderSecondary.controlBlock = nullptr;
        ivars.runtime.txSlotProviderSecondary.numSlots = 0;

        if (ivars.device.audioNub) {
            ivars.device.audioNub->FreeTxIsochResources();
        }

        kr = super::StopIO(in_flags);
    });

    return kr;
}

kern_return_t ASFWAudioDevice::HandleChangeSampleRate(double in_sample_rate) {
    ASFW_LOG(Audio, "ASFWAudioDevice: HandleChangeSampleRate %.0f Hz (entry)", in_sample_rate);
    if (!ivars || !ivars->driverIvars) {
        ASFW_LOG(Audio,
                 "ASFWAudioDevice: HandleChangeSampleRate NOT READY (ivars=%p driverIvars=%p)",
                 static_cast<void*>(ivars),
                 static_cast<void*>(ivars ? ivars->driverIvars : nullptr));
        return kIOReturnNotReady;
    }
    auto& ivars = *this->ivars->driverIvars;

    const uint32_t rateHz = static_cast<uint32_t>(in_sample_rate);

    // Reject a rate change while IO is active. Live (hot) reconfiguration would
    // restart the duplex transport underneath running IO across the cross-service
    // seam, which currently desynchronizes the device clock from the host and
    // leaves audio dead until a full stop/replug. Until hot-swap is supported,
    // require the device to be stopped: returning an error makes CoreAudio keep
    // the current rate rather than believe the hardware moved. The rate then
    // changes cleanly on the next idle pick + StartIO.
    if (ivars.runtime.isRunning.load(std::memory_order_acquire)) {
        ASFW_LOG(Audio,
                 "ASFWAudioDevice: HandleChangeSampleRate %.0f Hz refused - IO active "
                 "(stop playback to change sample rate)",
                 in_sample_rate);
        return kIOReturnBusy;
    }

    // Program the device's DICE clock to the new rate via the transport-side
    // coordinator (CLOCK_SELECT + duplex reconfigure). The device is idle here,
    // so this stores/applies the clock for the next StartIO. Reject the change if
    // the transport can't apply it so CoreAudio does not believe the hardware moved.
    if (ivars.device.audioNub) {
        const kern_return_t kr =
            ivars.device.audioNub->RequestSampleRateChange(rateHz);
        if (kr != kIOReturnSuccess) {
            ASFW_LOG(Audio,
                     "ASFWAudioDevice: HandleChangeSampleRate transport reconfig failed: 0x%x",
                     kr);
            return kr;
        }
        ivars.device.currentSampleRate = static_cast<double>(rateHz);
    }

    // Commit the rate to the ADK device. The validated ADK contract
    // (ADKVirtualAudioLab) applies the change by calling SetSampleRate here, not
    // by delegating to super — the base HandleChangeSampleRate does not move the
    // active format, so without this the HAL reverts to the previous rate.
    const kern_return_t setKr = SetSampleRate(in_sample_rate);
    if (setKr != kIOReturnSuccess) {
        ASFW_LOG(Audio,
                 "ASFWAudioDevice: HandleChangeSampleRate SetSampleRate(%.0f) failed: 0x%x",
                 in_sample_rate, setKr);
    }
    return setKr;
}
