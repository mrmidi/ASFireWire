// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "AudioCoordinator.hpp"

#include "../Shared/AudioGeometryReport.hpp"
#include "../Shared/AudioRuntimeTuningStore.hpp"

#include "AudioEndpointRuntime.hpp"
#include "AudioRuntimeRegistry.hpp"
#include "../Duplex/SyncAsyncBridge.hpp"
#include "../Protocols/IDeviceProtocol.hpp"
#include <net.mrmidi.ASFW.ASFWDriver/ASFWAudioNub.h>
#include <net.mrmidi.ASFW.ASFWDriver/ASFWMidiNub.h>
#include "../../Logging/Logging.hpp"

#include <utility>

namespace ASFW::Audio {

AudioCoordinator::AudioCoordinator(IOService* driver,
                                   Discovery::DeviceRegistry& registry,
                                   AudioRuntimeRegistry& runtime,
                                   Driver::IsochService& isoch,
                                   Driver::HardwareInterface& hardware) noexcept
    : publisher_(driver)
    , midiPublisher_(driver)
    , runtime_(runtime)
    , hostTransport_(isoch)
    , duplexCoordinator_(
          registry, runtime_, hostTransport_, hardware, &teardownRequested_,
          [this](EndpointId endpointId)
              -> Runtime::IDirectAudioBindingSource* {
              auto endpoint = runtime_.FindEndpointRuntime(endpointId);
              return endpoint ? endpoint.get() : nullptr;
          }) {
    lock_ = IOLockAlloc();
    if (!lock_) {
        ASFW_LOG_ERROR(Audio, "AudioCoordinator: lock allocation failed");
    }
    hostTransport_.SetTimingLossCallback(
        [this](EndpointId endpointId) {
            HandleHostTimingLoss(endpointId);
        });
    hostTransport_.SetTxTransportFaultCallback(
        [this](EndpointId endpointId, uint32_t statusRaw,
               uint64_t streamGeneration) {
            HandleTxTransportFault(endpointId, statusRaw, streamGeneration);
        });
}

AudioCoordinator::~AudioCoordinator() noexcept {
    BeginTeardown();
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

void AudioCoordinator::SetTxPreparationCallback(
    Driver::IsochService::TxPreparationCallback callback) noexcept {
    hostTransport_.SetTxPreparationCallback(std::move(callback));
}

void AudioCoordinator::SetClockAnchorReadyCallback(
    IsochDuplexHostTransport::ClockAnchorReadyCallback callback) noexcept {
    hostTransport_.SetClockAnchorReadyCallback(std::move(callback));
}

void AudioCoordinator::SetSessionStreamingCallback(
    SessionStreamingCallback callback) noexcept {
    if (!lock_) return;
    IOLockLock(lock_);
    sessionStreamingCallback_ = std::move(callback);
    IOLockUnlock(lock_);
}

void AudioCoordinator::EndpointReady(
    std::shared_ptr<const Devices::ResolvedAudioEndpointProfile> profile,
    std::shared_ptr<IDeviceProtocol> protocolHold) noexcept {
    if (!profile || !profile->endpointId || teardownRequested_.load()) return;

    auto runtime = runtime_.InsertResolved(profile, std::move(protocolHold));
    if (!runtime) {
        ASFW_LOG_ERROR(Audio,
                       "[AudioSession] endpoint=%llu runtime installation failed",
                       profile->endpointId.value);
        return;
    }
    duplexCoordinator_.AcknowledgeDevicePresent(profile->endpointId);
    duplexCoordinator_.SynchronizeCommittedConfiguration(
        profile->endpointId,
        AudioClockConfig{.sampleRateHz = profile->currentSampleRateHz},
        profile->runtimeCaps,
        runtime->CopyTopologyRevision());
    if (lock_) {
        IOLockLock(lock_);
        invalidatedEndpoints_.erase(profile->endpointId);
        IOLockUnlock(lock_);
    }

    if (!publisher_.EnsureNub(*profile, "resolved-endpoint")) {
        runtime_.Remove(profile->endpointId);
        duplexCoordinator_.CancelRemoteDevice(profile->endpointId);
        ASFW_LOG_ERROR(Audio,
                       "[AudioSession] endpoint=%llu nub publication failed",
                       profile->endpointId.value);
        return;
    }
    // MIDI is projected from the same wire geometry the audio profile already
    // carries, and published as its own nub. A failure here is not allowed to
    // unwind the audio endpoint: an interface whose MIDI cannot be published is
    // still a working audio device, and tearing down the audio nub over it
    // would turn a missing DIN socket into silence.
    const auto midiCaps = ASFW::Midi::ProjectMidiCapabilities(
        profile->runtimeCaps, profile->observedGuid,
        runtime->CopyTopologyRevision());
    if (!midiPublisher_.EnsureNub(midiCaps, profile->endpointId.value,
                                  profile->deviceName.c_str(),
                                  profile->deviceName.c_str(),
                                  profile->vendorName.empty()
                                      ? "ASFireWire"
                                      : profile->vendorName.c_str())) {
        ASFW_LOG_ERROR(Midi,
                       "[AudioSession] endpoint=%llu MIDI nub publication failed; "
                       "audio endpoint stays up",
                       profile->endpointId.value);
    } else if (midiCaps.deviceToHost.Usable()) {
        // Point receive extraction at this endpoint's rings. Arm publishes the
        // epoch and clears the rings before anything can read them, so a
        // restart cannot deliver the previous stream's bytes.
        auto* nub = reinterpret_cast<ASFWMidiNub*>(
            midiPublisher_.GetNub(profile->endpointId.value));
        if (nub != nullptr) {
            nub->ArmTransport(midiCaps.streamEpoch);
            auto* block = static_cast<ASFW::Midi::MidiTransportBlock*>(
                nub->GetTransportBlock());
            if (block != nullptr) {
                const auto& source = midiCaps.deviceToHost;
                hostTransport_.SetMidiReceiveTransport(
                    block, midiCaps.streamEpoch,
                    ASFW::Encoding::MpxMidiGeometry{
                        .dbs = source.dbs,
                        .midiSlotIndex = source.midiSlotIndex,
                        .portCount = source.portCount,
                        .dbcAligned = source.dbcAligned,
                    },
                    [nub] { (void)nub->NotifyMidiReceived(); });
                ASFW_LOG(Midi,
                         "[AudioSession] endpoint=%llu MIDI receive armed "
                         "epoch=%llu ports=%u slot=%u dbs=%u",
                         profile->endpointId.value, midiCaps.streamEpoch,
                         source.portCount, source.midiSlotIndex, source.dbs);
            }
        }
    }

    ASFW_LOG(Audio,
             "[AudioSession] endpoint=%llu provider=%u published instance=%llu observedGUID=%llx",
             profile->endpointId.value,
             static_cast<unsigned>(profile->familyProvider),
             profile->deviceInstanceId.value,
             profile->observedGuid);
}

void AudioCoordinator::QuiesceEndpoint(EndpointId endpointId) noexcept {
    if (!endpointId) return;
    bool wasActive = false;
    if (lock_) {
        IOLockLock(lock_);
        wasActive = activeEndpoint_ == endpointId;
        if (wasActive) activeEndpoint_ = {};
        IOLockUnlock(lock_);
    }
    if (wasActive) {
        const IOReturn status = duplexCoordinator_.StopStreaming(endpointId);
        if (status != kIOReturnSuccess && status != kIOReturnNoDevice &&
            status != kIOReturnNotReady) {
            ASFW_LOG_WARNING(Audio,
                             "[AudioSession] endpoint=%llu quiesce status=0x%x",
                             endpointId.value, status);
        }
        (void)StopHostTransport("endpoint-quiesce", true);
    }
    if (auto endpoint = runtime_.FindEndpointRuntime(endpointId)) {
        endpoint->MarkStreaming(false);
    }
    (void)NotifySessionStreaming(endpointId, false);
}

void AudioCoordinator::InvalidateEndpointBindings(EndpointId endpointId) noexcept {
    if (!endpointId) return;
    duplexCoordinator_.CancelRemoteDevice(endpointId);
    runtime_.Remove(endpointId);
    if (lock_) {
        IOLockLock(lock_);
        invalidatedEndpoints_.insert(endpointId);
        IOLockUnlock(lock_);
    }
}

void AudioCoordinator::TerminateEndpoint(EndpointId endpointId) noexcept {
    // MIDI first: its service holds no hardware, so dropping it before the
    // audio nub keeps the teardown order monotonic from the outside in. Both
    // are idempotent for an endpoint that never published one.
    //
    // Detach receive extraction before the nub goes away: the sink borrows the
    // nub's rings, so the borrow has to end first.
    hostTransport_.SetMidiReceiveTransport(nullptr, 0, {}, {});
    midiPublisher_.TerminateNub(endpointId.value);
    publisher_.TerminateNub(endpointId, "session-retired");
    duplexCoordinator_.ClearSession(endpointId);
}

IOReturn AudioCoordinator::StartStreaming(EndpointId endpointId) noexcept {
    if (!endpointId || teardownRequested_.load(std::memory_order_acquire)) {
        return kIOReturnNotReady;
    }
    if (!runtime_.FindProfile(endpointId)) return kIOReturnNoDevice;

    bool alreadyActive = false;
    if (lock_) {
        IOLockLock(lock_);
        if (invalidatedEndpoints_.contains(endpointId)) {
            IOLockUnlock(lock_);
            return kIOReturnNoDevice;
        }
        if (!activeEndpoint_) {
            activeEndpoint_ = endpointId;
        } else if (activeEndpoint_ == endpointId) {
            alreadyActive = true;
        } else {
            IOLockUnlock(lock_);
            return kIOReturnBusy;
        }
        IOLockUnlock(lock_);
    }

    if (alreadyActive) {
        return NotifySessionStreaming(endpointId, true)
            ? kIOReturnSuccess
            : kIOReturnNoDevice;
    }

    const IOReturn status = duplexCoordinator_.StartStreaming(endpointId);
    if (status != kIOReturnSuccess) {
        if (lock_) {
            IOLockLock(lock_);
            if (activeEndpoint_ == endpointId) activeEndpoint_ = {};
            IOLockUnlock(lock_);
        }
        return status;
    }
    if (auto endpoint = runtime_.FindEndpointRuntime(endpointId)) {
        endpoint->MarkStreaming(true);
    }
    if (!NotifySessionStreaming(endpointId, true)) {
        (void)duplexCoordinator_.StopStreaming(endpointId);
        if (auto endpoint = runtime_.FindEndpointRuntime(endpointId)) {
            endpoint->MarkStreaming(false);
        }
        if (lock_) {
            IOLockLock(lock_);
            if (activeEndpoint_ == endpointId) activeEndpoint_ = {};
            IOLockUnlock(lock_);
        }
        return kIOReturnNoDevice;
    }
    return kIOReturnSuccess;
}

IOReturn AudioCoordinator::StopStreaming(EndpointId endpointId) noexcept {
    if (!endpointId) return kIOReturnBadArgument;
    if (lock_) {
        IOLockLock(lock_);
        if (invalidatedEndpoints_.contains(endpointId)) {
            IOLockUnlock(lock_);
            return kIOReturnSuccess;
        }
        if (activeEndpoint_ && activeEndpoint_ != endpointId) {
            IOLockUnlock(lock_);
            return kIOReturnBusy;
        }
        IOLockUnlock(lock_);
    }
    const IOReturn status = duplexCoordinator_.StopStreaming(endpointId);
    if (status == kIOReturnSuccess && lock_) {
        IOLockLock(lock_);
        if (activeEndpoint_ == endpointId) activeEndpoint_ = {};
        IOLockUnlock(lock_);
    }
    if (status == kIOReturnSuccess) {
        if (auto endpoint = runtime_.FindEndpointRuntime(endpointId)) {
            endpoint->MarkStreaming(false);
        }
        (void)NotifySessionStreaming(endpointId, false);
    }
    return status;
}

IOReturn AudioCoordinator::RequestClockConfig(
    EndpointId endpointId,
    const AudioClockConfig& desiredClock,
    DuplexRestartReason reason) noexcept {
    if (!endpointId) return kIOReturnBadArgument;
    const auto profile = runtime_.FindProfile(endpointId);
    if (!profile) return kIOReturnNoDevice;
    bool supported = false;
    for (uint8_t i = 0; i < profile->supportedRateCount; ++i) {
        supported |= profile->supportedRates[i] == desiredClock.sampleRateHz;
    }
    if (!supported) return kIOReturnUnsupported;

    const IOReturn status = duplexCoordinator_.RequestClockConfig(
        endpointId, desiredClock, reason);
    if (status == kIOReturnSuccess) {
        if (auto endpoint = runtime_.FindEndpointRuntime(endpointId)) {
            endpoint->SetCurrentSampleRate(desiredClock.sampleRateHz);
        }
    }
    return status;
}

IOReturn AudioCoordinator::ApplyDeviceConfiguration(
    EndpointId endpointId,
    const Configuration::DeviceConfiguration& desired,
    AudioConfigurationApplyResult& outResult) noexcept {
    outResult = {};
    if (!endpointId || teardownRequested_.load(std::memory_order_acquire)) {
        return kIOReturnNotReady;
    }
    const auto profile = runtime_.FindProfile(endpointId);
    const auto protocol = runtime_.FindShared(endpointId);
    if (!profile || !protocol) {
        return kIOReturnNoDevice;
    }
    const auto* capability = profile->ConfigurationFor(desired);
    auto* control = protocol->AsAudioConfigurationControl();
    if (!capability || !control || !control->SupportsConfiguration(desired)) {
        ASFW_LOG(Audio,
                 "[AudioConfig] endpoint=%llu rejected unsupported rate=%u opticalIn=%u opticalOut=%u",
                 endpointId.value, desired.sampleRate,
                 desired.opticalInput ? static_cast<unsigned>(*desired.opticalInput) : 0U,
                 desired.opticalOutput ? static_cast<unsigned>(*desired.opticalOutput) : 0U);
        return kIOReturnUnsupported;
    }

    // The FCP completion is delivered independently from the nub dispatch
    // queue. The bridge is bounded and deliberately mirrors the existing
    // clock-config synchronization policy; no ADK object is touched here.
    const auto completed = WaitForAsyncResult<AudioConfigurationApplyResult>(
        [control, desired](IAudioConfigurationControl::ApplyCallback callback) {
            control->ApplyConfiguration(desired, std::move(callback));
        },
        profile->clockPolicy.lockTimeoutMs + 2'000U,
        kIOReturnTimeout,
        &teardownRequested_);
    if (completed.status != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio,
                       "[AudioConfig] endpoint=%llu hardware apply failed kr=0x%x",
                       endpointId.value, completed.status);
        return completed.status;
    }
    const auto* confirmedCapability =
        profile->ConfigurationFor(completed.value.configuration);
    if (!confirmedCapability ||
        confirmedCapability->runtimeCaps.hostInputPcmChannels !=
            completed.value.runtimeCaps.hostInputPcmChannels ||
        confirmedCapability->runtimeCaps.hostOutputPcmChannels !=
            completed.value.runtimeCaps.hostOutputPcmChannels) {
        ASFW_LOG_ERROR(Audio,
                       "[AudioConfig] endpoint=%llu hardware returned unadvertised topology",
                       endpointId.value);
        return kIOReturnError;
    }
    outResult = completed.value;
    return kIOReturnSuccess;
}

IOReturn AudioCoordinator::CommitDeviceConfiguration(
    EndpointId endpointId,
    const AudioConfigurationApplyResult& confirmed) noexcept {
    if (!endpointId || teardownRequested_.load(std::memory_order_acquire)) {
        return kIOReturnNotReady;
    }
    const auto profile = runtime_.FindProfile(endpointId);
    const auto endpoint = runtime_.FindEndpointRuntime(endpointId);
    const auto* capability = profile ? profile->ConfigurationFor(confirmed.configuration)
                                     : nullptr;
    if (!endpoint || !capability ||
        capability->runtimeCaps.hostInputPcmChannels !=
            confirmed.runtimeCaps.hostInputPcmChannels ||
        capability->runtimeCaps.hostOutputPcmChannels !=
            confirmed.runtimeCaps.hostOutputPcmChannels) {
        return kIOReturnBadArgument;
    }
    if (!endpoint->ApplyConfiguration(confirmed.runtimeCaps)) {
        return kIOReturnError;
    }
    duplexCoordinator_.SynchronizeCommittedConfiguration(
        endpointId,
        AudioClockConfig{.sampleRateHz = confirmed.configuration.sampleRate},
        confirmed.runtimeCaps,
        endpoint->CopyTopologyRevision());
    return kIOReturnSuccess;
}

IOReturn AudioCoordinator::RequestRuntimeTuning(
    EndpointId endpointId, const Shared::AudioRuntimeTuning& candidate,
    uint32_t groups) noexcept {
    if (!endpointId || teardownRequested_.load(std::memory_order_acquire)) {
        return kIOReturnNotReady;
    }
    // A mask of zero is "apply nothing", which must not restart the streams.
    // Reported as success so a panel submitting an unchanged form is not shown
    // an error for doing nothing.
    if (groups == 0) {
        ASFW_LOG(Audio,
                 "[AudioTuning] endpoint=%llu request with an empty group mask -- "
                 "nothing to apply, streams left running",
                 endpointId.value);
        return kIOReturnSuccess;
    }
    auto* nub = publisher_.GetNub(endpointId);
    if (!nub) return kIOReturnNoDevice;
    const auto kr = nub->NotifyRuntimeTuningRequested(candidate, groups);
    if (kr != kIOReturnSuccess) {
        // Silent here would leave the operator with an app-side error code and
        // no record of which request the driver turned away, or why.
        ASFW_LOG_ERROR(Audio,
                       "[AudioTuning] endpoint=%llu REFUSED kr=0x%x groups=0x%x "
                       "slack=%u guard=%u -- request never reached the audio side",
                       endpointId.value, kr, groups,
                       candidate.txDispatchSlackPackets,
                       candidate.txOwnershipGuardPackets);
    }
    return kr;
}

IOReturn AudioCoordinator::CopyRuntimeTuningSnapshot(
    EndpointId endpointId,
    UserClient::Wire::AudioTuningSnapshotWire& out) noexcept {
    if (!endpointId) return kIOReturnBadArgument;
    auto* nub = publisher_.GetNub(endpointId);
    if (!nub) return kIOReturnNoDevice;

    Shared::RuntimeTuningSnapshot snapshot{};
    // A not-ready endpoint still reports: `ready` and the request state are
    // what the panel needs in order to say why it is not showing live geometry.
    if (!nub->CopyRuntimeTuningSnapshot(snapshot)) return kIOReturnNotReady;
    const auto& active = snapshot.effective;

    out = {};
    out.endpointId = endpointId.value;
    out.txDispatchSlackPackets = active.txDispatchSlackPackets;
    out.txOwnershipGuardPackets = active.txOwnershipGuardPackets;
    out.preparedTargetPackets = active.PreparedTargetPackets();
    out.preparedLeadFrames = Shared::PreparedLeadFrames(active, snapshot.sampleRateHz);
    out.outputLatencyFrames = active.outputLatencyFrames;
    out.inputLatencyFrames = active.inputLatencyFrames;
    out.outputSafetyOffsetFrames = active.outputSafetyOffsetFrames;
    out.inputSafetyOffsetFrames = active.inputSafetyOffsetFrames;
    out.frameRingFrames = active.frameRingFrames;
    out.clientIoBudgetFrames = active.clientIoBudgetFrames;
    out.zeroTimestampPeriodFrames = active.zeroTimestampPeriodFrames;
    out.sampleRateHz = snapshot.sampleRateHz;
    out.txPacketsPerGroup = Shared::AudioTimingGeometry::kTxPacketsPerGroup;
    out.txHardwareRingPackets =
        Shared::AudioTimingGeometry::kTxHardwareRingPackets;
    out.txSharedSlotPackets = Shared::AudioTimingGeometry::kTxSharedSlotPackets;
    out.inputChannels = snapshot.inputChannels;
    out.outputChannels = snapshot.outputChannels;
    out.lastError = snapshot.lastError;
    out.lastWarnings = snapshot.warnings;
    out.appliedSequence = snapshot.appliedSequence;
    out.streaming = snapshot.streaming ? 1U : 0U;
    out.pendingGroups = snapshot.pendingGroups;
    out.requestId = snapshot.requestId;
    out.requestStatus = static_cast<uint32_t>(snapshot.status);
    out.supportedGroups = Shared::kSupportedTuningGroups;
    out.ready = snapshot.ready ? 1U : 0U;

    // Read-only geometry. Derived in Audio/Shared so the arithmetic the
    // operator reads is covered by the host suite; this is a mechanical copy.
    const auto geometry = Shared::DeriveGeometryReport(snapshot.sampleRateHz);
    out.framesPerDataPacket = geometry.framesPerDataPacket;
    out.txDispatchSlackFloorPackets = geometry.txDispatchSlackFloorPackets;
    out.txDispatchSlackDefaultPackets = geometry.txDispatchSlackDefaultPackets;
    out.isochCyclesPerSecond = geometry.isochCyclesPerSecond;
    out.microsecondsPerIsochCycle = geometry.microsecondsPerIsochCycle;
    out.rxPacketsPerGroup = geometry.rxPacketsPerGroup;
    out.rxHardwareRingPackets = geometry.rxHardwareRingPackets;
    out.txInterruptIntervalMicroseconds =
        geometry.txInterruptIntervalMicroseconds;
    out.rxInterruptIntervalMicroseconds =
        geometry.rxInterruptIntervalMicroseconds;
    out.framesPerCompletionGroupTx = geometry.framesPerCompletionGroupTx;
    out.framesPerCompletionGroupRx = geometry.framesPerCompletionGroupRx;
    out.minFramesPerRxInterrupt = geometry.minFramesPerRxInterrupt;
    out.maxFramesPerRxInterrupt = geometry.maxFramesPerRxInterrupt;
    out.cadenceBlockPackets = geometry.cadenceBlockPackets;
    out.cadenceBlockFrames = geometry.cadenceBlockFrames;
    out.txContentFreezePackets = geometry.txContentFreezePackets;
    out.txRepointGuardPackets = geometry.txRepointGuardPackets;
    out.schedulingJitterFrames = geometry.schedulingJitterFrames;
    out.frameAlignmentFrames = geometry.frameAlignmentFrames;
    out.pcmPublicationCacheFrames = geometry.pcmPublicationCacheFrames;
    out.maxBlockingFramesPerDataPacket =
        geometry.maxBlockingFramesPerDataPacket;
    out.txSafetyOffsetPolicyFrames = geometry.txSafetyOffsetPolicyFrames;
    out.rxSafetyOffsetPolicyFrames = geometry.rxSafetyOffsetPolicyFrames;
    out.reportedLatencyPolicyFrames = geometry.reportedLatencyPolicyFrames;
    out.completionBatchFrames = geometry.completionBatchFrames;
    out.txRingLapFrames = geometry.txRingLapFrames;
    return kIOReturnSuccess;
}

IOReturn AudioCoordinator::RequestDeviceConfiguration(
    EndpointId endpointId,
    const Configuration::DeviceConfiguration& desired) noexcept {
    if (!endpointId || teardownRequested_.load(std::memory_order_acquire)) {
        return kIOReturnNotReady;
    }
    const auto profile = runtime_.FindProfile(endpointId);
    auto* nub = publisher_.GetNub(endpointId);
    if (!profile || !nub) return kIOReturnNoDevice;
    if (!profile->ConfigurationFor(desired)) {
        return kIOReturnUnsupported;
    }
    const uint32_t input = !desired.opticalInput ? 0U :
        (*desired.opticalInput == Configuration::OpticalMode::Adat ? 1U : 2U);
    const uint32_t output = !desired.opticalOutput ? 0U :
        (*desired.opticalOutput == Configuration::OpticalMode::Adat ? 1U : 2U);
    return nub->NotifyDeviceConfigurationRequested(desired.sampleRate, input, output)
        ? kIOReturnSuccess : kIOReturnNotReady;
}

IOReturn AudioCoordinator::CopyDeviceConfigurationSnapshot(
    EndpointId endpointId,
    Configuration::DeviceConfigurationSnapshot& outSnapshot) noexcept {
    outSnapshot = {};
    const auto profile = runtime_.FindProfile(endpointId);
    const auto endpoint = runtime_.FindEndpointRuntime(endpointId);
    if (!profile || !endpoint) return kIOReturnNoDevice;

    uint32_t sampleRateHz = 0;
    uint32_t inputChannels = 0;
    uint32_t outputChannels = 0;
    uint64_t topologyRevision = 0;
    if (!endpoint->CopyActiveConfiguration(sampleRateHz, inputChannels, outputChannels,
                                           topologyRevision)) {
        return kIOReturnNotReady;
    }
    outSnapshot.endpointId = endpointId.value;
    outSnapshot.topologyRevision = topologyRevision;
    outSnapshot.inputChannels = inputChannels;
    outSnapshot.outputChannels = outputChannels;
    const uint8_t count = std::min(
        profile->configurationCapabilityCount,
        static_cast<uint8_t>(outSnapshot.capabilities.size()));
    bool foundCommitted = false;
    for (uint8_t i = 0; i < count; ++i) {
        const auto& source = profile->configurationCapabilities[i];
        auto& destination = outSnapshot.capabilities[i];
        destination.configuration = source.configuration;
        destination.inputChannels = source.runtimeCaps.hostInputPcmChannels;
        destination.outputChannels = source.runtimeCaps.hostOutputPcmChannels;
        if (source.configuration.sampleRate == sampleRateHz &&
            destination.inputChannels == inputChannels &&
            destination.outputChannels == outputChannels) {
            outSnapshot.committed = source.configuration;
            foundCommitted = true;
        }
    }
    outSnapshot.capabilityCount = count;
    return foundCommitted ? kIOReturnSuccess : kIOReturnError;
}

uint32_t AudioCoordinator::CopyRuntimeTuningEndpointIds(
    std::array<EndpointId, Shared::kMaxAudioRuntimeTuningEndpoints>& out) noexcept {
    return runtime_.CopyRuntimeTuningEndpointIds(out);
}

uint32_t AudioCoordinator::CopyConfigurationEndpointIds(
    std::array<EndpointId,
               Configuration::kMaxConfigurationSnapshotCapabilities>& out) noexcept {
    return runtime_.CopyConfigurationEndpointIds(out);
}

uint32_t AudioCoordinator::CopySemanticTopologyEndpointIds(
    std::array<EndpointId, kMaxAudioSemanticTopologyEndpoints>& out) noexcept {
    return runtime_.CopySemanticTopologyEndpointIds(out);
}

uint32_t AudioCoordinator::CopySemanticMatrixEndpointIds(
    std::array<EndpointId, kMaxAudioSemanticMatrixEndpoints>& out) noexcept {
    return runtime_.CopySemanticMatrixEndpointIds(out);
}

IOReturn AudioCoordinator::CopyAudioControlSurfaceSnapshot(
    EndpointId endpointId, AudioControlSurfaceSnapshot& outSnapshot) noexcept {
    outSnapshot = {};
    if (!endpointId || teardownRequested_.load(std::memory_order_acquire)) {
        return kIOReturnNotReady;
    }
    const auto protocol = runtime_.FindShared(endpointId);
    const auto endpoint = runtime_.FindEndpointRuntime(endpointId);
    auto* surface = protocol ? protocol->AsAudioControlSurface() : nullptr;
    if (!surface || !endpoint) return kIOReturnUnsupported;
    const uint64_t topologyRevision = endpoint->CopyTopologyRevision();
    if (topologyRevision == 0 || !surface->CopyAudioControlSurfaceSnapshot(outSnapshot)) {
        return kIOReturnNotReady;
    }
    // A concurrent configuration commit changes the interpretation of the
    // surface. Reject instead of publishing a mixed-revision snapshot.
    if (endpoint->CopyTopologyRevision() != topologyRevision) return kIOReturnBusy;
    outSnapshot.topologyRevision = topologyRevision;
    return kIOReturnSuccess;
}

IOReturn AudioCoordinator::CopyAudioSemanticTopology(
    EndpointId endpointId, AudioSemanticTopologySnapshot& outSnapshot) noexcept {
    outSnapshot = {};
    if (!endpointId || teardownRequested_.load(std::memory_order_acquire)) {
        return kIOReturnNotReady;
    }
    const auto protocol = runtime_.FindShared(endpointId);
    const auto endpoint = runtime_.FindEndpointRuntime(endpointId);
    const auto* topology = protocol ? protocol->AsAudioSemanticTopology() : nullptr;
    if (!topology || !endpoint) return kIOReturnUnsupported;
    const uint64_t topologyRevision = endpoint->CopyTopologyRevision();
    if (topologyRevision == 0 || !topology->CopyAudioSemanticTopology(outSnapshot)) {
        return kIOReturnNotReady;
    }
    if (endpoint->CopyTopologyRevision() != topologyRevision) return kIOReturnBusy;
    outSnapshot.topologyRevision = topologyRevision;
    // A topology producer is an internal implementation detail; the UserClient
    // boundary is not. Do not publish a malformed graph merely because the
    // producer returned true. This mirrors the lab's validate-before-commit
    // rule without making the DriverKit path allocate diagnostics.
    if (!ValidateAudioSemanticTopology(outSnapshot)) return kIOReturnBadArgument;
    return kIOReturnSuccess;
}

IOReturn AudioCoordinator::CopyAudioSemanticConsoleLayout(
    EndpointId endpointId, AudioSemanticConsoleLayoutSnapshot& outSnapshot) noexcept {
    outSnapshot = {};
    if (!endpointId || teardownRequested_.load(std::memory_order_acquire)) {
        return kIOReturnNotReady;
    }
    const auto protocol = runtime_.FindShared(endpointId);
    const auto endpoint = runtime_.FindEndpointRuntime(endpointId);
    const auto* layout = protocol ? protocol->AsAudioSemanticConsoleLayout() : nullptr;
    if (!layout || !endpoint) return kIOReturnUnsupported;
    const uint64_t topologyRevision = endpoint->CopyTopologyRevision();
    if (topologyRevision == 0 || !layout->CopyAudioSemanticConsoleLayout(outSnapshot)) {
        return kIOReturnNotReady;
    }
    if (endpoint->CopyTopologyRevision() != topologyRevision) return kIOReturnBusy;
    outSnapshot.topologyRevision = topologyRevision;
    if (!ValidateAudioSemanticConsoleLayout(outSnapshot)) return kIOReturnBadArgument;
    return kIOReturnSuccess;
}

IOReturn AudioCoordinator::CopyAudioSemanticMatrix(
    EndpointId endpointId, AudioSemanticMatrixSnapshot& outSnapshot) noexcept {
    outSnapshot = {};
    if (!endpointId || teardownRequested_.load(std::memory_order_acquire)) {
        return kIOReturnNotReady;
    }
    const auto protocol = runtime_.FindShared(endpointId);
    const auto endpoint = runtime_.FindEndpointRuntime(endpointId);
    const auto* matrix = protocol ? protocol->AsAudioSemanticMatrix() : nullptr;
    if (!matrix || !endpoint) return kIOReturnUnsupported;
    const uint64_t topologyRevision = endpoint->CopyTopologyRevision();
    if (topologyRevision == 0 || !matrix->CopyAudioSemanticMatrix(outSnapshot)) {
        return kIOReturnNotReady;
    }
    if (endpoint->CopyTopologyRevision() != topologyRevision) return kIOReturnBusy;
    outSnapshot.topologyRevision = topologyRevision;
    if (!ValidateAudioSemanticMatrix(outSnapshot)) return kIOReturnBadArgument;
    return kIOReturnSuccess;
}

IOReturn AudioCoordinator::RequestAudioControlValue(
    EndpointId endpointId, uint32_t controlId, int32_t value) noexcept {
    // Selector 1019 predates the asynchronous control plane. It deliberately
    // remains a hard refusal: waiting for a FireWire completion here can block
    // the UserClient queue for two seconds and make the host appear frozen.
    (void)endpointId;
    (void)controlId;
    (void)value;
    return kIOReturnUnsupported;
}

IOReturn AudioCoordinator::SubmitAudioControlValue(
    EndpointId endpointId, uint32_t controlId, int32_t value,
    IAudioControlSurface::ApplyCallback completion) noexcept {
    if (!completion) return kIOReturnBadArgument;
    if (!endpointId || teardownRequested_.load(std::memory_order_acquire)) {
        return kIOReturnNotReady;
    }
    const auto protocol = runtime_.FindShared(endpointId);
    auto* surface = protocol ? protocol->AsAudioControlSurface() : nullptr;
    if (!surface) return kIOReturnUnsupported;
    surface->ApplyAudioControlValue(controlId, value, std::move(completion));
    return kIOReturnSuccess;
}

IOReturn AudioCoordinator::CopyAudioMeterSnapshot(
    EndpointId endpointId, AudioMeterSnapshot& outSnapshot) noexcept {
    outSnapshot = {};
    if (!endpointId || teardownRequested_.load(std::memory_order_acquire)) {
        return kIOReturnNotReady;
    }
    const auto protocol = runtime_.FindShared(endpointId);
    const auto endpoint = runtime_.FindEndpointRuntime(endpointId);
    auto* metering = protocol ? protocol->AsAudioMetering() : nullptr;
    if (!metering || !endpoint) return kIOReturnUnsupported;
    const uint64_t topologyRevision = endpoint->CopyTopologyRevision();
    if (topologyRevision == 0 || !metering->CopyAudioMeterSnapshot(outSnapshot)) {
        return kIOReturnNotReady;
    }
    if (endpoint->CopyTopologyRevision() != topologyRevision) return kIOReturnBusy;
    outSnapshot.topologyRevision = topologyRevision;
    return kIOReturnSuccess;
}

IOReturn AudioCoordinator::SetAudioMeteringEnabled(
    EndpointId endpointId, bool enabled) noexcept {
    if (!endpointId || teardownRequested_.load(std::memory_order_acquire)) {
        return kIOReturnNotReady;
    }
    const auto protocol = runtime_.FindShared(endpointId);
    auto* metering = protocol ? protocol->AsAudioMetering() : nullptr;
    return metering ? metering->SetAudioMeteringEnabled(enabled) : kIOReturnUnsupported;
}

void AudioCoordinator::HandleCycleInconsistent() noexcept {
    EndpointId endpointId{};
    if (lock_) {
        IOLockLock(lock_);
        endpointId = activeEndpoint_;
        IOLockUnlock(lock_);
    }
    if (endpointId) {
        (void)duplexCoordinator_.RecoverStreaming(
            endpointId, DuplexRestartReason::kRecoverAfterCycleInconsistent);
    }
}

void AudioCoordinator::SetTxTransportFaultDispatch(
    TxTransportFaultDispatch dispatch) noexcept {
    txTransportFaultDispatch_ = std::move(dispatch);
}

// Arrives on the isoch watchdog/poll thread, straight out of
// IsochTransmitContext::StopImmediatelyForTxFault(). Everything here must stay
// non-blocking: that thread also carries the RX drain, which in the
// interrupt-stall failure is the only thing still moving audio.
//
// The hop is an OSAction fired by ASFWAudioNub, landing on ASFWAudioDriver's
// default queue, which then calls back in through the nub to
// RecoverAfterTxTransportFault(). That is the same shape as the existing
// TxPreparationReady/DeviceConfigurationRequested actions.
//
// Two cheaper wirings were considered and rejected. If the OSAction hop proves
// troublesome (action registration races on teardown, or the fault arriving
// while the audio driver's queue is already blocked in a stop), they are the
// fallbacks, in order:
//
//   (b) Call AudioDuplexCoordinator::RecoverStreaming() inline from here, the
//       way HandleHostTimingLoss() already does for RX
//       (DirectAudioReceiveConsumer.cpp fires its callback inline from the RX
//       drain). ~30 lines, symmetric with shipping behaviour, and wrong for
//       this path: RecoverStreaming() IOSleep-polls for the endpoint claim up
//       to kSyncBridgeTimeoutMs, so it would stall the RX drain for up to that
//       long. Acceptable only as a stopgap, and only if the poll thread is
//       ever split so TX and RX no longer share it.
//
//   (c) Give AudioCoordinator (or AudioDuplexCoordinator) its own
//       IODispatchQueue and make RecoverStreaming() async for both RX and TX.
//       This is the right long-term answer — it also fixes the latent RX case
//       in (b) — but it changes recovery execution for every caller, so it
//       belongs in its own change with its own test pass. Note that
//       Driver::Scheduler is NOT a shortcut here: it is bound to
//       ctx.workQueue (DriverContext.cpp:327), which is also the interrupt
//       dispatch source's queue (DriverContext.cpp:371), so dispatching a
//       blocking recovery onto it would stall interrupt delivery and the
//       watchdog — a strictly worse version of the bug being fixed.
void AudioCoordinator::HandleTxTransportFault(
    EndpointId endpointId, uint32_t statusRaw,
    uint64_t streamGeneration) noexcept {
    if (!endpointId || teardownRequested_.load(std::memory_order_acquire)) {
        return;
    }
    ASFW_LOG(Audio,
             "AudioCoordinator: TX transport fault endpoint=%llx status=%u "
             "streamGeneration=%llu — dispatching recovery",
             endpointId.value, statusRaw, streamGeneration);
    if (txTransportFaultDispatch_) {
        txTransportFaultDispatch_(statusRaw, streamGeneration);
    } else {
        // No audio driver is attached to hop through. Report it rather than
        // silently leaving the HAL believing the stream is alive.
        ASFW_LOG_ERROR(Audio,
                       "AudioCoordinator: TX transport fault endpoint=%llx has "
                       "no dispatch target; recovery not scheduled",
                       endpointId.value);
    }
}

void AudioCoordinator::RecoverAfterTxTransportFault(
    EndpointId endpointId) noexcept {
    if (!endpointId || teardownRequested_.load(std::memory_order_acquire)) {
        return;
    }
    if (lock_) {
        IOLockLock(lock_);
        const bool invalidated = invalidatedEndpoints_.contains(endpointId);
        IOLockUnlock(lock_);
        if (invalidated) return;
    }
    (void)duplexCoordinator_.RecoverStreaming(
        endpointId, DuplexRestartReason::kRecoverAfterTxFault);
}

void AudioCoordinator::HandleHostTimingLoss(EndpointId endpointId) noexcept {
    if (!endpointId) return;
    if (lock_) {
        IOLockLock(lock_);
        const bool invalidated = invalidatedEndpoints_.contains(endpointId);
        IOLockUnlock(lock_);
        if (invalidated) return;
    }
    (void)duplexCoordinator_.RecoverStreaming(
        endpointId, DuplexRestartReason::kRecoverAfterTimingLoss);
}

void AudioCoordinator::BeginTeardown() noexcept {
    if (teardownRequested_.exchange(true, std::memory_order_acq_rel)) return;
    hostTransport_.SetTimingLossCallback({});
    hostTransport_.SetTxTransportFaultCallback({});
    txTransportFaultDispatch_ = {};
    hostTransport_.SetTxPreparationCallback({});
    hostTransport_.SetClockAnchorReadyCallback({});
    (void)StopHostTransport("service-teardown", false);
    if (lock_) {
        IOLockLock(lock_);
        activeEndpoint_ = {};
        IOLockUnlock(lock_);
    }
}

bool AudioCoordinator::NotifySessionStreaming(EndpointId endpointId,
                                              bool streaming) noexcept {
    SessionStreamingCallback callback;
    if (lock_) {
        IOLockLock(lock_);
        callback = sessionStreamingCallback_;
        IOLockUnlock(lock_);
    }
    // Endpoint publication is session-manager owned. Absence of the callback
    // therefore means there is no authoritative live session to transition.
    return callback && callback(endpointId, streaming);
}

kern_return_t AudioCoordinator::StopHostTransport(
    const char* reason, bool generationInvalidated) noexcept {
    const kern_return_t status = generationInvalidated
        ? hostTransport_.StopAllAfterBusReset()
        : hostTransport_.StopAll();
    ASFW_LOG(Audio,
             "[Lifecycle] AudioCoordinator host teardown reason=%{public}s reset=%u kr=0x%08x",
             reason ? reason : "unknown", generationInvalidated ? 1U : 0U,
             status);
    return status;
}

std::optional<AudioCoordinator::EndpointId>
AudioCoordinator::GetSinglePublishedEndpointId() const noexcept {
    return publisher_.GetSingleEndpointId();
}

} // namespace ASFW::Audio
