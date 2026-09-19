// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "AudioNubPublisher.hpp"

#include <DriverKit/IOService.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSSharedPtr.h>

#include "../../Logging/Logging.hpp"
#include <net.mrmidi.ASFW.ASFWDriver/ASFWAudioNub.h>

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
            const bool ready = (it->second != nullptr);
            IOLockUnlock(lock_);
            if (!ready) {
                ASFW_LOG_WARNING(Audio,
                                 "AudioNubPublisher[%{public}s]: GUID=%llx is reserved but nub is not ready",
                                 sourceTag ? sourceTag : "unknown",
                                 guid);
            }
            return ready;
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
            IOLockLock(lock_);
            nubsByGuid_.erase(guid);
            IOLockUnlock(lock_);
            nubService->release();
            return false;
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
    } else {
        ASFW_LOG_ERROR(Audio,
                       "AudioNubPublisher[%{public}s]: Failed to copy properties for ASFWAudioNub (GUID=%llx kr=0x%x)",
                       sourceTag ? sourceTag : "unknown",
                       guid,
                       kr);
        IOLockLock(lock_);
        nubsByGuid_.erase(guid);
        IOLockUnlock(lock_);
        nubService->release();
        return false;
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

bool AudioNubPublisher::RefreshNubProperties(uint64_t guid,
                                             const Model::ASFWAudioDevice& config,
                                             const char* sourceTag) noexcept {
    ASFWAudioNub* nub = GetNub(guid);
    if (nub == nullptr) {
        return false;
    }

    OSDictionary* propertiesRaw = nullptr;
    const kern_return_t kr = nub->CopyProperties(&propertiesRaw);
    OSSharedPtr<OSDictionary> properties = OSSharedPtr(propertiesRaw, OSNoRetain);
    if (kr != kIOReturnSuccess || !properties) {
        ASFW_LOG_ERROR(Audio,
                       "AudioNubPublisher[%{public}s]: refresh could not copy properties "
                       "(GUID=%llx kr=0x%x)",
                       sourceTag ? sourceTag : "unknown", guid, kr);
        return false;
    }

    // Build the whole set before applying any of it. PopulateNubProperties
    // fails as a unit -- a stream array that could not be serialized aborts it
    // -- so a failure here leaves the nub on its previous description rather
    // than a half-updated one.
    if (!config.PopulateNubProperties(properties.get())) {
        ASFW_LOG_ERROR(Audio,
                       "AudioNubPublisher[%{public}s]: refresh could not populate properties, "
                       "keeping the previous ones (GUID=%llx)",
                       sourceTag ? sourceTag : "unknown", guid);
        return false;
    }

    nub->SetProperties(properties.get());
    ASFW_LOG(Audio,
             "AudioNubPublisher[%{public}s]: refreshed nub properties (GUID=%llx rate=%u "
             "in=%u out=%u playbackStreams=%zu)",
             sourceTag ? sourceTag : "unknown", guid, config.currentSampleRate,
             config.inputChannelCount, config.outputChannelCount,
             config.playbackStreams.size());
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

} // namespace ASFW::Audio
