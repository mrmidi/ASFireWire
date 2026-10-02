//
// AVCDiscovery.cpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C Discovery implementation
//

#include "AVCDiscovery.hpp"
#include "../../Discovery/DeviceRegistry.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Discovery/DiscoveryTypes.hpp"
#include <DriverKit/IOService.h>
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/OSString.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSArray.h>
#include <DriverKit/OSDictionary.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>

using namespace ASFW::Protocols::AVC;

//==============================================================================
// Constants
//==============================================================================


//==============================================================================
// Constructor / Destructor
//==============================================================================

AVCDiscovery::AVCDiscovery(IOService* driver,
                           Discovery::DeviceRegistry& deviceRegistry,
                           Discovery::IDeviceManager& deviceManager,
                           Protocols::Ports::FireWireBusOps& busOps,
                           Protocols::Ports::FireWireBusInfo& busInfo,
                           Scheduling::ITimerScheduler& timerScheduler,
                           std::shared_ptr<DiscoveryOwner> owner)
    : driver_(driver)
    , deviceRegistry_(deviceRegistry)
    , deviceManager_(deviceManager)
    , busOps_(busOps)
    , busInfo_(busInfo)
    , timerScheduler_(timerScheduler)
    , owner_(std::move(owner)) {

    // Allocate lock
    lock_ = IOLockAlloc();
    if (!lock_) {
        os_log_error(log_, "AVCDiscovery: Failed to allocate lock");
    }

    // The injected timer is prepared on ctx.workQueue (DriverContext.cpp).
    // Keep discovery there with FCP delivery and teardown: LiveRef is a serial
    // queue lifetime guard, not synchronization across a separate rescan queue.

    // Register as discovery observers
    deviceManager_.RegisterUnitObserver(this);
    deviceManager_.RegisterDeviceObserver(this);

    os_log_info(log_, "AVCDiscovery: Initialized");
}

AVCDiscovery::~AVCDiscovery() {
    Shutdown();

    // Shutdown() unregisters observers before stopping outstanding FCP work.
    // Do not free the lock until that lifecycle boundary has been established.

    // Clean up lock
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }

    os_log_info(log_, "AVCDiscovery: Destroyed");
}

