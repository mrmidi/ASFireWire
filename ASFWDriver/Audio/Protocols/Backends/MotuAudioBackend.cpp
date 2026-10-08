// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuAudioBackend.cpp - see MotuAudioBackend.hpp.

#include "MotuAudioBackend.hpp"

#include "../../Session/AudioSessions.hpp"
#include "../../../Audio/Core/AudioEndpointRuntime.hpp"
#include "../../../Audio/Core/AudioNubPublisher.hpp"
#include "../../../Audio/Core/AudioRuntimeRegistry.hpp"
#include "../../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "../../../Discovery/DeviceRegistry.hpp"
#include "../../../Logging/Logging.hpp"
#include "../IDeviceProtocol.hpp"
#include "../MOTU/MotuV2Protocol.hpp"
#include "../MOTU/MotuV2Registers.hpp"
#include "../../Wire/MOTU/MotuBlockLayout.hpp"
#include "../../Wire/MOTU/MotuPortLayout.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace ASFW::Audio {

MotuAudioBackend::MotuAudioBackend(AudioNubPublisher& publisher,
                                   Discovery::DeviceRegistry& registry,
                                   AudioRuntimeRegistry& runtime,
                                   Session::AudioSessions& sessions) noexcept
    : publisher_(publisher)
    , registry_(registry)
    , runtime_(runtime)
    , sessions_(sessions) {
    lock_ = IOLockAlloc();

    IODispatchQueue* queue = nullptr;
    const kern_return_t queueStatus = IODispatchQueue::Create("com.asfw.audio.motu", 0, 0, &queue);
    if (queueStatus == kIOReturnSuccess && queue) {
        workQueue_ = OSSharedPtr(queue, OSNoRetain);
    }
}

