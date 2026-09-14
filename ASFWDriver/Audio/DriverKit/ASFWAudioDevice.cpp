//
// ASFWAudioDevice.cpp
// ASFWDriver
//
// IOUserAudioDevice subclass implementing StartIO/StopIO for transport lifecycle.
//
#include <array>
#include <new>

#include "ASFWAudioDevice.h"
#include "ASFWAudioDeviceTokens.hpp"
#include "Config/HalTimingProjection.hpp"
#include "../Shared/AudioGeometryPolicy.hpp"
#include "../Shared/AudioRuntimeTuningStore.hpp"
#include "../Shared/AudioTimingGeometry.hpp"
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
    if (this->ivars->driverIvars->device.audioNub) {
        const uint32_t window =
            ivars->driverIvars->device.audioNub->CurrentRuntimeTuningWindow();
        if (window != 0) {
            const auto& active = ivars->driverIvars->runtime.activeTuning;
            ASFW_LOG(Audio,
                     "[AudioTuning] win=%u IO STARTING with slack=%u target=%u "
                     "lead=%u fr -- this geometry is now in force",
                     window, active.txDispatchSlackPackets,
                     active.PreparedTargetPackets(),
                     ASFW::Audio::Shared::PreparedLeadFrames(active, static_cast<uint32_t>(ivars->driverIvars->device.currentSampleRate)));
        }
    }
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

        const auto releaseTxResources = [&]() noexcept {
            if (ivars.device.audioNub) {
                ivars.device.audioNub->SetTxPcmSource(nullptr);
            }
            ivars.runtime.pcmPublicationCache.BeginEpoch(0);
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
            if (ivars.device.audioNub) {
                ivars.device.audioNub->SetTxPcmSource(nullptr);
            }
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

        // Pre-arming sanity checks
        const uint32_t activeRate = static_cast<uint32_t>(ivars.device.currentSampleRate);
        const uint32_t expectedRingFrames =
            ASFW::Audio::Shared::AudioTimingGeometry::FrameRingFrames(activeRate);
        const uint32_t expectedZtsPeriod =
            ASFW::Audio::Shared::AudioTimingGeometry::ZeroTimestampPeriodFrames(activeRate);

        if (ivars.runtime.directAudioGraph.sampleRateHz != activeRate) {
            ASFW_LOG_ERROR(Audio,
                           "ASFWAudioDevice: StartIO rate mismatch graph=%u device=%u",
                           ivars.runtime.directAudioGraph.sampleRateHz, activeRate);
            kr = failStart(kIOReturnBadArgument, "RateMismatch");
            return;
        }

        if (ivars.runtime.directAudioGraph.memory.activeOutputRingFrames != expectedRingFrames ||
            ivars.runtime.directAudioGraph.memory.activeInputRingFrames != expectedRingFrames) {
            ASFW_LOG_ERROR(Audio,
                           "ASFWAudioDevice: StartIO active ring frames mismatch expected=%u out=%u in=%u",
                           expectedRingFrames,
                           ivars.runtime.directAudioGraph.memory.activeOutputRingFrames,
                           ivars.runtime.directAudioGraph.memory.activeInputRingFrames);
            kr = failStart(kIOReturnBadArgument, "ActiveRingFramesMismatch");
            return;
        }

        const uint64_t reqOutBytes = static_cast<uint64_t>(expectedRingFrames) *
            ivars.runtime.directAudioGraph.memory.outputChannels * sizeof(float);
        const uint64_t reqInBytes = static_cast<uint64_t>(expectedRingFrames) *
            ivars.runtime.directAudioGraph.memory.inputChannels * sizeof(float);

        if (!ivars.outputMap || reqOutBytes > ivars.outputMap->GetLength() ||
            !ivars.inputMap || reqInBytes > ivars.inputMap->GetLength()) {
            ASFW_LOG_ERROR(Audio,
                           "ASFWAudioDevice: StartIO mapped bytes check failed reqOut=%llu mapOut=%llu reqIn=%llu mapIn=%llu",
                           reqOutBytes, ivars.outputMap ? ivars.outputMap->GetLength() : 0ULL,
                           reqInBytes, ivars.inputMap ? ivars.inputMap->GetLength() : 0ULL);
            kr = failStart(kIOReturnNoMemory, "MappedBytesCheckFailed");
            return;
        }

        if (GetZeroTimestampPeriod() != expectedZtsPeriod) {
            ASFW_LOG_ERROR(Audio,
                           "ASFWAudioDevice: StartIO ZTS period mismatch declared=%u expected=%u",
                           GetZeroTimestampPeriod(), expectedZtsPeriod);
            kr = failStart(kIOReturnBadArgument, "ZtsPeriodMismatch");
            return;
        }

        if (this->ivars &&
            this->ivars->configurationInFlightToken.load(std::memory_order_acquire) != 0) {
            ASFW_LOG_ERROR(Audio,
                           "ASFWAudioDevice: StartIO rejected with configuration change in flight token=%llu",
                           this->ivars->configurationInFlightToken.load(std::memory_order_relaxed));
            kr = failStart(kIOReturnBusy, "ConfigurationInFlight");
            return;
        }

        control->ResetForStart();
        ivars.runtime.txPlanBusTicksValid = false;
        ivars.runtime.lastTxPlanBusTicks = 0;
        ivars.runtime.txCorrelationUnwrap = {};
        ivars.runtime.txNoCycleAnchorEvents = 0;
        ivars.runtime.txNoPresentationOriginEvents = 0;
        ivars.runtime.txAlignmentDeltaFrames = 0;
        ivars.runtime.txAlignmentValid = false;
        ivars.runtime.txReplayResyncs = 0;
        ivars.runtime.mAudioPresentationObserver.Disarm();
        ivars.runtime.mAudioInternalTxTiming.Disarm();
        ivars.runtime.lastHalZeroTimestampGeneration.store(0, std::memory_order_release);
        ivars.runtime.lastHalZeroTimestampSampleFrame.store(0, std::memory_order_release);
        ivars.runtime.lastHalZeroTimestampHostTicks.store(0, std::memory_order_release);

        // --- Configure PCM publication cache and bind to session ---
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

            ivars.runtime.pcmPublicationCache.BindTelemetry(
                &control->pcmPublicationTelemetry);
            const uint32_t committedChannels =
                ivars.runtime.directAudioGraph.memory.outputChannels != 0
                    ? ivars.runtime.directAudioGraph.memory.outputChannels
                    : 2U;
            const uint32_t committedCapacity =
                ASFW::Audio::Shared::AudioTimingGeometry::
                    PcmPublicationCacheFrames(txConfig.sampleRate);
            if (ivars.runtime.pcmPublicationCache.ChannelCount() != committedChannels ||
                ivars.runtime.pcmPublicationCache.CacheCapacityFrames() != committedCapacity) {
                if (!ivars.runtime.pcmPublicationCache.Configure(
                        committedChannels, committedCapacity)) {
                    ASFW_LOG(
                        Audio,
                        "ASFWAudioDevice: PCM publication cache allocation failed channels=%u frames=%u",
                        committedChannels, committedCapacity);
                    kr = failStart(kIOReturnNoMemory, "ConfigurePcmPublicationCache");
                    return;
                }
            }
            const auto timelineSource =
                (useMAudioTxClock || ivars.device.inputChannelCount == 0)
                    ? ASFW::Audio::Runtime::HardwareTimelineSource::Transmit
                    : ASFW::Audio::Runtime::HardwareTimelineSource::Receive;
            // Distinguish joining an actively running session from starting a fresh one.
            // Adopt existing epoch only if the session is currently streaming, its epoch is valid,
            // and its active sample rate matches the requested rate.
            const bool isRunningSession =
                control->isSessionStreaming.load(std::memory_order_acquire) &&
                control->hardwareTimeline.Epoch() != 0 &&
                control->hardwareTimeline.SampleRateHz() == txConfig.sampleRate;
            uint64_t timelineEpoch = 0;
            if (isRunningSession) {
                timelineEpoch = control->hardwareTimeline.Epoch();
                ASFW_LOG(
                    DirectAudio,
                    "[TimelineEpoch] epoch=%llu reason=start-io-adopt source=%u rate=%u",
                    timelineEpoch, static_cast<uint32_t>(timelineSource),
                    txConfig.sampleRate);
            } else {
                timelineEpoch = control->hardwareTimeline.BeginEpoch(
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
            }
            ivars.runtime.pcmPublicationCache.BeginEpoch(timelineEpoch);

            control->rxTransferDelayTicks.store(
                profile->RxTransferDelayTicks(ivars.device.currentSampleRate),
                std::memory_order_relaxed);
            control->txTransferDelayTicks.store(
                profile->TxTransferDelayTicks(ivars.device.currentSampleRate),
                std::memory_order_relaxed);

            if (ivars.device.audioNub) {
                ivars.device.audioNub->SetTxPcmSource(&ivars.runtime.pcmPublicationCache);
            }
        }

        // --- Start hardware streaming ---
        ivars.runtime.txActive.store(true, std::memory_order_release);
        streamingStarted = true;
        const kern_return_t startKr =
            ivars.device.audioNub
                ? ivars.device.audioNub->StartAudioStreaming()
                : kIOReturnNotReady;
        if (startKr != kIOReturnSuccess) {
            if (ivars.device.audioNub) {
                ivars.device.audioNub->SetTxPcmSource(nullptr);
            }
            ASFW_LOG(Audio,
                     "ASFWAudioDevice: StartAudioStreaming failed: 0x%x",
                     startKr);
            kr = failStart(startKr, "StartAudioStreaming");
            return;
        }

        // AudioDriverKit needs a valid clock anchor when StartIO transitions
        // the device into the running state. The profile-owned hardware anchor
        // publisher runs on an independent RX or TX preparation queue, so it
        // can publish while this work queue waits. Generic BeBoB devices can
        // spend ~1 s in CIP NO-DATA after input starts (Linux
        // bebob_stream.c:661-666); the M-Audio path instead seeds from TX.
        //
        // Blocking here is the sanctioned behaviour, not a workaround, and it
        // reads like a bug to anyone arriving cold -- so: "This call is expected
        // to always succeed or fail. The hardware can take as long as necessary
        // in this call such that it always either succeeds ... or fails"
        // (AudioDriverKit IOUserAudioDevice.iig:182-183). There is no
        // asynchronous completion for StartIO: its return value is the contract.
        // Returning success before an anchor exists would hand the HAL a running
        // device with no clock, which is precisely what this wait prevents.
        //
        // Measured on an Apogee Duet: waitMs=133, i.e. about one
        // kHalZeroTimestampPeriodFrames (8192 frames = 170.67 ms at 48 kHz).
        // That period, not this loop, is what sets the cost -- shortening it is
        // the cheap lever if start latency ever needs attacking, at the price of
        // the HAL's maximum buffer size, which ADK derives from the same period.
        // A clock anchor alone is not readiness. Linux's DICE start waits after
        // GLOBAL_ENABLE for every stream to reach ready_processing and aborts
        // the start if it does not (dice-stream.c:457,
        // amdtp_domain_wait_ready, READY_TIMEOUT_MS = 200), and it derives the
        // host->device presentation-time sequence by replaying the device's own
        // rather than synthesising one -- amdtp_domain_start(d, 0,
        // replay_seq=true, replay_on_the_fly=false), whose MEMO notes that some
        // devices are strict about an invalid sequence of presentation time in
        // the CIP header.
        //
        // We had the anchor half of that and not the replay half: StartIO
        // returned as soon as any ZTS existed, which can be published while the
        // replay is still bootstrapping. The stream then runs with no
        // device-derived presentation time, which is consistent with an
        // observed Saffire cold start that transmitted correct audio the device
        // never presented, while a restart -- inheriting an established replay
        // -- played normally.
        //
        // RX-clocked endpoints only. A TX-clock-master profile (M-Audio,
        // Weiss) never establishes an RX replay by design, so requiring one
        // would hang its start.
        const bool requireRxReplay =
            control->hardwareTimeline.Source() ==
            ASFW::Audio::Runtime::HardwareTimelineSource::Receive;
        const auto replayEstablished = [&]() noexcept {
            return !requireRxReplay || control->rxSequenceReplay.IsEstablished();
        };

        uint32_t ztsWaitMs = 0;
        while (ztsWaitMs < initialClockAnchorTimeoutMs &&
               (ivars.runtime.lastHalZeroTimestampHostTicks.load(
                    std::memory_order_acquire) == 0 ||
                !replayEstablished())) {
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
        // Separate branch on purpose: "no clock at all" and "clock but no
        // device-derived sequence" need different fixes and must not collapse
        // into one timeout status.
        if (!replayEstablished()) {
            ASFW_LOG(
                Audio,
                "ASFWAudioDevice: RX sequence replay not established after %u ms "
                "(clock anchor present); refusing to start with a synthesised "
                "presentation-time sequence",
                ztsWaitMs);
            kr = failStart(kIOReturnTimeout, "WaitForRxSequenceReplay");
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
            ivars.runtime.directAudioGraph.memory.activeInputRingFrames,
            ivars.runtime.directAudioGraph.memory.activeOutputRingFrames,
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

    if (ivars.device.audioNub) ivars.device.audioNub->SetAudioIoRunning(
        kr == kIOReturnSuccess, static_cast<uint32_t>(ivars.device.currentSampleRate));
    return kr;
}

kern_return_t ASFWAudioDevice::StopIO(IOUserAudioStartStopFlags in_flags) {
    if (!ivars || !ivars->driverIvars) {
        ASFW_LOG(Audio, "ASFWAudioDevice: StopIO failed - no driver ivars");
        return kIOReturnNotReady;
    }

    const uint64_t inFlightToken = this->ivars->configurationInFlightToken.load(
        std::memory_order_acquire);
    if (this->ivars->driverIvars->device.audioNub) {
        const uint32_t window =
            ivars->driverIvars->device.audioNub->CurrentRuntimeTuningWindow();
        if (window != 0) {
            ASFW_LOG(Audio,
                     "[AudioTuning] win=%u IO STOPPING (this is the restart the "
                     "apply asked for)",
                     window);
        }
        ivars->driverIvars->device.audioNub->SetAudioIoRunning(false, 0);
    }
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
            ivars.device.audioNub->SetTxPcmSource(nullptr);
            const kern_return_t stopKr = ivars.device.audioNub->StopAudioStreaming();
            if (stopKr != kIOReturnSuccess) {
                ASFW_LOG(Audio, "ASFWAudioDevice: StopAudioStreaming failed: 0x%x", stopKr);
            }
        }
        ivars.runtime.pcmPublicationCache.BeginEpoch(0);

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
    uint32_t inputChannels, uint32_t outputChannels,
    bool inPerformConfigurationChange,
    const std::optional<ASFW::Audio::DriverKit::HalTimingProjection>& preResolvedProjection = std::nullopt) noexcept {
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
    std::optional<ASFW::Audio::DriverKit::HalTimingProjection> resolvedProjection;
    if (preResolvedProjection &&
        preResolvedProjection->resolvedGeometry.inputChannels == inputChannels &&
        preResolvedProjection->resolvedGeometry.outputChannels == outputChannels &&
        preResolvedProjection->resolvedGeometry.sampleRateHz == configuration.sampleRate) {
        resolvedProjection = preResolvedProjection;
    } else {
        const ASFW::Audio::Shared::DirectAudioAllocationLimits allocationLimits{
            .allocatedOutputBytes = driverIvars.outputMap ? driverIvars.outputMap->GetLength() : 0,
            .allocatedInputBytes = driverIvars.inputMap ? driverIvars.inputMap->GetLength() : 0,
            .maxOutputChannels = driverIvars.resolvedProfile.TxChannelCount(),
            .maxInputChannels = driverIvars.resolvedProfile.RxChannelCount(),
            .maxAllocatedFrames = ASFW::Audio::Shared::AudioTimingGeometry::kAllocatedFrameRingFrames,
        };
        uint64_t topologyRevision = 0;
        if (driverIvars.device.audioNub) {
            (void)driverIvars.device.audioNub->GetTopologyRevision(&topologyRevision);
        }
        resolvedProjection = DeriveHalTimingProjection(
            driverIvars.resolvedProfile, targetRate, inputChannels, outputChannels,
            /*tuningRequest=*/nullptr, &allocationLimits, topologyRevision);
    }
    if (!resolvedProjection) {
        ASFW_LOG_ERROR(Audio, "[AudioConfig] unsupported V3 safety rate=%.0f", targetRate);
        return kIOReturnUnsupported;
    }
    const auto& projection = *resolvedProjection;

    const uint32_t priorRate = static_cast<uint32_t>(device.GetSampleRate());
    kern_return_t kr = device.SetSampleRate(targetRate);
    if (kr != kIOReturnSuccess) return kr;

    if (device.GetZeroTimestampPeriod() != projection.zeroTimestampPeriodFrames) {
        if (inPerformConfigurationChange) {
            kr = device.SetZeroTimeStampPeriod(projection.zeroTimestampPeriodFrames);
            if (kr != kIOReturnSuccess) {
                ASFW_LOG_ERROR(
                    Audio,
                    "[AudioConfig] SetZeroTimeStampPeriod(%u) failed kr=0x%x",
                    projection.zeroTimestampPeriodFrames, kr);
                return kr;
            }
        } else {
            ASFW_LOG_ERROR(
                Audio,
                "[AudioConfig] ZTS period change (%u) requested outside perform window",
                projection.zeroTimestampPeriodFrames);
            return kIOReturnNotPermitted;
        }
    }
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

    if ((kr = device.SetOutputLatency(projection.outputLatencyFrames)) != kIOReturnSuccess) {
        return kr;
    }
    if ((kr = device.SetInputLatency(projection.inputLatencyFrames)) != kIOReturnSuccess) {
        return kr;
    }
    if ((kr = device.SetOutputSafetyOffset(projection.outputSafetyOffsetFrames)) != kIOReturnSuccess) {
        return kr;
    }
    if ((kr = device.SetInputSafetyOffset(projection.inputSafetyOffsetFrames)) != kIOReturnSuccess) {
        return kr;
    }

    std::array<IOUserAudioStreamBasicDescription,
               ASFW::Audio::Devices::kMaxConfigurationCapabilities> inputFormats{};
    std::array<IOUserAudioStreamBasicDescription,
               ASFW::Audio::Devices::kMaxConfigurationCapabilities> outputFormats{};
    uint32_t formatCount = 0;
    const auto& profileCapabilities = driverIvars.resolvedProfile.Value();
    for (uint8_t i = 0; i < profileCapabilities.configurationCapabilityCount; ++i) {
        const auto& candidate = profileCapabilities.configurationCapabilities[i];
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
            {.inputFrames = projection.resolvedGeometry.activeInputRingFrames,
             .outputFrames = projection.resolvedGeometry.activeOutputRingFrames,
              .inputChannels = inputChannels,
              .outputChannels = outputChannels})) {
        return kIOReturnError;
    }
    driverIvars.runtime.activeTuning.outputLatencyFrames = projection.outputLatencyFrames;
    driverIvars.runtime.activeTuning.inputLatencyFrames = projection.inputLatencyFrames;
    driverIvars.runtime.activeTuning.outputSafetyOffsetFrames = projection.outputSafetyOffsetFrames;
    driverIvars.runtime.activeTuning.inputSafetyOffsetFrames = projection.inputSafetyOffsetFrames;
    driverIvars.runtime.activeTuning.zeroTimestampPeriodFrames = projection.zeroTimestampPeriodFrames;
    driverIvars.runtime.activeTuning.frameRingFrames = projection.resolvedGeometry.activeOutputRingFrames;
    driverIvars.runtime.activeTuning.clientIoBudgetFrames = projection.resolvedGeometry.clientIoBudgetFrames;
    driverIvars.device.audioNub->PublishRuntimeTuningGraph(driverIvars.runtime.activeTuning,
        configuration.sampleRate, inputChannels, outputChannels);
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
        !std::holds_alternative<ASFW::Configuration::RequestADKWindowEffect>(transition.effects[0])) {
        return kr == kIOReturnSuccess ? kIOReturnError : kr;
    }
    const auto window = std::get<ASFW::Configuration::RequestADKWindowEffect>(transition.effects[0]);
    ASFW_LOG(Audio,
             "[AudioConfig] CoreAudio requesting ADK window endpoint=%llu token=%llu rate=%u",
             driverIvars.device.endpointId, window.identity.token, rateHz);
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
             "[AudioConfig] CoreAudio ADK window request result endpoint=%llu token=%llu kr=0x%x",
             driverIvars.device.endpointId, window.identity.token, kr);
    return kr;
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