void AVCDiscovery::Shutdown() {
    if (owner_) owner_->Shutdown();
    bool expected = false;
    if (!shuttingDown_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    // Remove external producers first. Device callbacks may otherwise enqueue
    // a fresh AV/C command after FCP has been shut down.
    deviceManager_.UnregisterDeviceObserver(this);
    deviceManager_.UnregisterUnitObserver(this);

    std::vector<std::shared_ptr<AVCUnit>> units;
    std::vector<Scheduling::TimerToken> tokensToCancel;
    if (lock_) {
        IOLockLock(lock_);
        units.reserve(units_.size());
        for (const auto& [guid, unit] : units_) {
            (void)guid;
            if (unit) {
                units.push_back(unit);
            }
        }
        for (const auto& [guid, token] : rescanTimersByGuid_) {
            (void)guid;
            if (token != Scheduling::kInvalidTimerToken) {
                tokensToCancel.push_back(token);
            }
        }
        rescanTimersByGuid_.clear();
        activeRescanSerialByGuid_.clear();
        fcpTransportsByNodeID_.clear();
        rescanAttempts_.clear();
        IOLockUnlock(lock_);
    }

    for (auto token : tokensToCancel) {
        timerScheduler_.Cancel(token);
    }

    for (const auto& unit : units) {
        unit->Shutdown();
    }
}

//==============================================================================
// IUnitObserver Interface
//==============================================================================

void AVCDiscovery::OnUnitPublished(std::shared_ptr<Discovery::FWUnit> unit) {
    if (shuttingDown_.load(std::memory_order_acquire) || !unit || !IsAVCUnit(unit) || !owner_) return;
    const auto device = unit->GetDevice();
    const auto route = device ? deviceRegistry_.CurrentRoute(device->GetGUID()) : std::nullopt;
    if (!route) return;
    const auto weak = weak_from_this();
    // No transport producer exists until preparation has completed. Revalidate
    // the exact route again in the completion before constructing the unit.
    owner_->PrepareProducer(unit, [weak, unit, route = *route](bool ready) {
        const auto self = weak.lock();
        if (!ready || !self || self->shuttingDown_.load(std::memory_order_acquire) ||
            !self->deviceRegistry_.IsCurrent(route)) return;
        self->OnPreparedUnit(unit);
    });
}

void AVCDiscovery::OnPreparedUnit(std::shared_ptr<Discovery::FWUnit> unit) {
    const auto device = unit->GetDevice(); if (!device || !owner_) return;
    const auto guid = device->GetGUID();
    const auto route = deviceRegistry_.CurrentRoute(guid);
    if (!route || !deviceRegistry_.IsCurrent(*route) || route->generation != device->GetGeneration() ||
        route->nodeId != device->GetNodeID()) return;
    auto avcUnit = std::make_shared<AVCUnit>(device, unit, deviceRegistry_, busOps_, busInfo_,
                                          timerScheduler_, owner_->OptionsFor(*unit));
    IOLockLock(lock_);
    if (shuttingDown_.load(std::memory_order_acquire) || units_.contains(guid)) {
        IOLockUnlock(lock_); avcUnit->Shutdown(); return;
    }
    units_[guid] = avcUnit;
    IOLockUnlock(lock_);
    owner_->UnitCreated(avcUnit);
    RebuildNodeIDMap();
    if (!owner_->AllowsDiscovery(*unit)) return;
    const auto weak = weak_from_this();
    // Weak: the unit owns this completion through its session, so a strong
    // capture would keep a terminated unit alive until discovery finishes.
    avcUnit->Initialize([weak, weakUnit = std::weak_ptr<AVCUnit>(avcUnit)](bool success) {
        const auto self = weak.lock(); const auto unit = weakUnit.lock();
        if (self && unit && !self->shuttingDown_.load(std::memory_order_acquire) && self->owner_)
            self->owner_->UnitCompleted(unit, success);
    });
}


void AVCDiscovery::ScheduleRescan(uint64_t guid, const std::shared_ptr<AVCUnit>& avcUnit) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    if (!avcUnit) {
        return;
    }
    const auto route = deviceRegistry_.CurrentRoute(guid);
    if (!route.has_value()) {
        return;
    }

    constexpr uint8_t kMaxAutoRescanAttempts = 1;
    constexpr uint32_t kRescanDelayMs = 250;

    uint8_t attempt = 0;
    uint64_t operationSerial = 0;
    Scheduling::TimerToken oldRescanToken = Scheduling::kInvalidTimerToken;
    IOLockLock(lock_);
    auto rescanIt = rescanTimersByGuid_.find(guid);
    if (rescanIt != rescanTimersByGuid_.end()) {
        oldRescanToken = rescanIt->second;
        rescanTimersByGuid_.erase(rescanIt);
    }
    auto& count = rescanAttempts_[guid];
    if (count >= kMaxAutoRescanAttempts) {
        IOLockUnlock(lock_);
        if (oldRescanToken != Scheduling::kInvalidTimerToken) {
            timerScheduler_.Cancel(oldRescanToken);
        }
        ASFW_LOG(Audio,
                 "AVCDiscovery: Auto re-scan limit reached for GUID=%llx (attempts=%u)",
                 guid, count);
        return;
    }
    count++;
    attempt = count;
    operationSerial = ++nextRescanOperationSerial_;
    activeRescanSerialByGuid_[guid] = operationSerial;
    IOLockUnlock(lock_);

    if (oldRescanToken != Scheduling::kInvalidTimerToken) {
        timerScheduler_.Cancel(oldRescanToken);
    }

    auto unit = avcUnit;
    const std::weak_ptr<AVCDiscovery> weakSelf = weak_from_this();
    const auto token = timerScheduler_.ScheduleAfter(
        static_cast<uint64_t>(kRescanDelayMs) * 1000000ULL,
        [weakSelf, route = *route, operationSerial, attempt, unit]() {
            const auto self = weakSelf.lock();
            if (!self || !self->IsRescanCurrent(route, operationSerial)) {
                return;
            }

            if (self->lock_) {
                IOLockLock(self->lock_);
                self->rescanTimersByGuid_.erase(route.guid);
                IOLockUnlock(self->lock_);
            }

            auto work = [weakSelf, route, operationSerial, attempt, unit]() {
                const auto self = weakSelf.lock();
                if (!self || !self->IsRescanCurrent(route, operationSerial)) {
                    return;
                }

                ASFW_LOG(Audio, "AVCDiscovery: Auto re-scan attempt %u for GUID=%llx", attempt, route.guid);
                unit->ReScan([weakSelf, route, operationSerial, weakUnit = std::weak_ptr<AVCUnit>(unit)](bool success) {
                    const auto self = weakSelf.lock();
                    const auto unit = weakUnit.lock();
                    if (!self || !unit || !self->IsRescanCurrent(route, operationSerial)) {
                        return;
                    }

                    if (!success) {
                        if (self->owner_) self->owner_->UnitCompleted(unit, false);
                        ASFW_LOG_ERROR(Audio,
                                       "AVCDiscovery: AVCUnit re-scan failed GUID=%llx",
                                       route.guid);
                        return;
                    }

                    if (self->owner_) self->owner_->UnitCompleted(unit, true);
                });
            };

            work();
        });

    if (lock_) {
        IOLockLock(lock_);
        rescanTimersByGuid_[guid] = token;
        IOLockUnlock(lock_);
    }
}