MotuAudioBackend::~MotuAudioBackend() noexcept {
    BeginTeardown();
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

void MotuAudioBackend::BeginTeardown() noexcept {
    // Latch first so a publication or recovery that arrives now refuses rather
    // than touching a device whose bus is going away.
    stopping_.store(true, std::memory_order_release);
    if (teardownStarted_.exchange(true, std::memory_order_acq_rel)) {
#ifdef ASFW_HOST_TEST
        if (onSecondaryTeardown_) onSecondaryTeardown_();
#endif
        while (!teardownComplete_.load(std::memory_order_acquire)) IOSleep(1);
        return;
    }
    recoveryAdmission_.CloseAndWait();
    if (workQueue_) {
#ifdef ASFW_HOST_TEST
        if (onTeardownDrain_) onTeardownDrain_();
        workQueue_->DispatchSync([] {});
#else
        workQueue_->DispatchSync(^{});
#endif
    }
    teardownComplete_.store(true, std::memory_order_release);
}

void MotuAudioBackend::OnDeviceRecordUpdated(uint64_t guid) noexcept {
    if (stopping_.load(std::memory_order_acquire)) {
        return;
    }
    EnsureNubForGuid(guid);
}

void MotuAudioBackend::CancelRemoteDeviceWork(uint64_t guid) noexcept {
    (void)guid;
}

void MotuAudioBackend::EnsureNubForGuid(uint64_t guid) noexcept {
    if (guid == 0) {
        return;
    }

    const auto record = registry_.SnapshotByGuid(guid);
    if (!record.has_value()) {
        ASFW_LOG(Audio,
                 "MotuAudioBackend::EnsureNubForGuid: no registry record for GUID=0x%016llx",
                 guid);
        return;
    }

    auto protocol = runtime_.FindShared(guid);
    if (!protocol) {
        ASFW_LOG(Audio,
                 "MotuAudioBackend::EnsureNubForGuid: no protocol for GUID=0x%016llx",
                 guid);
        return;
    }

    // MOTU has no profile registry to consult. The device's geometry comes from its own
    // registers, so the nub is built from the hardware's answer rather than a table
    // keyed on model_id (which MOTU publishes as 0 anyway).
    //
    // The optical config (0x0c04) is read first, asynchronously, as DICE loads its caps:
    // ADAT adds PCM chunks to either direction, so the counts are not a model constant.
    // Linux reads the same register at PCM open (motu-pcm.c:143); it needs only the
    // async bus, not a started stream, so reading it before publication cannot deadlock
    // CoreAudio against the nub.
    protocol->EnsureRuntimeStreamGeometry(
        [this, guid, record = *record, protocol](IOReturn geometryStatus) {
            if (stopping_.load(std::memory_order_acquire)) {
                return;
            }
            PublishNub(guid, record, *protocol, geometryStatus);
        });
}

void MotuAudioBackend::PublishNub(uint64_t guid, const Discovery::DeviceRecord& record,
                                  IDeviceProtocol& protocol, IOReturn geometryStatus) noexcept {
    Model::ASFWAudioDevice dev{};
    dev.guid = record.guid;
    dev.vendorId = record.vendorId;
    dev.modelId = record.modelId;
    // CoreAudio shows this in the Sound panel, where MOTU's own driver named the device
    // "MOTU UltraLite". The model constants stay bare; only the display name
    // is qualified here.
    const char* const modelName =
        DeviceProfiles::Audio::AudioDeviceCatalog::MotuModelNameForSwVersion(
            record.unitSwVersion.value_or(0U));
    dev.deviceName = modelName != nullptr
                         ? std::string(DeviceProfiles::Audio::kMotuVendorName) + " " + modelName
                         : protocol.GetName();
    dev.inputPlugName = "Input";
    dev.outputPlugName = "Output";
    dev.sampleRates.assign(std::begin(Motu::kPublishedSampleRatesHz),
                           std::end(Motu::kPublishedSampleRatesHz));
    dev.currentSampleRate = 48000u;

    // Geometry: the counts read from the optical config. Only a failed read falls back
    // to the model's fixed layout (14 PCM chunks per direction at 44.1/48 kHz,
    // motu-protocol-v2.c:274-282); that is the same 14 x 14 the device carries unless an
    // optical port is in ADAT mode.
    AudioStreamRuntimeCaps caps{};
    const bool haveCaps = protocol.GetRuntimeAudioStreamCaps(caps) && caps.sampleRateHz != 0;
    if (haveCaps) {
        dev.inputChannelCount = caps.hostInputPcmChannels;
        dev.outputChannelCount = caps.hostOutputPcmChannels;
        dev.currentSampleRate = caps.sampleRateHz;
    } else {
        const uint32_t fixedChunks = ::ASFW::Encoding::Motu::k828mk2FixedPcmChunks[0];
        dev.inputChannelCount = fixedChunks;
        dev.outputChannelCount = fixedChunks;
        dev.currentSampleRate = 48000u;
    }
    if (geometryStatus != kIOReturnSuccess) {
        ASFW_LOG(Audio,
                 "MotuAudioBackend::EnsureNubForGuid: optical config read failed kr=0x%08x "
                 "for GUID=0x%016llx; publishing the model's fixed geometry (%u x %u)",
                 geometryStatus, guid, dev.inputChannelCount, dev.outputChannelCount);
    }
    dev.channelCount = std::max(dev.inputChannelCount, dev.outputChannelCount);

    // Port names in host channel order, which is not wire order: the encoder and decoder
    // apply the same model table, so these line up with what each channel carries.
    std::vector<std::string> inNames;
    std::vector<std::string> outNames;
    if (protocol.GetChannelLabels(inNames, outNames)) {
        dev.inputChannelNames = std::move(inNames);
        dev.outputChannelNames = std::move(outNames);
    }

    if (auto endpoint = runtime_.EnsureEndpointRuntime(guid)) {
        endpoint->UpdateConfig(dev);
    }
    (void)publisher_.EnsureNub(guid, dev, "MOTU");
}

bool MotuAudioBackend::QueueTimingRecovery(uint64_t guid) noexcept {
    PublicationGate::AdmissionScope admission(recoveryAdmission_);
    if (!admission.IsAdmitted()) return false;
    if (guid == 0 || stopping_.load(std::memory_order_acquire)) {
        return false;
    }

    // The fault belongs to the run streaming now; the session drops it if
    // that run has ended by the time the block below asks for the restart.
    const uint64_t observedRun = sessions_.RunningRun(guid);
    if (observedRun == Session::SessionScheduler::kNotRunning) return false;

    if (recoveryInFlight_.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }

#ifdef ASFW_HOST_TEST
    auto recover = [this, guid, observedRun] {
#else
    auto recover = ^{
#endif
        if (stopping_.load(std::memory_order_acquire) || sessions_.IsCancelled(guid)) {
            recoveryInFlight_.store(false, std::memory_order_release);
            return;
        }

        ASFW_LOG(Audio,
                 "MotuAudioBackend: scheduling async recovery for timing loss GUID=0x%016llx",
                 guid);
        const IOReturn status = sessions_.RequestRestart(
            guid, DuplexRestartReason::kRecoverAfterTimingLoss, observedRun);
        if (status == kIOReturnSuccess) {
            ASFW_LOG(Audio,
                     "MotuAudioBackend: timing-loss recovery succeeded GUID=0x%016llx",
                     guid);
        } else if (status == kIOReturnUnsupported || status == kIOReturnAborted) {
            ASFW_LOG(Audio,
                     "MotuAudioBackend: timing-loss recovery not applicable GUID=0x%016llx kr=0x%x",
                     guid, status);
        } else {
            ASFW_LOG_ERROR(Audio,
                           "MotuAudioBackend: timing-loss recovery failed GUID=0x%016llx kr=0x%x",
                           guid, status);
        }
        recoveryInFlight_.store(false, std::memory_order_release);
    };

    if (workQueue_) {
        workQueue_->DispatchAsync(recover);
        return true;
    }
    // No work queue available — cannot recover synchronously from the packet
    // thread. This backend cannot recover until it is recreated with a queue.
    recoveryInFlight_.store(false, std::memory_order_release);
    return false;
}

} // namespace ASFW::Audio