kern_return_t ASFWAudioDevice::RequestRuntimeTuning(uint32_t requestId) {
    if (!ivars || !ivars->driverIvars || !ivars->driverIvars->device.audioNub)
        return kIOReturnNotReady;
    auto* nub = ivars->driverIvars->device.audioNub;
    if (!nub->BeginRuntimeTuningWindow(requestId)) return kIOReturnNotReady;
    const auto kr = RequestDeviceConfigurationChange(
        ASFW::Audio::Shared::TuningToken(requestId), nullptr);
    if (kr != kIOReturnSuccess) nub->CompleteRuntimeTuning(requestId, kr, false);
    ASFW_LOG(Audio, "[AudioTuning] request=%u window result=0x%x", requestId, kr);
    return kr;
}

kern_return_t ASFWAudioDevice::PerformDeviceConfigurationChange(
    uint64_t change_action, OSObject* in_change_info) {
    if (!ivars || !ivars->driverIvars) {
        return super::PerformDeviceConfigurationChange(change_action, in_change_info);
    }
    if (ASFW::Audio::DriverKit::IsZtsPeriodToken(change_action)) {
        const uint32_t newPeriod =
            ASFW::Audio::DriverKit::ZtsPeriodFromToken(change_action);
        const kern_return_t setKr = SetZeroTimeStampPeriod(newPeriod);
        if (setKr != kIOReturnSuccess) {
            ASFW_LOG_ERROR(
                Audio,
                "[AudioConfig] SetZeroTimeStampPeriod(%u) failed in Perform window kr=0x%x",
                newPeriod, setKr);
        } else {
            ASFW_LOG(
                Audio,
                "[AudioConfig] SetZeroTimeStampPeriod(%u) succeeded in Perform window",
                newPeriod);
        }
        const kern_return_t superKr =
            super::PerformDeviceConfigurationChange(change_action, in_change_info);
        return setKr != kIOReturnSuccess ? setKr : superKr;
    }
    // Checked before the configuration machine so the tuning path neither reads
    // nor advances it.
    if (ASFW::Audio::Shared::IsTuningToken(change_action)) {
        namespace Tuning = ASFW::Audio::Shared;
        auto& driver = *ivars->driverIvars;
        auto* nub = driver.device.audioNub;
        if (!nub) return kIOReturnNotReady;
        const auto id = static_cast<uint32_t>(change_action);
        const auto rate = static_cast<uint32_t>(driver.device.currentSampleRate);
        // The window exists to be entered with IO stopped -- StartIO is what
        // reads the geometry this apply changes. Still hand the window back to
        // the host on the way out: every other early return in this function
        // calls super, and leaving one unbalanced desynchronises the ADK's own
        // configuration bookkeeping.
        if (driver.runtime.isRunning.load(std::memory_order_acquire)) {
            ASFW_LOG_ERROR(Audio,
                           "[AudioTuning] request=%u WINDOW GRANTED while IO is "
                           "still running -- refused, geometry unchanged",
                           id);
            nub->CompleteRuntimeTuning(id, kIOReturnBusy, false);
            (void)super::PerformDeviceConfigurationChange(change_action, in_change_info);
            return kIOReturnBusy;
        }
        // Mutate first, release the host last. super:: is the commit point that
        // lets the HAL restart IO, and StartIO reads runtime.activeTuning --
        // committing after it would put the new geometry in force one restart
        // late while the panel already read back "Applied". This is the same
        // ordering the rate/optical path below uses.
        const auto previous = driver.runtime.activeTuning;
        const bool applied = nub->CommitRuntimeTuning(id, driver.runtime.activeTuning);
        if (!applied) {
            // A duplicate or late callback for a request that is no longer the
            // one in flight. Nothing was mutated; do not restart on a lie.
            ASFW_LOG_ERROR(Audio,
                           "[AudioTuning] request=%u WINDOW GRANTED with no "
                           "matching in-flight request -- geometry unchanged",
                           id);
            (void)super::PerformDeviceConfigurationChange(change_action, in_change_info);
            return kIOReturnNotReady;
        }
        const auto& active = driver.runtime.activeTuning;
        const auto outcome = Tuning::ValidateTuning(active);
        ASFW_LOG(Audio,
                 "[AudioTuning] request=%u APPLYING warnings=0x%x slack=%u->%u "
                 "target=%u->%u lead=%u->%u fr @%u Hz",
                 id, outcome.warnings, previous.txDispatchSlackPackets,
                 active.txDispatchSlackPackets, previous.PreparedTargetPackets(),
                 active.PreparedTargetPackets(),
                 Tuning::PreparedLeadFrames(previous, rate),
                 Tuning::PreparedLeadFrames(active, rate), rate);
        if (outcome.Has(Tuning::TuningWarning::kDispatchSlackBelowAssertedFloor)) {
            ASFW_LOG(Audio,
                     "[AudioTuning] request=%u dispatch slack %u packets is BELOW "
                     "the asserted floor of %u -- a coalesced completion delta "
                     "larger than this holes the descriptor ring",
                     id, active.txDispatchSlackPackets,
                     12U * Tuning::AudioTimingGeometry::kTxPacketsPerGroup);
        }
        const auto superKr =
            super::PerformDeviceConfigurationChange(change_action, in_change_info);
        if (superKr != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Audio,
                           "[AudioTuning] request=%u host refused the completed "
                           "window kr=0x%x -- geometry is installed, the restart "
                           "is not",
                           id, superKr);
        }
        return superKr;
    }
    if (!ivars->configurationLock || !ivars->configurationEnabled) {
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
    const auto* candidateCap = driverIvars.resolvedProfile.Value().ConfigurationFor(apply.transition.candidate);
    if (!driverIvars.device.audioNub || !candidateCap ||
        !ASFW::Audio::Shared::AudioTimingGeometry::IsV3SampleRate(apply.transition.candidate.sampleRate) ||
        !ASFW::Encoding::AmdtpRateGeometryForSampleRate(apply.transition.candidate.sampleRate).has_value()) {
        (void)dispatch(ASFW::Configuration::HardwareCompleted{
            .identity = apply.transition.identity,
            .outcome = ASFW::Configuration::HardwareUnknown{},
        });
        (void)super::PerformDeviceConfigurationChange(change_action, in_change_info);
        uint64_t expected = identity.token;
        (void)ivars->configurationInFlightToken.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel);
        return kIOReturnBadArgument;
    }

    const uint32_t candInChannels = candidateCap->runtimeCaps.hostInputPcmChannels;
    const uint32_t candOutChannels = candidateCap->runtimeCaps.hostOutputPcmChannels;
    const double candTargetRate = static_cast<double>(apply.transition.candidate.sampleRate);
    const ASFW::Audio::Shared::DirectAudioAllocationLimits allocationLimits{
        .allocatedOutputBytes = driverIvars.outputMap ? driverIvars.outputMap->GetLength() : 0,
        .allocatedInputBytes = driverIvars.inputMap ? driverIvars.inputMap->GetLength() : 0,
        .maxOutputChannels = driverIvars.resolvedProfile.TxChannelCount(),
        .maxInputChannels = driverIvars.resolvedProfile.RxChannelCount(),
        .maxAllocatedFrames = ASFW::Audio::Shared::AudioTimingGeometry::kAllocatedFrameRingFrames,
    };
    uint64_t topologyRevision = 0;
    if (driverIvars.device.audioNub) {
        (void)driverIvars.device.audioNub->GetTopologyRevision(&topologyRevision);
    }
    const auto candidateProjection = DeriveHalTimingProjection(
        driverIvars.resolvedProfile, candTargetRate, candInChannels, candOutChannels,
        /*tuningRequest=*/nullptr, &allocationLimits, topologyRevision);
    if (!candidateProjection) {
        ASFW_LOG_ERROR(Audio,
                       "[AudioConfig] candidate geometry rejected before hardware apply endpoint=%llu token=%llu rate=%u",
                       driverIvars.device.endpointId, identity.token,
                       static_cast<uint32_t>(apply.transition.candidate.sampleRate));
        (void)dispatch(ASFW::Configuration::HardwareCompleted{
            .identity = apply.transition.identity,
            .outcome = ASFW::Configuration::HardwareUnknown{},
        });
        (void)super::PerformDeviceConfigurationChange(change_action, in_change_info);
        uint64_t expected = identity.token;
        (void)ivars->configurationInFlightToken.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel);
        return kIOReturnUnsupported;
    }

    const uint32_t candCacheFrames = candidateProjection->resolvedGeometry.pcmCacheCapacityFrames;
    auto stagedStorage = ASFW::Audio::Runtime::PcmPublicationCache::AllocateStorage(
        candOutChannels != 0 ? candOutChannels : 2U, candCacheFrames);
    if (!stagedStorage) {
        ASFW_LOG_ERROR(Audio,
                       "[AudioConfig] Failed to allocate staged PCM cache storage ch=%u frames=%u",
                       candOutChannels, candCacheFrames);
        (void)dispatch(ASFW::Configuration::HardwareCompleted{
            .identity = apply.transition.identity,
            .outcome = ASFW::Configuration::HardwareUnknown{},
        });
        (void)super::PerformDeviceConfigurationChange(change_action, in_change_info);
        uint64_t expected = identity.token;
        (void)ivars->configurationInFlightToken.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel);
        return kIOReturnNoMemory;
    }

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
        inputChannels, outputChannels, /*inPerformConfigurationChange=*/true,
        candidateProjection);
    const kern_return_t runtimeKr = mutation == kIOReturnSuccess
        ? driverIvars.device.audioNub->CommitDeviceConfiguration(
              project.plan.confirmed.configuration.sampleRate,
              OpticalModeWire(project.plan.confirmed.configuration.opticalInput),
              OpticalModeWire(project.plan.confirmed.configuration.opticalOutput))
        : mutation;
    if (mutation == kIOReturnSuccess && runtimeKr == kIOReturnSuccess && stagedStorage) {
        driverIvars.runtime.pcmPublicationCache.CommitStorage(std::move(*stagedStorage));
    }
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
    if (ASFW::Audio::DriverKit::IsZtsPeriodToken(change_action)) {
        ASFW_LOG_ERROR(
            Audio,
            "[AudioConfig] SetZeroTimeStampPeriod(%u) aborted by host",
            ASFW::Audio::DriverKit::ZtsPeriodFromToken(change_action));
        return super::AbortDeviceConfigurationChange(change_action, in_change_info);
    }
    if (ASFW::Audio::Shared::IsTuningToken(change_action)) {
        if (ivars && ivars->driverIvars && ivars->driverIvars->device.audioNub)
            ivars->driverIvars->device.audioNub->CompleteRuntimeTuning(
                static_cast<uint32_t>(change_action), kIOReturnAborted, true);
        return super::AbortDeviceConfigurationChange(change_action, in_change_info);
    }
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
