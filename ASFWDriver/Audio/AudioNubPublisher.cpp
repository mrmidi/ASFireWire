// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project

#include "AudioNubPublisher.hpp"

#include <DriverKit/IOService.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSSharedPtr.h>

#include "../Logging/Logging.hpp"
#include "ASFWAudioNub.h"

namespace ASFW::Audio {

AudioNubPublisher::AudioNubPublisher(IOService* driver) noexcept
    : driver_(driver) {
    lock_ = IOLockAlloc();
    if (!lock_) {
        ASFW_LOG_ERROR(Audio, "AudioNubPublisher: Failed to allocate lock");
    }
}

AudioNubPublisher::~AudioNubPublisher() noexcept {
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

bool AudioNubPublisher::ReserveGuidLocked(uint64_t guid) noexcept {
    // Reserve slot so concurrent creators cannot race-create duplicates.
    const auto [it, inserted] = nubsByGuid_.emplace(guid, nullptr);
    return inserted;
}

bool AudioNubPublisher::EnsureNub(uint64_t guid,
                                  const Model::ASFWAudioDevice& config,
                                  const char* sourceTag) noexcept {
    if (!driver_ || !lock_ || guid == 0) {
        return false;
    }

    IOLockLock(lock_);
    {
        auto it = nubsByGuid_.find(guid);
        if (it != nubsByGuid_.end()) {
            // Already present (or reserved/in-progress).
            IOLockUnlock(lock_);
            return true;
        }

        if (!ReserveGuidLocked(guid)) {
            IOLockUnlock(lock_);
            return true;
        }
    }
    IOLockUnlock(lock_);

    IOService* nubService = nullptr;
    kern_return_t kr = driver_->Create(
        driver_,                  // provider
        "ASFWAudioNubProperties",  // propertiesKey from Info.plist
        &nubService);

    if (kr != kIOReturnSuccess || !nubService) {
        ASFW_LOG_ERROR(Audio,
                       "AudioNubPublisher[%{public}s]: Failed to create ASFWAudioNub (GUID=%llx kr=0x%x)",
                       sourceTag ? sourceTag : "unknown",
                       guid,
                       kr);
        IOLockLock(lock_);
        nubsByGuid_.erase(guid);
        IOLockUnlock(lock_);
        return false;
    }

    // Populate properties on the nub BEFORE it starts.
    OSDictionary* propertiesRaw = nullptr;
    kr = nubService->CopyProperties(&propertiesRaw);
    OSSharedPtr<OSDictionary> properties = OSSharedPtr(propertiesRaw, OSNoRetain);
    if (kr == kIOReturnSuccess && properties) {
        if (!config.PopulateNubProperties(properties.get())) {
            ASFW_LOG_ERROR(Audio,
                           "AudioNubPublisher[%{public}s]: Failed to populate properties (GUID=%llx)",
                           sourceTag ? sourceTag : "unknown",
                           guid);
        } else {
            nubService->SetProperties(properties.get());
            ASFW_LOG(Audio,
                     "AudioNubPublisher[%{public}s]: ASFWAudioDevice properties set (GUID=%llx rate=%u Hz agg=%u in=%u out=%u)",
                     sourceTag ? sourceTag : "unknown",
                     guid,
                     config.currentSampleRate,
                     config.channelCount,
                     config.inputChannelCount,
                     config.outputChannelCount);
        }
    }

    ASFWAudioNub* audioNub = OSDynamicCast(ASFWAudioNub, nubService);
    if (!audioNub) {
        ASFW_LOG_ERROR(Audio,
                       "AudioNubPublisher[%{public}s]: Created service is not ASFWAudioNub (GUID=%llx)",
                       sourceTag ? sourceTag : "unknown",
                       guid);
        IOLockLock(lock_);
        nubsByGuid_.erase(guid);
        IOLockUnlock(lock_);
        nubService->release();
        return false;
    }

    // Stream mode and GUID are LOCALONLY helpers; channel topology is derived from nub properties.
    audioNub->SetStreamMode(static_cast<uint32_t>(config.streamMode));
    audioNub->SetGuid(config.guid);

    IOLockLock(lock_);
    nubsByGuid_[guid] = audioNub;
    IOLockUnlock(lock_);

    // Release our creation reference - IOKit retains it.
    nubService->release();

    ASFW_LOG(Audio,
             "✅ AudioNubPublisher[%{public}s]: ASFWAudioNub ready for GUID=%llx",
             sourceTag ? sourceTag : "unknown",
             guid);
    return true;
}

ASFWAudioNub* AudioNubPublisher::GetNub(uint64_t guid) const noexcept {
    if (!lock_ || guid == 0) return nullptr;

    IOLockLock(lock_);
    auto it = nubsByGuid_.find(guid);
    ASFWAudioNub* nub = (it != nubsByGuid_.end()) ? it->second : nullptr;
    IOLockUnlock(lock_);
    return nub;
}

std::optional<uint64_t> AudioNubPublisher::GetSingleGuid() const noexcept {
    if (!lock_) return std::nullopt;

    IOLockLock(lock_);
    std::optional<uint64_t> result = std::nullopt;
    if (nubsByGuid_.size() == 1) {
        result = nubsByGuid_.begin()->first;
    }
    IOLockUnlock(lock_);
    return result;
}

void AudioNubPublisher::TerminateNub(uint64_t guid, const char* reasonTag) noexcept {
    if (!lock_ || guid == 0) return;

    ASFWAudioNub* nub = nullptr;
    IOLockLock(lock_);
    auto it = nubsByGuid_.find(guid);
    if (it != nubsByGuid_.end()) {
        nub = it->second;
        nubsByGuid_.erase(it);
    }
    IOLockUnlock(lock_);

    if (nub) {
        ASFW_LOG(Audio,
                 "AudioNubPublisher[%{public}s]: Terminating ASFWAudioNub for GUID=%llx",
                 reasonTag ? reasonTag : "unknown",
                 guid);
        nub->Terminate(0);
    }
}

bool AudioNubPublisher::PublishDICEDiagnostic(const Model::DICEBackendDiagnostic& diagnostic,
                                              const char* sourceTag) noexcept {
    if (!driver_) {
        return false;
    }

    auto properties = OSSharedPtr(OSDictionary::withCapacity(24), OSNoRetain);
    auto guidNum = OSSharedPtr(OSNumber::withNumber(diagnostic.guid, 64), OSNoRetain);
    auto vendorNum = OSSharedPtr(OSNumber::withNumber(diagnostic.vendorId, 32), OSNoRetain);
    auto modelNum = OSSharedPtr(OSNumber::withNumber(diagnostic.modelId, 32), OSNoRetain);
    auto deviceName = OSSharedPtr(OSString::withCString(diagnostic.deviceName.c_str()), OSNoRetain);
    auto protocolName = OSSharedPtr(OSString::withCString(diagnostic.protocolName.c_str()), OSNoRetain);
    auto profileSource = OSSharedPtr(OSString::withCString(diagnostic.profileSource.c_str()), OSNoRetain);
    auto probeState = OSSharedPtr(OSString::withCString(diagnostic.probeState.c_str()), OSNoRetain);
    auto failReason = OSSharedPtr(OSString::withCString(diagnostic.failReason.c_str()), OSNoRetain);
    auto capsSource = OSSharedPtr(OSString::withCString(diagnostic.capsSource.c_str()), OSNoRetain);
    auto hostIn = OSSharedPtr(OSNumber::withNumber(diagnostic.hostInputPcmChannels, 32), OSNoRetain);
    auto hostOut = OSSharedPtr(OSNumber::withNumber(diagnostic.hostOutputPcmChannels, 32), OSNoRetain);
    auto d2hSlots = OSSharedPtr(OSNumber::withNumber(diagnostic.deviceToHostAm824Slots, 32), OSNoRetain);
    auto h2dSlots = OSSharedPtr(OSNumber::withNumber(diagnostic.hostToDeviceAm824Slots, 32), OSNoRetain);
    auto d2hStreams = OSSharedPtr(OSNumber::withNumber(diagnostic.deviceToHostActiveStreams, 32), OSNoRetain);
    auto h2dStreams = OSSharedPtr(OSNumber::withNumber(diagnostic.hostToDeviceActiveStreams, 32), OSNoRetain);
    auto sampleRate = OSSharedPtr(OSNumber::withNumber(diagnostic.sampleRateHz, 32), OSNoRetain);
    auto d2hIso = OSSharedPtr(OSNumber::withNumber(diagnostic.deviceToHostIsoChannel, 32), OSNoRetain);
    auto h2dIso = OSSharedPtr(OSNumber::withNumber(diagnostic.hostToDeviceIsoChannel, 32), OSNoRetain);
    auto attempt = OSSharedPtr(OSNumber::withNumber(diagnostic.attempt, 32), OSNoRetain);
    auto maxAttempts = OSSharedPtr(OSNumber::withNumber(diagnostic.maxAttempts, 32), OSNoRetain);
    auto status = OSSharedPtr(OSNumber::withNumber(diagnostic.status, 32), OSNoRetain);

    if (!properties || !guidNum || !vendorNum || !modelNum || !deviceName || !protocolName ||
        !profileSource || !probeState || !failReason || !capsSource || !hostIn || !hostOut ||
        !d2hSlots || !h2dSlots || !d2hStreams || !h2dStreams || !sampleRate || !d2hIso ||
        !h2dIso || !attempt || !maxAttempts || !status) {
        ASFW_LOG_ERROR(Audio,
                       "AudioNubPublisher[%{public}s]: Failed to allocate DICE diagnostic properties (GUID=%llx)",
                       sourceTag ? sourceTag : "unknown",
                       diagnostic.guid);
        return false;
    }

    properties->setObject("ASFWDICELastProbeGUID", guidNum.get());
    properties->setObject("ASFWDICELastProbeVendorID", vendorNum.get());
    properties->setObject("ASFWDICELastProbeModelID", modelNum.get());
    properties->setObject("ASFWDICELastProbeDeviceName", deviceName.get());
    properties->setObject("ASFWDICELastProbeProtocol", protocolName.get());
    properties->setObject("ASFWDICELastProbeProfileSource", profileSource.get());
    properties->setObject("ASFWDICELastProbeState", probeState.get());
    properties->setObject("ASFWDICELastProbeFailReason", failReason.get());
    properties->setObject("ASFWDICELastProbeCapsSource", capsSource.get());
    properties->setObject("ASFWDICELastProbeHostInputPcmChannels", hostIn.get());
    properties->setObject("ASFWDICELastProbeHostOutputPcmChannels", hostOut.get());
    properties->setObject("ASFWDICELastProbeDeviceToHostAm824Slots", d2hSlots.get());
    properties->setObject("ASFWDICELastProbeHostToDeviceAm824Slots", h2dSlots.get());
    properties->setObject("ASFWDICELastProbeDeviceToHostActiveStreams", d2hStreams.get());
    properties->setObject("ASFWDICELastProbeHostToDeviceActiveStreams", h2dStreams.get());
    properties->setObject("ASFWDICELastProbeSampleRateHz", sampleRate.get());
    properties->setObject("ASFWDICELastProbeDeviceToHostIsoChannel", d2hIso.get());
    properties->setObject("ASFWDICELastProbeHostToDeviceIsoChannel", h2dIso.get());
    properties->setObject("ASFWDICELastProbeAttempt", attempt.get());
    properties->setObject("ASFWDICELastProbeMaxAttempts", maxAttempts.get());
    properties->setObject("ASFWDICELastProbeStatus", status.get());

    const kern_return_t kr = driver_->SetProperties(properties.get());
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio,
                       "AudioNubPublisher[%{public}s]: Failed to publish DICE diagnostic properties (GUID=%llx kr=0x%x)",
                       sourceTag ? sourceTag : "unknown",
                       diagnostic.guid,
                       kr);
        return false;
    }

    ASFW_LOG(Audio,
             "AudioNubPublisher[%{public}s]: DICE diagnostic state=%{public}s reason=%{public}s GUID=%llx in=%u out=%u d2hStreams=%u h2dStreams=%u",
             sourceTag ? sourceTag : "unknown",
             diagnostic.probeState.c_str(),
             diagnostic.failReason.c_str(),
             diagnostic.guid,
             diagnostic.hostInputPcmChannels,
             diagnostic.hostOutputPcmChannels,
             diagnostic.deviceToHostActiveStreams,
             diagnostic.hostToDeviceActiveStreams);
    return true;
}

} // namespace ASFW::Audio