void AVCDiscovery::OnUnitSuspended(std::shared_ptr<Discovery::FWUnit> unit) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    uint64_t guid = GetUnitGUID(unit);

    IOLockLock(lock_);
    auto it = units_.find(guid);
    if (it != units_.end()) {
        os_log_info(log_,
                    "AVCDiscovery: AV/C unit suspended: GUID=%llx",
                    guid);
        // Unit remains in map but operations will fail until resumed
    }
    IOLockUnlock(lock_);

    // Rebuild node ID map (suspended units removed from routing)
    RebuildNodeIDMap();
}

void AVCDiscovery::OnUnitResumed(std::shared_ptr<Discovery::FWUnit> unit) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    uint64_t guid = GetUnitGUID(unit);

    std::shared_ptr<AVCUnit> avcUnit;
    IOLockLock(lock_);
    auto it = units_.find(guid);
    if (it != units_.end()) {
        avcUnit = it->second;
        os_log_info(log_,
                    "AVCDiscovery: AV/C unit resumed: GUID=%llx",
                    guid);
        // Unit is now available again
    }
    IOLockUnlock(lock_);

    if (!avcUnit) { OnUnitPublished(unit); return; }
    if (avcUnit) {
        avcUnit->OnRouteRevalidated();
        // An attach that failed (a device that reset the bus mid-discovery, as a
        // crashing Phase 88 does) never published an audio device. Run it once
        // more now that the unit is back.
        if (avcUnit->GetDiscoveryStatus() == AVCDiscoveryStatus::Failed) {
            ASFW_LOG(AVC, "AVCDiscovery: unit resumed after a failed attach; rescanning GUID=%llx", guid);
            ScheduleRescan(guid, avcUnit);
        }
    }

    // Rebuild node ID map (resumed units back in routing)
    RebuildNodeIDMap();
}

void AVCDiscovery::OnUnitTerminated(std::shared_ptr<Discovery::FWUnit> unit) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    uint64_t guid = GetUnitGUID(unit);

    std::shared_ptr<AVCUnit> avcUnit;
    IOLockLock(lock_);

    auto it = units_.find(guid);
    if (it != units_.end()) {
        avcUnit = it->second;
        os_log_info(log_,
                    "AVCDiscovery: AV/C unit terminated: GUID=%llx",
                    guid);
        units_.erase(it);
    }
    rescanAttempts_.erase(guid);
    IOLockUnlock(lock_);

    // A response-router lease may keep the transport alive after its unit has
    // left discovery. Stop it explicitly so no pending callback survives the
    // unit-removal lifecycle boundary.
    if (avcUnit) {
        avcUnit->Shutdown();
    }

    // Rebuild node ID map (terminated unit removed)
    RebuildNodeIDMap();
}

void AVCDiscovery::OnDeviceAdded(std::shared_ptr<Discovery::FWDevice> device) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    if (owner_) owner_->DeviceAdded(device);
}

void AVCDiscovery::OnDeviceResumed(std::shared_ptr<Discovery::FWDevice> device) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    if (owner_) owner_->DeviceAdded(device);
}


