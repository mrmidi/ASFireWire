//
// ASFWAudioDevice.cpp
// ASFWDriver
//
// IOUserAudioDevice subclass implementing StartIO/StopIO for transport lifecycle.
//
#include <expected>
#include <new>

#include "ASFWAudioDevice.h"
#include "ASFWAudioDriverPrivate.hpp"
#include "../../Logging/Logging.hpp"
#include "Config/AudioProfileRegistry.hpp"
#include "Config/ResolvedStreamConfig.hpp"
#include "../Protocols/Duplex/AudioClockConfig.hpp"
#include "../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../../Common/DriverKitOwnership.hpp"
#include "../../Isoch/Core/IsochTxQueue.hpp"
#include "../../Isoch/Core/IsochDmaGeometry.hpp"

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>

// The audio completion group (frames per interrupt, input-safety floor, ZTS
// tiling) and the transport's IOC cadence must be the same number of cycles.
// Checked here, on the audio side of the seam: transport stays payload-opaque.
static_assert(ASFW::IsochTransport::AudioTimingGeometry::kTimingGroupPackets ==
                  ASFW::Isoch::IsochDmaGeometry::kPacketsPerInterrupt,
              "audio completion group must equal the OHCI interrupt group");

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
            ivars.runtime.txSlotProvider.queueControl = nullptr;
            ivars.runtime.txSlotProvider.audioControl = nullptr;
            ivars.runtime.txSlotProvider.numSlots = 0;
            ivars.runtime.txExecutionTimeline.queueControl = nullptr;

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
            if (ivars.runtime.mAudioInternalTxActive && ivars.txPreparationQueue) {
                ivars.txPreparationQueue->DispatchSync(^{ });
            }
            ivars.runtime.mAudioInternalTxTiming.Disarm();
            ivars.runtime.mAudioTxClockBridge.Disarm();
            ivars.runtime.mAudioInternalTxActive = false;
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
        ivars.runtime.mAudioInternalTxTiming.Disarm();
        ivars.runtime.mAudioTxClockBridge.Disarm();
        ivars.runtime.mAudioInternalTxActive = false;
        ivars.runtime.lastHalZeroTimestampGeneration.store(0, std::memory_order_release);
        ivars.runtime.lastHalZeroTimestampSampleFrame.store(0, std::memory_order_release);
        ivars.runtime.lastHalZeroTimestampHostTicks.store(0, std::memory_order_release);

        // --- Allocate and map shared TX isoch resources ---
        uint32_t initialClockAnchorTimeoutMs = 500;
        {
            // Resolved once at graph construction (G-19); never looked up here.
            const auto* profile =
                static_cast<const ASFW::Isoch::Audio::IAudioStreamProfile*>(ivars.device.profile);
            if (!profile) {
                ASFW_LOG(Audio, "ASFWAudioDevice: StartIO failed - graph did not resolve a profile");
                kr = failStart(kIOReturnNotReady, "ResolveProfile");
                return;
            }
            initialClockAnchorTimeoutMs = profile->InitialClockAnchorTimeoutMs();
            if (!ASFW::Audio::DriverKit::SelectTxClockDomain(ivars, *profile)) {
                kr = failStart(kIOReturnUnsupported, "MAudioInternalTxTiming");
                return;
            }

            // How many playback streams to arm, and what shape each one is.
            //
            // Both come from the device when the publishing side resolved them
            // and carried them across the nub. The profile supplies only the
            // framing constants the DICE registers do not hold -- fdf, fmt,
            // frames per data packet, stream mode -- and no longer decides how
            // many channels a stream carries.
            //
            // That distinction is the whole point: the recorded Midas Venice
            // F24 carries 16 + 8 on playback while its profile describes an F32
            // at 16 + 16, so building stream 1 from the profile put 16 channels
            // and DBS 16 into a stream the device frames with 8 slots. The
            // device is authoritative and now actually reaches the packetizer.
            const uint32_t resolvedPlaybackStreams = ivars.device.playbackStreamCount;
            const uint32_t playbackStreamCount =
                ASFW::Isoch::Audio::ResolvedPlaybackStreamCount(profile->TxStreamCount(),
                                                                resolvedPlaybackStreams);

            // This build allocates one primary and one secondary TX stream, so
            // it cannot honour a device carrying more. Refuse rather than arm a
            // subset: silently dropping a stream the device transmits on is the
            // failure mode the geometry work exists to end.
            constexpr uint32_t kMaxPlaybackStreamsThisBuild = 2;
            if (playbackStreamCount > kMaxPlaybackStreamsThisBuild) {
                ASFW_LOG(Audio,
                         "ASFWAudioDevice: StartIO failed - device carries %u playback streams, "
                         "this build configures at most %u",
                         playbackStreamCount, kMaxPlaybackStreamsThisBuild);
                kr = failStart(kIOReturnUnsupported, "PlaybackStreamCount");
                return;
            }
            if (resolvedPlaybackStreams == 0) {
                if (ivars.device.resolvedGeometryRequired) {
                    // The publisher resolved geometry and said so, but none
                    // arrived. Falling back to the profile here would silently
                    // reinstate the exact mismatch the resolution removed -- a
                    // Venice F24 framed as an F32 -- so this is a transport
                    // fault, not a device without geometry.
                    ASFW_LOG(Audio,
                             "ASFWAudioDevice: StartIO failed - device requires resolved "
                             "playback geometry but none crossed the nub");
                    kr = failStart(kIOReturnNotFound, "MissingResolvedGeometry");
                    return;
                }
                ASFW_LOG(Audio,
                         "ASFWAudioDevice: StartIO has no resolved playback geometry; framing "
                         "from profile constants (streams=%u) - correct only if every stream "
                         "is the same width",
                         playbackStreamCount);
            }

            const auto buildTxConfig =
                [&profile, &ivars](uint32_t index,
                                   ASFW::Isoch::Audio::AudioStreamConfig& out) -> bool {
                return ASFW::Isoch::Audio::BuildResolvedTxStreamConfig(
                    *profile, ivars.device.playbackStreams,
                    ivars.device.playbackStreamCount, index, out);
            };

            ASFW::Isoch::Audio::AudioStreamConfig txConfig{};
            if (!buildTxConfig(0, txConfig)) {
                ASFW_LOG(Audio, "ASFWAudioDevice: StartIO failed - BuildDefaultTxStreamConfig failed");
                kr = failStart(kIOReturnError, "BuildDefaultTxStreamConfig");
                return;
            }
            // The profile describes the wire geometry at its default (48 kHz);
            // live cadence/FDF follow the device's current nominal rate.
            if (ivars.device.currentSampleRate > 0) {
                txConfig.sampleRate =
                    static_cast<uint32_t>(ivars.device.currentSampleRate);
            }

            const uint32_t numSlots =
                ASFW::IsochTransport::AudioTimingGeometry::kTxSharedSlotPackets;
            const uint32_t maxPacketBytes =
                ASFW::Isoch::Audio::TxPacketBytesForStreamConfig(txConfig);
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
            auto* metadataRing = reinterpret_cast<ASFW::Isoch::IsochTxPacketMeta*>(ivars.txMetadataMap->GetAddress());
            auto* queueControl = reinterpret_cast<ASFW::Isoch::IsochTxQueueControl*>(ivars.txControlMap->GetAddress());

            const auto armed = ASFW::Audio::DriverKit::ArmPrimaryTxProducer(
                ivars, *profile, txConfig,
                {.payloadBase = payloadBase,
                 .metadataRing = metadataRing,
                 .queueControl = queueControl,
                 .numSlots = numSlots,
                 .slotStrideBytes = maxPacketBytes});
            if (armed.failedStage != nullptr) {
                kr = failStart(armed.status, armed.failedStage);
                return;
            }
            const uint32_t timingRateHz =
                ivars.device.currentSampleRate > 0
                    ? static_cast<uint32_t>(ivars.device.currentSampleRate)
                    : 48000u;

            ASFW_LOG(Audio,
                     "ASFWAudioDevice: Allocated & configured TX isoch resources channel=%u rxTransferDelay=%u txTransferDelay=%u (rate=%u)",
                     txConfig.sid,
                     control->rxTransferDelayTicks.load(std::memory_order_relaxed),
                     control->txTransferDelayTicks.load(std::memory_order_relaxed),
                     timingRateHz);

        // Secondary playback uses its own wire shape and source-channel slice.
        // It shadows the master's packet timing; the duplex bring-up wires its
        // shared slab to the matching IT context.
        if (playbackStreamCount > 1) {
            ASFW::Isoch::Audio::AudioStreamConfig txConfig2{};
            if (!buildTxConfig(1, txConfig2)) {
                kr = failStart(kIOReturnError, "BuildDefaultTxStreamConfig2");
                return;
            }
            if (ivars.device.currentSampleRate > 0) {
                txConfig2.sampleRate =
                    static_cast<uint32_t>(ivars.device.currentSampleRate);
            }

            const uint32_t numSlots2 =
                ASFW::IsochTransport::AudioTimingGeometry::kTxSharedSlotPackets;
            const uint32_t maxPacketBytes2 =
                ASFW::Isoch::Audio::TxPacketBytesForStreamConfig(txConfig2);
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
            ivars.runtime.txStreamEngineSecondary.ResetForStart(0, 0);
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
            txControl->numSlots != ASFW::IsochTransport::AudioTimingGeometry::kTxSharedSlotPackets ||
            txControl->slotStrideBytes != ivars.runtime.txSlotProvider.slotStrideBytes ||
            txControl->maxPacketBytes != ivars.runtime.txSlotProvider.slotStrideBytes ||
            txControl->interruptInterval != ASFW::IsochTransport::AudioTimingGeometry::kTxPacketsPerGroup) {
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
        // the device into the running state. The hardware ZTS action executes
        // on its dedicated queue, so it can publish while this work queue
        // waits. The budget is profile-owned: BeBoB devices spend ~1 s in CIP
        // NO-DATA after their input stream starts before the first data packet
        // can seed the anchor (Linux bebob_stream.c:661-666).
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
            if (ivars.runtime.mAudioInternalTxActive.load(
                    std::memory_order_acquire)) {
                ASFW_LOG_ERROR(
                    DirectAudio,
                    "[MAudioTxClock] initial TX ZTS timed out after %u ms wakes=%llu conversionFailures=%llu stamps=%llu",
                    ztsWaitMs,
                    ivars.runtime.mAudioTxClockNoDataWakes.load(
                        std::memory_order_relaxed),
                    ivars.runtime.mAudioTxClockConversionFailures.load(
                        std::memory_order_relaxed),
                    ivars.runtime.txSlotProvider.queueControl
                        ? ivars.runtime.txSlotProvider.queueControl->
                              completionStampCount.load(std::memory_order_relaxed)
                        : 0);
            }
            // RX profiles attribute a missing anchor through capture counters;
            // M-Audio uses qualified TX completion stamps and reports those
            // independently above.
            if (const auto* control = ivars.runtime.directAudioGraph.control) {
                ASFW_LOG(Audio,
                         "ASFWAudioDevice: initial hardware ZTS timed out after %u ms "
                         "rxSeen=%llu data=%llu noData=%llu short=%llu badCip=%llu "
                         "zeroDbs=%llu geometry=%llu",
                         ztsWaitMs,
                         control->rxPacketsSeen.load(std::memory_order_relaxed),
                         control->rxDataPackets.load(std::memory_order_relaxed),
                         control->rxNoDataPackets.load(std::memory_order_relaxed),
                         control->rxShortPackets.load(std::memory_order_relaxed),
                         control->rxInvalidCipHeaders.load(std::memory_order_relaxed),
                         control->rxZeroDataBlockSize.load(std::memory_order_relaxed),
                         control->rxGeometryMismatch.load(std::memory_order_relaxed));
            } else {
                ASFW_LOG(
                    Audio,
                    "ASFWAudioDevice: initial hardware ZTS timed out after %u ms "
                    "(no control block)",
                    ztsWaitMs);
            }
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
        if (ivars.runtime.mAudioInternalTxActive && ivars.txPreparationQueue) {
            ivars.txPreparationQueue->DispatchSync(^{ });
        }
        ivars.runtime.mAudioInternalTxTiming.Disarm();
        ivars.runtime.mAudioTxClockBridge.Disarm();
        ivars.runtime.mAudioInternalTxActive = false;

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
        ivars.runtime.txSlotProvider.queueControl = nullptr;
        ivars.runtime.txSlotProvider.audioControl = nullptr;
        ivars.runtime.txSlotProvider.numSlots = 0;
        ivars.runtime.txExecutionTimeline.queueControl = nullptr;

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

        if (ivars.device.audioNub) {
            ivars.device.audioNub->FreeTxIsochResources();
        }

        kr = super::StopIO(in_flags);
    });

    return kr;
}

