// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "AudioNubPublisher.hpp"

#include "../Devices/AudioEndpointProfileWire.hpp"
#include "../Model/AudioPropertyKeys.hpp"

#include <DriverKit/IOService.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSData.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/OSString.h>

#include <algorithm>

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

bool AudioNubPublisher::ReserveEndpointLocked(
    Devices::AudioEndpointId endpointId) noexcept {
    // Reserve slot so concurrent creators cannot race-create duplicates.
    const auto [it, inserted] = nubsByEndpoint_.emplace(endpointId, nullptr);
    return inserted;
}

bool AudioNubPublisher::EnsureNub(
    const Devices::ResolvedAudioEndpointProfile& profile,
                                  const char* sourceTag) noexcept {
    const auto endpointId = profile.endpointId;
    if (!driver_ || !lock_ || !endpointId || !profile.deviceInstanceId) {
        return false;
    }

    const auto serialized = Devices::Wire::Serialize(profile);
    if (!serialized) {
        ASFW_LOG_ERROR(Audio,
                       "AudioNubPublisher[%{public}s]: profile serialization failed endpoint=%llu error=%u",
                       sourceTag ? sourceTag : "unknown",
                       endpointId.value,
                       static_cast<unsigned>(serialized.error()));
        return false;
    }

    IOLockLock(lock_);
    {
        auto it = nubsByEndpoint_.find(endpointId);
        if (it != nubsByEndpoint_.end()) {
            const bool ready = (it->second != nullptr);
            IOLockUnlock(lock_);
            if (!ready) {
                ASFW_LOG_WARNING(Audio,
                                 "AudioNubPublisher[%{public}s]: endpoint=%llu is reserved but nub is not ready",
                                 sourceTag ? sourceTag : "unknown",
                                 endpointId.value);
            }
            return ready;
        }

        if (!ReserveEndpointLocked(endpointId)) {
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
                       "AudioNubPublisher[%{public}s]: Failed to create ASFWAudioNub (endpoint=%llu kr=0x%x)",
                       sourceTag ? sourceTag : "unknown",
                       endpointId.value,
                       kr);
        IOLockLock(lock_);
        nubsByEndpoint_.erase(endpointId);
        IOLockUnlock(lock_);
        return false;
    }

    // Populate properties on the nub BEFORE it starts.
    OSDictionary* propertiesRaw = nullptr;
    kr = nubService->CopyProperties(&propertiesRaw);
    OSSharedPtr<OSDictionary> properties = OSSharedPtr(propertiesRaw, OSNoRetain);
    if (kr == kIOReturnSuccess && properties) {
        namespace Keys = Model::PropertyKeys;
        auto profileData = OSSharedPtr(
            OSData::withBytes(serialized->data(),
                              static_cast<uint32_t>(serialized->size())),
            OSNoRetain);
        auto endpointNumber = OSSharedPtr(
            OSNumber::withNumber(endpointId.value, 64), OSNoRetain);
        auto instanceNumber = OSSharedPtr(
            OSNumber::withNumber(profile.deviceInstanceId.value, 64), OSNoRetain);
        auto observedGuidNumber = OSSharedPtr(
            OSNumber::withNumber(profile.observedGuid, 64), OSNoRetain);
        auto identityStatusNumber = OSSharedPtr(
            OSNumber::withNumber(static_cast<uint8_t>(profile.identityStatus), 8),
            OSNoRetain);
        auto deviceName = OSSharedPtr(
            OSString::withCString(profile.deviceName.c_str()), OSNoRetain);
        auto vendorName = OSSharedPtr(
            OSString::withCString(profile.vendorName.c_str()), OSNoRetain);
        auto coreAudioUid = OSSharedPtr(
            OSString::withCString(profile.coreAudioUid.c_str()), OSNoRetain);
        if (!profileData || !endpointNumber || !instanceNumber ||
            !observedGuidNumber || !identityStatusNumber || !deviceName ||
            !vendorName || !coreAudioUid) {
            ASFW_LOG_ERROR(Audio,
                           "AudioNubPublisher[%{public}s]: Failed to allocate profile properties (endpoint=%llu)",
                           sourceTag ? sourceTag : "unknown",
                           endpointId.value);
            IOLockLock(lock_);
            nubsByEndpoint_.erase(endpointId);
            IOLockUnlock(lock_);
            nubService->release();
            return false;
        }
        properties->setObject(Keys::kEndpointProfile, profileData.get());
        properties->setObject(Keys::kEndpointId, endpointNumber.get());
        properties->setObject(Keys::kDeviceInstanceId, instanceNumber.get());
        properties->setObject(Keys::kObservedGuid, observedGuidNumber.get());
        properties->setObject(Keys::kPersistentIdentityStatus,
                              identityStatusNumber.get());
        properties->setObject(Keys::kDeviceName, deviceName.get());
        properties->setObject(Keys::kVendorName, vendorName.get());
        properties->setObject(Keys::kCoreAudioUid, coreAudioUid.get());
        kr = nubService->SetProperties(properties.get());
        if (kr != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Audio,
                           "AudioNubPublisher[%{public}s]: SetProperties failed endpoint=%llu kr=0x%x",
                           sourceTag ? sourceTag : "unknown",
                           endpointId.value,
                           kr);
            IOLockLock(lock_);
            nubsByEndpoint_.erase(endpointId);
            IOLockUnlock(lock_);
            nubService->release();
            return false;
        }
    } else {
        ASFW_LOG_ERROR(Audio,
                       "AudioNubPublisher[%{public}s]: Failed to copy properties for ASFWAudioNub (endpoint=%llu kr=0x%x)",
                       sourceTag ? sourceTag : "unknown",
                       endpointId.value,
                       kr);
        IOLockLock(lock_);
        nubsByEndpoint_.erase(endpointId);
        IOLockUnlock(lock_);
        nubService->release();
        return false;
    }

    ASFWAudioNub* audioNub = OSDynamicCast(ASFWAudioNub, nubService);
    if (!audioNub) {
        ASFW_LOG_ERROR(Audio,
                       "AudioNubPublisher[%{public}s]: Created service is not ASFWAudioNub (endpoint=%llu)",
                       sourceTag ? sourceTag : "unknown",
                       endpointId.value);
        IOLockLock(lock_);
        nubsByEndpoint_.erase(endpointId);
        IOLockUnlock(lock_);
        nubService->release();
        return false;
    }

    // Pass scalar values only. The immutable profile itself crosses the service
    // boundary exclusively as copied OSData above.
    audioNub->SetEndpointId(endpointId.value);
    audioNub->SetAudioGeometry(
        profile.runtimeCaps.hostInputPcmChannels,
        profile.runtimeCaps.hostOutputPcmChannels,
        profile.currentSampleRateHz,
        static_cast<uint32_t>(profile.streamMode));

    IOLockLock(lock_);
    nubsByEndpoint_[endpointId] = audioNub;
    IOLockUnlock(lock_);

    // Release our creation reference - IOKit retains it.
    nubService->release();

    ASFW_LOG(Audio,
             "AudioNubPublisher[%{public}s]: ASFWAudioNub ready endpoint=%llu instance=%llu observedGUID=%llx",
             sourceTag ? sourceTag : "unknown",
             endpointId.value,
             profile.deviceInstanceId.value,
             profile.observedGuid);
    return true;
}

