// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuAudioBackend.cpp - see MotuAudioBackend.hpp.

#include "MotuAudioBackend.hpp"

#include "AudioDuplexCoordinator.hpp"
#include "../../../Audio/Core/AudioEndpointRuntime.hpp"
#include "../../../Audio/Core/AudioNubPublisher.hpp"
#include "../../../Audio/Core/AudioRuntimeRegistry.hpp"
#include "../../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "../../../Discovery/DeviceRegistry.hpp"
#include "../../../Logging/Logging.hpp"
#include "../DeviceProtocolFactory.hpp"
#include "../IDeviceProtocol.hpp"
#include "../MOTU/MotuV2Protocol.hpp"
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
                                   AudioDuplexCoordinator& duplexCoordinator,
                                   Driver::HardwareInterface& hardware) noexcept
    : publisher_(publisher)
    , registry_(registry)
    , runtime_(runtime)
    , hardware_(hardware)
    , coordinator_(duplexCoordinator) {
    lock_ = IOLockAlloc();
}

MotuAudioBackend::~MotuAudioBackend() noexcept {
    BeginTeardown();
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

void MotuAudioBackend::BeginTeardown() noexcept {
    // Latch first so an in-flight StartStreaming refuses rather than handing the
    // coordinator a device whose bus is going away.
    if (stopping_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (lock_) {
        IOLockLock(lock_);
        activeStreamingGuids_.clear();
        IOLockUnlock(lock_);
    }
}

void MotuAudioBackend::OnDeviceRecordUpdated(uint64_t guid) noexcept {
    if (stopping_.load(std::memory_order_acquire)) {
        return;
    }
    EnsureNubForGuid(guid);
}

void MotuAudioBackend::CancelRemoteDeviceWork(uint64_t guid) noexcept {
    if (lock_) {
        IOLockLock(lock_);
        activeStreamingGuids_.erase(guid);
        IOLockUnlock(lock_);
    }
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
    // registers via PrepareDuplex, which MotuV2Protocol reports through runtime caps --
    // so the nub is built from the hardware's answer rather than a table keyed on
    // model_id (which MOTU publishes as 0 anyway).
    Model::ASFWAudioDevice dev{};
    dev.guid = record->guid;
    dev.vendorId = record->vendorId;
    dev.modelId = record->modelId;
    // CoreAudio shows this in the Sound panel, where MOTU's own driver named the device
    // "MOTU UltraLite". The model constants stay bare because DeviceIdentityHint keeps
    // vendor and model in separate fields; only the display name is qualified here.
    const char* const modelName =
        DeviceProfiles::Audio::AudioDeviceCatalog::MotuModelNameForSwVersion(
            record->unitSwVersion.value_or(0U));
    dev.deviceName = modelName != nullptr
                         ? std::string(DeviceProfiles::Audio::kMotuVendorName) + " " + modelName
                         : protocol->GetName();
    dev.inputPlugName = "Input";
    dev.outputPlugName = "Output";
    dev.sampleRates = {44100u, 48000u};
    dev.currentSampleRate = 48000u;

    // Geometry: prefer the device's live answer, but fall back to the model's known
    // chunk layout.
    //
    // The fallback is not an optimisation, it is required. Live caps only exist after
    // PrepareDuplex, which runs during streaming; CoreAudio only streams to a device it
    // can see; and it can only see a published nub. Waiting for caps before publishing
    // deadlocks those two against each other and the device never appears at all.
    //
    // The v2 fixed-chunk models carry 14 PCM chunks per direction at 44.1/48 kHz
    // (motu-protocol-v2.c:274-282), which is the geometry to publish with until the
    // hardware says otherwise.
    AudioStreamRuntimeCaps caps{};
    const bool haveLiveCaps =
        protocol->GetRuntimeAudioStreamCaps(caps) && caps.sampleRateHz != 0;
    if (haveLiveCaps) {
        dev.inputChannelCount = caps.hostInputPcmChannels;
        dev.outputChannelCount = caps.hostOutputPcmChannels;
        dev.currentSampleRate = caps.sampleRateHz;
    } else {
        const uint32_t fixedChunks = ::ASFW::Encoding::Motu::k828mk2FixedPcmChunks[0];
        dev.inputChannelCount = fixedChunks;
        dev.outputChannelCount = fixedChunks;
        dev.currentSampleRate = 48000u;
        ASFW_LOG(Audio,
                 "MotuAudioBackend::EnsureNubForGuid: no live caps yet for GUID=0x%016llx; "
                 "publishing the model's fixed geometry (%u x %u @ 48k)",
                 guid, fixedChunks, fixedChunks);
    }
    dev.channelCount = std::max(dev.inputChannelCount, dev.outputChannelCount);

    // Port names in host channel order, which is not wire order: the encoder and decoder
    // apply the same model table, so these line up with what each channel carries.
    std::vector<std::string> inNames;
    std::vector<std::string> outNames;
    if (protocol->GetChannelLabels(inNames, outNames)) {
        dev.inputChannelNames = std::move(inNames);
        dev.outputChannelNames = std::move(outNames);
    }

    if (auto endpoint = runtime_.EnsureEndpointRuntime(guid)) {
        endpoint->UpdateConfig(dev);
    }
    (void)publisher_.EnsureNub(guid, dev, "MOTU");
}

IOReturn MotuAudioBackend::StartStreaming(uint64_t guid) noexcept {
    if (guid == 0) {
        return kIOReturnBadArgument;
    }
    if (stopping_.load(std::memory_order_acquire)) {
        ASFW_LOG(Audio,
                 "MotuAudioBackend: StartStreaming refused by teardown GUID=0x%016llx",
                 guid);
        return kIOReturnAborted;
    }

    auto* nub = publisher_.GetNub(guid);
    if (!nub) {
        EnsureNubForGuid(guid);
        nub = publisher_.GetNub(guid);
        if (!nub) {
            return kIOReturnNotReady;
        }
    }

    auto endpoint = runtime_.FindEndpointRuntime(guid);
    if (!endpoint || !endpoint->HasCompleteDirectAudioMemory()) {
        ASFW_LOG_ERROR(Audio,
                       "MotuAudioBackend: StartStreaming refused missing direct runtime/memory GUID=0x%016llx",
                       guid);
        return kIOReturnNotReady;
    }

    const IOReturn status = coordinator_.StartStreaming(guid);
    if (status == kIOReturnSuccess) {
        EnsureNubForGuid(guid);
        if (lock_) {
            IOLockLock(lock_);
            activeStreamingGuids_.insert(guid);
            IOLockUnlock(lock_);
        }
    }
    return status;
}

IOReturn MotuAudioBackend::StopStreaming(uint64_t guid) noexcept {
    if (stopping_.load(std::memory_order_acquire)) {
        if (lock_) {
            IOLockLock(lock_);
            activeStreamingGuids_.erase(guid);
            IOLockUnlock(lock_);
        }
        return kIOReturnAborted;
    }

    const IOReturn status = coordinator_.StopStreaming(guid);
    if (status == kIOReturnSuccess && lock_) {
        IOLockLock(lock_);
        activeStreamingGuids_.erase(guid);
        IOLockUnlock(lock_);
    }
    return status;
}

} // namespace ASFW::Audio