namespace {

// Custom IOUserAudioDevice configuration-change action for a sample-rate move
// ("ASFWRATE"), whether the HAL or the device initiated it. Purely
// driver-internal per the ADK contract.
constexpr uint64_t kConfigChangeActionSampleRate = 0x4153465752415445ULL;

// Validation shared by the request (HandleChangeSampleRate, external resync)
// and the commit (PerformDeviceConfigurationChange): the rate must be one this
// device advertised and resolve to a timing geometry that fits the shared
// allocation. Resolving BEFORE the transport moves means an unsupported rate is
// refused while the device clock is untouched (TIMING_GEOMETRY_OWNERSHIP.md §4).
[[nodiscard]] std::expected<ASFW::Audio::Runtime::ResolvedTimingGeometry, kern_return_t>
ValidateSampleRate(ASFWAudioDriver_IVars& ivars, uint32_t rateHz, const char* origin) noexcept {
    bool rateSupported = false;
    for (uint32_t i = 0; i < ivars.device.sampleRateCount; ++i) {
        if (static_cast<uint32_t>(ivars.device.sampleRates[i]) == rateHz) {
            rateSupported = true;
            break;
        }
    }
    if (!rateSupported) {
        ASFW_LOG(Audio, "[Timing] %{public}s rate %u refused - not advertised", origin, rateHz);
        return std::unexpected(kIOReturnUnsupported);
    }
    // Advertised is not streamable: DICE announces every CLOCK_CAPABILITIES
    // rate, as the TCAT kexts do, while this build streams 1x only. Refuse a
    // parked rate here, before a configuration-change window is opened, rather
    // than letting CommitSampleRate fail inside it (the nub applies the same
    // gate in RequestSampleRateChange). See DiceAudioBackend::
    // RebuildEndpointForNewGeometry for what enabling high rates needs.
    const ASFW::Audio::AudioClockConfig requested{.sampleRateHz = rateHz};
    if (!ASFW::Audio::IsSupportedAudioClockConfig(requested) &&
        !ASFW::Audio::IsSupportedMAudioSpecialClockConfig(requested)) {
        ASFW_LOG(Audio,
                 "[Timing] %{public}s rate %u refused - announced by the device but not "
                 "streamable in this build (high rates parked)",
                 origin, rateHz);
        return std::unexpected(kIOReturnUnsupported);
    }
    // Without the nub the device clock can't be programmed; succeeding would
    // make CoreAudio believe the hardware moved when it didn't.
    if (!ivars.device.audioNub) {
        ASFW_LOG(Audio, "[Timing] %{public}s rate %u refused - no audio nub", origin, rateHz);
        return std::unexpected(kIOReturnNotReady);
    }
    if (!ivars.device.profile) {
        ASFW_LOG(Audio, "[Timing] %{public}s rate %u refused - no resolved profile", origin, rateHz);
        return std::unexpected(kIOReturnNotReady);
    }
    const auto next = ASFW::Audio::DriverKit::ResolveProfileTimingGeometry(
        *ivars.device.profile, rateHz, ivars.device.streamModeRaw);
    if (!next) {
        ASFW_LOG(Audio, "[Timing] %{public}s rate %u refused: %{public}s", origin, rateHz,
                 ASFW::Audio::Runtime::TimingGeometryErrorName(next.error()));
        return std::unexpected(kIOReturnUnsupported);
    }
    return *next;
}

// Applies a validated rate inside the host's configuration-change window (IO
// is stopped), in the midi branch's order: hardware clock, device nominal
// rate, zero-timestamp period, stream formats, HAL declarations, then the
// direct binding's active ring. The ZTS period is legal to change only here,
// which is why a HAL request is deferred into this window instead of being
// applied in HandleChangeSampleRate.
[[nodiscard]] kern_return_t CommitSampleRate(
    ASFWAudioDevice& device,
    ASFWAudioDriver_IVars& ivars,
    const ASFW::Audio::Runtime::ResolvedTimingGeometry& next) noexcept {
    const uint32_t rateHz = next.sampleRateHz;
    const double rate = static_cast<double>(rateHz);

    // Program the device clock through the transport-side coordinator
    // (CLOCK_SELECT + duplex reconfigure). The coordinator also moves the
    // endpoint runtime's active ring (AudioEndpointRuntime::SetCurrentSampleRate).
    // A device-initiated change is already at this rate, so the coordinator
    // skips the redundant CLOCK_SELECT write.
    kern_return_t kr = ivars.device.audioNub->RequestSampleRateChange(rateHz);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Audio, "[Timing] rate %u: transport reconfig failed 0x%x", rateHz, kr);
        return kr;
    }
    ivars.device.currentSampleRate = rate;

    // The validated ADK contract (ADKVirtualAudioLab) moves the active format
    // with SetSampleRate; the base implementation does nothing more.
    if ((kr = device.SetSampleRate(rate)) != kIOReturnSuccess) {
        ASFW_LOG(Audio, "[Timing] rate %u: SetSampleRate failed 0x%x", rateHz, kr);
        return kr;
    }

    // V3: the ZTS period is a function of the rate tier (12288 frames at 1x,
    // 24576 at 2x). The HAL wraps the stream buffer on it.
    const uint32_t priorPeriod = device.GetZeroTimestampPeriod();
    if (priorPeriod != next.zeroTimestampPeriodFrames) {
        if ((kr = device.SetZeroTimeStampPeriod(next.zeroTimestampPeriodFrames)) !=
            kIOReturnSuccess) {
            ASFW_LOG(Audio, "[Timing] rate %u: SetZeroTimeStampPeriod(%u) failed 0x%x",
                     rateHz, next.zeroTimestampPeriodFrames, kr);
            return kr;
        }
        ASFW_LOG(Audio, "[Timing] rate %u: ZTS period %u -> %u", rateHz, priorPeriod,
                 next.zeroTimestampPeriodFrames);
    }

    // CoreAudio params-change contract: "new params" includes each stream's
    // CURRENT format, not just the device nominal rate -- otherwise clients see
    // device=new-rate / stream=old-rate. FillFloat32Format is the single
    // construction point, so the format matches the advertised entry.
    IOUserAudioStreamBasicDescription inputFormat{};
    IOUserAudioStreamBasicDescription outputFormat{};
    ASFW::Audio::DriverKit::FillFloat32Format(inputFormat, rate, ivars.device.inputChannelCount);
    ASFW::Audio::DriverKit::FillFloat32Format(outputFormat, rate, ivars.device.outputChannelCount);
    if (ivars.inputStream &&
        (kr = ivars.inputStream->SetCurrentStreamFormat(&inputFormat)) != kIOReturnSuccess) {
        ASFW_LOG(Audio, "[Timing] rate %u: input SetCurrentStreamFormat failed 0x%x", rateHz, kr);
        return kr;
    }
    if (ivars.outputStream &&
        (kr = ivars.outputStream->SetCurrentStreamFormat(&outputFormat)) != kIOReturnSuccess) {
        ASFW_LOG(Audio, "[Timing] rate %u: output SetCurrentStreamFormat failed 0x%x", rateHz, kr);
        return kr;
    }

    // Re-declare for the new rate (FW-183: the declarations scale per tier).
    ivars.device.timing = next;
    const auto& timing = ivars.device.timing;
    const kern_return_t declKr[] = {
        device.SetOutputLatency(timing.outputLatencyFrames),
        device.SetInputLatency(timing.inputLatencyFrames),
        device.SetOutputSafetyOffset(timing.outputSafetyOffsetFrames),
        device.SetInputSafetyOffset(timing.inputSafetyOffsetFrames),
    };
    for (const kern_return_t declared : declKr) {
        if (declared != kIOReturnSuccess) {
            ASFW_LOG(Audio, "[Timing] rate %u: re-declaration failed 0x%x", rateHz, declared);
            return declared;
        }
    }

    // Move the IO handler's view onto the new active ring, matching the ring
    // the HAL now wraps on and the one the endpoint runtime publishes.
    if (!ASFW::Audio::DriverKit::UpdateDirectAudioGeometry(ivars)) {
        return kIOReturnError;
    }

    ASFW::Audio::DriverKit::LogResolvedTimingGeometry("rate-change", timing);
    ASFW_LOG(Audio, "ASFWAudioDriver: Reported HAL latency out=%u/in=%u, safety out=%u/in=%u frames",
             timing.outputLatencyFrames, timing.inputLatencyFrames,
             timing.outputSafetyOffsetFrames, timing.inputSafetyOffsetFrames);
    return kIOReturnSuccess;
}

} // namespace

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

    if (in_sample_rate <= 0.0 ||
        static_cast<double>(static_cast<uint32_t>(in_sample_rate)) != in_sample_rate) {
        return kIOReturnBadArgument;
    }
    const uint32_t rateHz = static_cast<uint32_t>(in_sample_rate);
    if (rateHz == static_cast<uint32_t>(ivars.device.currentSampleRate) &&
        rateHz == ivars.device.timing.sampleRateHz) {
        return kIOReturnSuccess;
    }

    // Reject a rate change while IO is active. Live (hot) reconfiguration would
    // restart the duplex transport underneath running IO across the
    // cross-service seam. Returning an error makes CoreAudio keep the current
    // rate rather than believe the hardware moved.
    if (ivars.runtime.isRunning.load(std::memory_order_acquire)) {
        ASFW_LOG(Audio,
                 "ASFWAudioDevice: HandleChangeSampleRate %.0f Hz refused - IO active "
                 "(stop playback to change sample rate)",
                 in_sample_rate);
        return kIOReturnBusy;
    }
    const auto next = ValidateSampleRate(ivars, rateHz, "HAL");
    if (!next) {
        return next.error();
    }

    // The commit (including the ZTS period, legal only inside the perform
    // window) happens in PerformDeviceConfigurationChange; the host stops IO,
    // performs, then restarts IO (SAMPLE_RATE_EXPANSION.md, ADK transaction).
    ivars.device.pendingSampleRateHz.store(rateHz, std::memory_order_release);
    const kern_return_t kr =
        RequestDeviceConfigurationChange(kConfigChangeActionSampleRate, nullptr);
    if (kr != kIOReturnSuccess) {
        ivars.device.pendingSampleRateHz.store(0, std::memory_order_release);
        ASFW_LOG(Audio,
                 "ASFWAudioDevice: HandleChangeSampleRate %u Hz window request failed 0x%x",
                 rateHz, kr);
    }
    return kr;
}