void AVCDiscovery::OnDeviceSuspended(std::shared_ptr<Discovery::FWDevice> device) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    (void)device;
}

void AVCDiscovery::OnDeviceRemoved(Discovery::Guid64 guid) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    std::shared_ptr<AVCUnit> avcUnit;
    IOLockLock(lock_);

    const auto it = units_.find(guid);
    if (it != units_.end()) {
        avcUnit = it->second;
        units_.erase(it);
    }
    rescanAttempts_.erase(guid);
    IOLockUnlock(lock_);

    if (avcUnit) {
        avcUnit->Shutdown();
    }

    RebuildNodeIDMap();
}

//==============================================================================
// Public API
//==============================================================================

AVCUnit* AVCDiscovery::GetAVCUnit(uint64_t guid) {
    IOLockLock(lock_);

    auto it = units_.find(guid);
    AVCUnit* result = (it != units_.end()) ? it->second.get() : nullptr;

    IOLockUnlock(lock_);

    return result;
}

AVCUnit* AVCDiscovery::GetAVCUnit(std::shared_ptr<Discovery::FWUnit> unit) {
    if (!unit) {
        return nullptr;
    }

    uint64_t guid = GetUnitGUID(unit);
    return GetAVCUnit(guid);
}

std::vector<AVCUnit*> AVCDiscovery::GetAllAVCUnits() {
    IOLockLock(lock_);

    std::vector<AVCUnit*> result;
    result.reserve(units_.size());

    for (auto& [guid, avcUnit] : units_) {
        result.push_back(avcUnit.get());
    }

    IOLockUnlock(lock_);

    return result;
}

void AVCDiscovery::ReScanAllUnits() {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<std::pair<uint64_t, std::shared_ptr<AVCUnit>>> scanUnits;
    IOLockLock(lock_);
    scanUnits.reserve(units_.size());
    for (const auto& [guid, avcUnit] : units_) {
        if (avcUnit) scanUnits.emplace_back(guid, avcUnit);
    }
    rescanAttempts_.clear();
    IOLockUnlock(lock_);

    ASFW_LOG(AVC, "[AVCDiag] manual discovery requested units=%zu", scanUnits.size());
    // Each unit reruns its bring-up -- the same commands attach sends, the
    // extension inventory included -- into a fresh exchange log, so the report
    // shows the complete discovery. Devices whose policy forbids discovery
    // traffic are sent nothing and keep their existing log.
    std::vector<std::pair<uint64_t, std::shared_ptr<AVCUnit>>> eligible;
    for (const auto& [guid, avcUnit] : scanUnits) {
        const auto unit = avcUnit->GetFWUnit();
        if (!owner_ || !unit || !owner_->AllowsDiscovery(*unit)) {
            avcUnit->MarkRescanSkipped();
            continue;
        }
        if (avcUnit->TryBeginRescan()) {
            avcUnit->BeginExchangeSession();
            eligible.emplace_back(guid, avcUnit);
        }
    }
    // Publish every unit's synchronous state before any async probe can finish.
    for (const auto& [guid, avcUnit] : eligible) {
        avcUnit->ReScanAlreadyBegun([guid](bool success) {
            ASFW_LOG(AVC, "[AVCDiag] GUID=%llx result=%{public}s", guid, success ? "completed" : "failed");
            // Per-unit status is finalized before this callback; no discovery lock is held.
        });
    }
}

FCPTransport* AVCDiscovery::GetFCPTransportForNodeID(uint16_t nodeID) {
    // Legacy borrowing API. New asynchronous callers must use Acquire...()
    // and retain the returned shared owner across their complete operation.
    const auto transport = AcquireFCPTransportForNodeID(nodeID);
    return transport.get();
}

std::shared_ptr<FCPTransport> AVCDiscovery::AcquireFCPTransportForNodeID(uint16_t nodeID) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return nullptr;
    }
    IOLockLock(lock_);

    // Normalize to node number (low 6 bits) to match map keys
    const uint16_t nodeNumber = static_cast<uint16_t>(nodeID & 0x3Fu);

    auto it = fcpTransportsByNodeID_.find(nodeNumber);
    std::shared_ptr<FCPTransport> result = (it != fcpTransportsByNodeID_.end())
                                                ? it->second
                                                : nullptr;

    IOLockUnlock(lock_);

    return result;
}