ASFWAudioNub* AudioNubPublisher::GetNub(
    Devices::AudioEndpointId endpointId) const noexcept {
    if (!lock_ || !endpointId) return nullptr;

    IOLockLock(lock_);
    auto it = nubsByEndpoint_.find(endpointId);
    ASFWAudioNub* nub = (it != nubsByEndpoint_.end()) ? it->second : nullptr;
    IOLockUnlock(lock_);
    return nub;
}

std::optional<Devices::AudioEndpointId>
AudioNubPublisher::GetSingleEndpointId() const noexcept {
    if (!lock_) return std::nullopt;

    IOLockLock(lock_);
    std::optional<Devices::AudioEndpointId> result = std::nullopt;
    if (nubsByEndpoint_.size() == 1) {
        result = nubsByEndpoint_.begin()->first;
    }
    IOLockUnlock(lock_);
    return result;
}

void AudioNubPublisher::TerminateNub(Devices::AudioEndpointId endpointId,
                                    const char* reasonTag) noexcept {
    if (!lock_ || !endpointId) return;

    ASFWAudioNub* nub = nullptr;
    IOLockLock(lock_);
    auto it = nubsByEndpoint_.find(endpointId);
    if (it != nubsByEndpoint_.end()) {
        nub = it->second;
        nubsByEndpoint_.erase(it);
    }
    IOLockUnlock(lock_);

    if (nub) {
        ASFW_LOG(Audio,
                 "AudioNubPublisher[%{public}s]: Terminating ASFWAudioNub endpoint=%llu",
                 reasonTag ? reasonTag : "unknown",
                 endpointId.value);
        nub->Terminate(0);
    }
}

} // namespace ASFW::Audio