kern_return_t ASFWAudioDevice::RequestExternalRateResync(uint32_t nominalRateHz) {
    if (!ivars || !ivars->driverIvars) {
        return kIOReturnNotReady;
    }
    auto& driverIvars = *this->ivars->driverIvars;

    if (static_cast<uint32_t>(driverIvars.device.currentSampleRate) ==
        nominalRateHz) {
        return kIOReturnSuccess; // already in sync — nothing to do
    }

    driverIvars.device.pendingSampleRateHz.store(nominalRateHz,
                                                 std::memory_order_release);
    ASFW_LOG(Audio,
             "ASFWAudioDevice: device-initiated clock change to %u Hz — "
             "requesting configuration-change window",
             nominalRateHz);
    // The host stops IO, calls PerformDeviceConfigurationChange, restarts IO
    // (AudioDriverKit contract; AppleUSBAudio's forced format change analog).
    return RequestDeviceConfigurationChange(kConfigChangeActionSampleRate,
                                            nullptr);
}

kern_return_t ASFWAudioDevice::PerformDeviceConfigurationChange(
    uint64_t change_action, OSObject* in_change_info) {
    if (change_action != kConfigChangeActionSampleRate || !ivars ||
        !ivars->driverIvars) {
        return super::PerformDeviceConfigurationChange(change_action,
                                                       in_change_info);
    }
    auto& driverIvars = *this->ivars->driverIvars;

    const uint32_t rateHz =
        driverIvars.device.pendingSampleRateHz.exchange(
            0, std::memory_order_acq_rel);
    if (rateHz == 0) {
        // Superseded/aborted meanwhile.
        return super::PerformDeviceConfigurationChange(change_action,
                                                       in_change_info);
    }

    // Re-validate: the profile or advertised rates may have moved since the
    // request, and a device-initiated change was not validated at all yet.
    kern_return_t kr = kIOReturnSuccess;
    const auto next = ValidateSampleRate(driverIvars, rateHz, "perform");
    if (!next) {
        kr = next.error();
    } else {
        kr = CommitSampleRate(*this, driverIvars, *next);
    }
    ASFW_LOG(Audio,
             "ASFWAudioDevice: sample rate change to %u Hz %{public}s (0x%x)",
             rateHz, kr == kIOReturnSuccess ? "committed" : "FAILED", kr);
    const kern_return_t superKr =
        super::PerformDeviceConfigurationChange(change_action, in_change_info);
    return kr != kIOReturnSuccess ? kr : superKr;
}

kern_return_t ASFWAudioDevice::AbortDeviceConfigurationChange(
    uint64_t change_action, OSObject* in_change_info) {
    if (change_action == kConfigChangeActionSampleRate) {
        if (ivars && ivars->driverIvars) {
            ivars->driverIvars->device.pendingSampleRateHz.store(
                0, std::memory_order_release);
        }
        ASFW_LOG(Audio,
                 "ASFWAudioDevice: sample rate change aborted by host");
    }
    return super::AbortDeviceConfigurationChange(change_action, in_change_info);
}