//==============================================================================
// Bus Reset Handling
//==============================================================================

void AVCDiscovery::OnBusReset(uint32_t newGeneration) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    os_log_info(log_,
                "AVCDiscovery: Bus reset (generation %u)",
                newGeneration);

    // ControllerCore invalidates DeviceRegistry routes before this callback.
    std::vector<Scheduling::TimerToken> cancelledRescans;

    // Invalidate in-flight rescans first, then notify the units with the lock
    // released: a unit fails its pending FCP command on reset, and the whole
    // discovery chain then unwinds synchronously into completions that take
    // lock_ (IsRescanCurrent). Notifying under the lock aborted the dext on a
    // recursive os_unfair_lock when a Phase 88 reset mid-rescan.
    std::vector<std::shared_ptr<AVCUnit>> units;
    IOLockLock(lock_);
    activeRescanSerialByGuid_.clear();
    for (const auto& [guid, token] : rescanTimersByGuid_) {
        (void)guid;
        if (token != Scheduling::kInvalidTimerToken) {
            cancelledRescans.push_back(token);
        }
    }
    rescanTimersByGuid_.clear();
    units.reserve(units_.size());
    for (const auto& [guid, avcUnit] : units_) {
        (void)guid;
        units.push_back(avcUnit);
    }
    IOLockUnlock(lock_);

    for (const auto token : cancelledRescans) {
        timerScheduler_.Cancel(token);
    }
    for (const auto& avcUnit : units) {
        avcUnit->OnBusReset(newGeneration);
    }

    // Rebuild node ID map (node IDs changed)
    RebuildNodeIDMap();
}

bool AVCDiscovery::IsRescanCurrent(const Discovery::DeviceRouteToken& route,
                                   uint64_t operationSerial) const noexcept {
    if (!route || operationSerial == 0 || shuttingDown_.load(std::memory_order_acquire) || !lock_) {
        return false;
    }

    IOLockLock(lock_);
    const auto it = activeRescanSerialByGuid_.find(route.guid);
    const bool active = it != activeRescanSerialByGuid_.end() && it->second == operationSerial;
    IOLockUnlock(lock_);
    return active && deviceRegistry_.IsCurrent(route);
}

//==============================================================================
// Private Helpers
//==============================================================================

bool AVCDiscovery::IsAVCUnit(std::shared_ptr<Discovery::FWUnit> unit) const {
    if (!unit) {
        return false;
    }

    return (unit->GetUnitSpecID() & 0xFFFFFFu) == ASFW::AVC::kTa1394SpecifierId;
}

uint64_t AVCDiscovery::GetUnitGUID(std::shared_ptr<Discovery::FWUnit> unit) const {
    if (!unit) {
        return 0;
    }

    auto device = unit->GetDevice();
    if (!device) {
        return 0;
    }

    return device->GetGUID();
}

void AVCDiscovery::RebuildNodeIDMap() {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    IOLockLock(lock_);

    // Clear old mappings
    fcpTransportsByNodeID_.clear();

    // Rebuild from current units
    for (auto& [guid, avcUnit] : units_) {
        auto device = avcUnit->GetDevice();
        if (!device) {
            continue;  // Device destroyed
        }

        auto unit = avcUnit->GetFWUnit();
        if (!unit || !unit->IsReady()) {
            continue;  // Unit suspended or terminated
        }

        // Normalize to node number (low 6 bits) to tolerate full vs short IDs
        const uint16_t fullNodeID = device->GetNodeID();
        const uint16_t nodeNumber = static_cast<uint16_t>(fullNodeID & 0x3Fu);
        
        auto transport = avcUnit->GetFCPTransportShared();
        if (!transport) {
            continue;
        }
        fcpTransportsByNodeID_[nodeNumber] = std::move(transport);

        os_log_debug(log_,
                     "AVCDiscovery: Mapped fullNodeID=0x%04x (node=%u) → FCPTransport (GUID=%llx)",
                     fullNodeID, nodeNumber, guid);
    }

    IOLockUnlock(lock_);
}
