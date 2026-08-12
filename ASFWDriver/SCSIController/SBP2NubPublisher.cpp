// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "SBP2NubPublisher.hpp"

#include "../Discovery/FWDevice.hpp"
#include "../Discovery/FWUnit.hpp"
#include "../Logging/Logging.hpp"
#include "../Protocols/SBP2/Session/SessionRegistry.hpp"

#include <DriverKit/IOService.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSSharedPtr.h>
#include <net.mrmidi.ASFW.ASFWDriver/ASFWSBP2Nub.h>

namespace ASFW::Protocols::SBP2 {

namespace {

constexpr const char* kDeviceInstanceIdProperty = "ASFWDeviceInstanceID";
constexpr const char* kObservedGuidProperty = "ASFWObservedGUID";
constexpr const char* kUnitDirectoryOffsetProperty = "ASFWUnitDirectoryOffset";
constexpr const char* kLogicalUnitNumberProperty = "ASFWLogicalUnitNumber";

} // namespace

SBP2NubPublisher::SBP2NubPublisher(IOService* driver,
                                   Discovery::IDeviceManager& deviceManager,
                                   IODispatchQueue* workQueue) noexcept
    : driver_(driver)
    , deviceManager_(deviceManager)
    , workQueue_(workQueue)
    , lock_(IOLockAlloc()) {
    if (lock_ == nullptr) {
        ASFW_LOG_ERROR(Controller, "[SBP2Nub] failed to allocate publisher lock");
    }
}

SBP2NubPublisher::~SBP2NubPublisher() {
    Shutdown();
    if (lock_ != nullptr) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

bool SBP2NubPublisher::IsSBP2Unit(const std::shared_ptr<Discovery::FWUnit>& unit) noexcept {
    return unit && unit->Matches(kSBP2UnitSpecId, kSBP2UnitSwVersion);
}

std::optional<SBP2NubPublisher::UnitKey> SBP2NubPublisher::MakeKey(
    const std::shared_ptr<Discovery::FWUnit>& unit) noexcept {
    if (!IsSBP2Unit(unit)) {
        return std::nullopt;
    }
    const auto device = unit->GetDevice();
    if (!device || !device->IsReady()) {
        return std::nullopt;
    }
    return unit->GetInstanceId();
}

void SBP2NubPublisher::Start() {
    if (lock_ == nullptr || workQueue_ == nullptr || driver_ == nullptr) {
        ASFW_LOG_ERROR(Controller, "[SBP2Nub] publisher cannot start without driver, queue, and lock");
        return;
    }

    IOLockLock(lock_);
    if (stopping_ || observing_) {
        IOLockUnlock(lock_);
        return;
    }
    // RegisterUnitObserver only inserts our pointer; unlike the callback API,
    // it does not invoke us synchronously. Holding this lock makes Start and
    // Shutdown atomic with respect to provider revocation during bring-up.
    deviceManager_.RegisterUnitObserver(this);
    observing_ = true;
    IOLockUnlock(lock_);

    for (const auto& unit : deviceManager_.GetReadyUnits()) {
        QueueEnsure(unit);
    }
}

void SBP2NubPublisher::Shutdown() noexcept {
    if (lock_ == nullptr) {
        return;
    }

    decltype(nubsByUnit_) nubs;
    bool unregister = false;
    IOLockLock(lock_);
    if (stopping_) {
        IOLockUnlock(lock_);
        return;
    }
    stopping_ = true;
    unregister = observing_;
    observing_ = false;
    nubs.swap(nubsByUnit_);
    IOLockUnlock(lock_);

    if (unregister) {
        deviceManager_.UnregisterUnitObserver(this);
    }

    for (const auto& [key, published] : nubs) {
        if (published.nub != nullptr) {
            ASFW_LOG(Controller,
                     "[SBP2Nub] terminating instance=%llu unitOffset=%u during shutdown",
                     key.device.value, key.unitDirectoryOffset);
            published.nub->Terminate(0);
        }
    }
}

void SBP2NubPublisher::OnUnitPublished(std::shared_ptr<Discovery::FWUnit> unit) {
    QueueEnsure(std::move(unit));
}

void SBP2NubPublisher::OnUnitSuspended(std::shared_ptr<Discovery::FWUnit> unit) {
    // A reset only invalidates the route. The physical unit and its service
    // incarnation remain valid until an actual removal terminates the unit.
    (void)unit;
}

void SBP2NubPublisher::OnUnitResumed(std::shared_ptr<Discovery::FWUnit> unit) {
    QueueEnsure(std::move(unit));
}

void SBP2NubPublisher::OnUnitTerminated(std::shared_ptr<Discovery::FWUnit> unit) {
    QueueTerminate(std::move(unit));
}

void SBP2NubPublisher::QueueEnsure(std::shared_ptr<Discovery::FWUnit> unit) {
    if (!IsSBP2Unit(unit) || workQueue_ == nullptr) {
        return;
    }
    const auto weak = weak_from_this();
    workQueue_->DispatchAsync(^{
        if (const auto self = weak.lock()) {
            self->EnsureNub(unit);
        }
    });
}

void SBP2NubPublisher::QueueTerminate(std::shared_ptr<Discovery::FWUnit> unit) {
    if (!IsSBP2Unit(unit) || workQueue_ == nullptr) {
        return;
    }
    const auto weak = weak_from_this();
    workQueue_->DispatchAsync(^{
        if (const auto self = weak.lock()) {
            self->TerminateNub(unit, "unit-removed");
        }
    });
}

void SBP2NubPublisher::EnsureNub(const std::shared_ptr<Discovery::FWUnit>& unit) noexcept {
    const auto key = MakeKey(unit);
    if (!key || lock_ == nullptr || driver_ == nullptr) {
        return;
    }

    ASFWSBP2Nub* replaced = nullptr;
    IOLockLock(lock_);
    if (stopping_) {
        IOLockUnlock(lock_);
        return;
    }
    const auto existing = nubsByUnit_.find(*key);
    if (existing != nubsByUnit_.end()) {
        if (existing->second.unit.lock() == unit) {
            IOLockUnlock(lock_);
            return;
        }
        replaced = existing->second.nub;
        nubsByUnit_.erase(existing);
    }
    nubsByUnit_.emplace(*key, PublishedNub{.unit = unit, .nub = nullptr});
    IOLockUnlock(lock_);

    if (replaced != nullptr) {
        // Same GUID/unit directory after a real replug: the old service
        // incarnation cannot represent the new FWUnit.
        replaced->Terminate(0);
    }

    IOService* nubService = nullptr;
    const kern_return_t kr = driver_->Create(driver_, "ASFWSBP2NubProperties", &nubService);
    if (kr != kIOReturnSuccess || nubService == nullptr) {
        ASFW_LOG_ERROR(Controller,
                       "[SBP2Nub] creation failed instance=%llu unitOffset=%u kr=0x%x",
                       key->device.value, key->unitDirectoryOffset, kr);
        TerminateNub(unit, "create-failed");
        return;
    }

    bool propertiesReady = false;
    OSDictionary* propertiesRaw = nullptr;
    if (nubService->CopyProperties(&propertiesRaw) == kIOReturnSuccess && propertiesRaw != nullptr) {
        OSSharedPtr<OSDictionary> properties(propertiesRaw, OSNoRetain);
        const auto device = unit->GetDevice();
        auto instance = OSSharedPtr(OSNumber::withNumber(key->device.value, 64), OSNoRetain);
        auto observedGuid = OSSharedPtr(
            OSNumber::withNumber(device ? device->GetObservedGuid() : 0, 64), OSNoRetain);
        auto directory = OSSharedPtr(
            OSNumber::withNumber(key->unitDirectoryOffset, 32), OSNoRetain);
        if (instance && observedGuid && directory) {
            properties->setObject(kDeviceInstanceIdProperty, instance.get());
            properties->setObject(kObservedGuidProperty, observedGuid.get());
            properties->setObject(kUnitDirectoryOffsetProperty, directory.get());
            if (const auto lun = unit->GetLUN()) {
                if (auto lunNumber = OSSharedPtr(OSNumber::withNumber(*lun, 32), OSNoRetain)) {
                    properties->setObject(kLogicalUnitNumberProperty, lunNumber.get());
                }
            }
            propertiesReady = nubService->SetProperties(properties.get()) == kIOReturnSuccess;
        }
    }
    if (!propertiesReady) {
        ASFW_LOG_ERROR(Controller,
                       "[SBP2Nub] property setup failed instance=%llu unitOffset=%u",
                       key->device.value, key->unitDirectoryOffset);
        nubService->release();
        TerminateNub(unit, "property-failed");
        return;
    }

    ASFWSBP2Nub* nub = OSDynamicCast(ASFWSBP2Nub, nubService);
    if (nub == nullptr) {
        ASFW_LOG_ERROR(Controller,
                       "[SBP2Nub] created wrong service type instance=%llu unitOffset=%u",
                       key->device.value, key->unitDirectoryOffset);
        nubService->release();
        TerminateNub(unit, "type-failed");
        return;
    }

    bool terminateCreated = false;
    IOLockLock(lock_);
    const auto published = nubsByUnit_.find(*key);
    if (stopping_ || published == nubsByUnit_.end() || published->second.unit.lock() != unit) {
        terminateCreated = true;
    } else {
        published->second.nub = nub;
    }
    IOLockUnlock(lock_);

    nubService->release();
    if (terminateCreated) {
        nub->Terminate(0);
        return;
    }

    ASFW_LOG(Controller,
             "[SBP2Nub] published instance=%llu unitOffset=%u",
             key->device.value, key->unitDirectoryOffset);
}

void SBP2NubPublisher::TerminateNub(const std::shared_ptr<Discovery::FWUnit>& unit,
                                    const char* reasonTag) noexcept {
    const auto key = MakeKey(unit);
    if (!key || lock_ == nullptr) {
        return;
    }

    ASFWSBP2Nub* nub = nullptr;
    IOLockLock(lock_);
    const auto published = nubsByUnit_.find(*key);
    if (published != nubsByUnit_.end() && published->second.unit.lock() == unit) {
        nub = published->second.nub;
        nubsByUnit_.erase(published);
    }
    IOLockUnlock(lock_);

    if (nub != nullptr) {
        ASFW_LOG(Controller,
                 "[SBP2Nub] terminating instance=%llu unitOffset=%u (%{public}s)",
                 key->device.value, key->unitDirectoryOffset,
                 reasonTag ? reasonTag : "unknown");
        nub->Terminate(0);
    }
}

} // namespace ASFW::Protocols::SBP2
