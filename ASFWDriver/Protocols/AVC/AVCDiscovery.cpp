// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "AVCDiscovery.hpp"

#include "AVCDefs.hpp"
#include "../../Discovery/FWDevice.hpp"
#include "../../Discovery/FWUnit.hpp"
#include "../../Logging/Logging.hpp"

#include <utility>

namespace ASFW::Protocols::AVC {

AVCDiscovery::AVCDiscovery(Discovery::DeviceRegistry& deviceRegistry,
                           Discovery::IDeviceManager& deviceManager,
                           Protocols::Ports::FireWireBusOps& busOps,
                           Protocols::Ports::FireWireBusInfo& busInfo,
                           Scheduling::ITimerScheduler& timerScheduler)
    : deviceRegistry_(deviceRegistry), deviceManager_(deviceManager),
      busOps_(busOps), busInfo_(busInfo), timerScheduler_(timerScheduler),
      lock_(IOLockAlloc()) {
    deviceManager_.RegisterUnitObserver(this);
    for (const auto& unit : deviceManager_.GetReadyUnits()) {
        OnUnitPublished(unit);
    }
}

AVCDiscovery::~AVCDiscovery() {
    Shutdown();
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

bool AVCDiscovery::IsAVCUnit(
    const std::shared_ptr<Discovery::FWUnit>& unit) const noexcept {
    return unit && (unit->GetUnitSpecID() & 0x00FF'FFFFU) == kSpecID_1394TA;
}

// Quarantine is deliberately NOT consulted here. Constructing a transport puts
// no bytes on the wire; what is dangerous is which frames get sent, and that is
// bounded per-device by the permitted-command table below. Refusing the
// transport outright would also refuse the one command such a device tolerates.
std::shared_ptr<FCPTransport> AVCDiscovery::EnsureRemoteTransportLocked(
    const std::shared_ptr<Discovery::FWDevice>& device) {
    if (!device) return nullptr;
    const auto instanceId = device->GetInstanceId();
    if (const auto it = transportsByDevice_.find(instanceId);
        it != transportsByDevice_.end()) {
        return it->second;
    }

    FCPTransportConfig config{};
    config.commandAddress = kFCPCommandAddress;
    config.responseAddress = kFCPResponseAddress;
    config.timeoutMs = kFCPTimeoutInitial;
    config.interimTimeoutMs = kFCPTimeoutAfterInterim;
    config.maxRetries = kFCPMaxRetries;
    config.allowBusResetRetry = false;
    config.permittedFrames = PermittedFramesFor(device->GetAvcCommandFilter());

    auto transport = std::make_shared<FCPTransport>();
    if (!transport ||
        !transport->init(&busOps_, &busInfo_, device.get(), deviceRegistry_,
                         timerScheduler_, config)) {
        return nullptr;
    }
    transportsByDevice_.emplace(instanceId, transport);
    return transport;
}

void AVCDiscovery::OnUnitPublished(std::shared_ptr<Discovery::FWUnit> unit) {
    if (!lock_ || shuttingDown_.load(std::memory_order_acquire) ||
        !IsAVCUnit(unit) || !unit->IsReady()) {
        return;
    }
    auto device = unit->GetDevice();
    if (!device || !device->IsReady() || device->IsQuarantined()) return;
    const auto unitId = unit->GetInstanceId();

    IOLockLock(lock_);
    if (units_.contains(unitId)) {
        IOLockUnlock(lock_);
        return;
    }
    auto transport = EnsureRemoteTransportLocked(device);
    if (!transport) {
        IOLockUnlock(lock_);
        return;
    }
    auto avcUnit = std::make_shared<AVCUnit>(
        device, unit, deviceRegistry_, busOps_, busInfo_, timerScheduler_,
        transport);
    if (avcUnit) units_.emplace(unitId, avcUnit);
    IOLockUnlock(lock_);

    ASFW_LOG(AVC,
             "AVCDiscovery: registered unit device=%llu offset=%u observedGUID=%llx (no probe issued)",
             unitId.device.value, unitId.unitDirectoryOffset,
             device->GetObservedGuid());
}

void AVCDiscovery::OnUnitSuspended(std::shared_ptr<Discovery::FWUnit> unit) {
    if (!unit || !lock_) return;
    const auto unitId = unit->GetInstanceId();
    std::shared_ptr<AVCUnit> avcUnit;
    IOLockLock(lock_);
    if (const auto it = units_.find(unitId); it != units_.end()) avcUnit = it->second;
    IOLockUnlock(lock_);
    if (avcUnit) {
        const auto device = unit->GetDevice();
        avcUnit->OnBusReset(device ? device->GetGeneration().value : 0U);
    }
}

void AVCDiscovery::OnUnitResumed(std::shared_ptr<Discovery::FWUnit> unit) {
    if (!unit || !lock_) return;
    const auto unitId = unit->GetInstanceId();
    std::shared_ptr<AVCUnit> avcUnit;
    IOLockLock(lock_);
    if (const auto it = units_.find(unitId); it != units_.end()) {
        avcUnit = it->second;
    }
    IOLockUnlock(lock_);
    if (avcUnit) avcUnit->OnRouteRevalidated();
    else OnUnitPublished(std::move(unit));
}

void AVCDiscovery::OnUnitTerminated(std::shared_ptr<Discovery::FWUnit> unit) {
    if (!unit || !lock_) return;
    const auto unitId = unit->GetInstanceId();
    std::shared_ptr<AVCUnit> removedUnit;
    std::shared_ptr<FCPTransport> removedTransport;
    IOLockLock(lock_);
    if (const auto it = units_.find(unitId); it != units_.end()) {
        removedUnit = std::move(it->second);
        units_.erase(it);
    }
    bool deviceStillUsed = false;
    for (const auto& [otherId, ignored] : units_) {
        (void)ignored;
        if (otherId.device == unitId.device) {
            deviceStillUsed = true;
            break;
        }
    }
    if (!deviceStillUsed) {
        if (const auto it = transportsByDevice_.find(unitId.device);
            it != transportsByDevice_.end()) {
            removedTransport = std::move(it->second);
            transportsByDevice_.erase(it);
        }
    }
    IOLockUnlock(lock_);
    if (removedUnit) removedUnit->Shutdown();
    if (removedTransport) removedTransport->Shutdown();
}

std::shared_ptr<AVCUnit> AVCDiscovery::AcquireAVCUnit(
    Discovery::UnitInstanceId unitId) noexcept {
    if (!lock_ || !unitId) return nullptr;
    IOLockLock(lock_);
    const auto it = units_.find(unitId);
    auto result = it != units_.end() ? it->second : nullptr;
    IOLockUnlock(lock_);
    return result;
}

std::shared_ptr<RemoteAvcSession> AVCDiscovery::AcquireRemoteSession(
    Discovery::UnitInstanceId unitId) {
    auto unit = AcquireAVCUnit(unitId);
    return unit ? std::make_shared<RemoteAvcSession>(std::move(unit)) : nullptr;
}

std::vector<AVCUnit*> AVCDiscovery::GetAllAVCUnits() {
    std::vector<AVCUnit*> result;
    if (!lock_) return result;
    IOLockLock(lock_);
    result.reserve(units_.size());
    for (const auto& [id, unit] : units_) {
        (void)id;
        if (unit) result.push_back(unit.get());
    }
    IOLockUnlock(lock_);
    return result;
}

void AVCDiscovery::ReScanAllUnits() {
    std::vector<std::shared_ptr<AVCUnit>> units;
    if (!lock_) return;
    IOLockLock(lock_);
    for (const auto& [id, unit] : units_) {
        (void)id;
        if (unit) units.push_back(unit);
    }
    IOLockUnlock(lock_);
    for (const auto& unit : units) unit->ReScan([](bool) {});
}

std::shared_ptr<FCPTransport>
AVCDiscovery::AcquireFCPTransportForIngress(Discovery::Generation generation,
                                             uint16_t sourceId) {
    if (!lock_) return nullptr;

    // Async packet headers carry a 16-bit bus/node source ID. Discovery's
    // generation route table is deliberately keyed by the six-bit physical
    // node number; it resolves that transient address to a DeviceInstanceId.
    const auto record = deviceRegistry_.SnapshotByNode(
        generation, static_cast<uint8_t>(sourceId & 0x3FU));
    if (!record || record->state == Discovery::LifeState::Quarantined) {
        return nullptr;
    }

    IOLockLock(lock_);
    const auto it = transportsByDevice_.find(record->instanceId);
    auto result = it != transportsByDevice_.end() ? it->second : nullptr;
    IOLockUnlock(lock_);
    return result;
}

void AVCDiscovery::OnBusReset(uint32_t newGeneration) {
    std::vector<std::shared_ptr<FCPTransport>> transports;
    if (!lock_) return;
    IOLockLock(lock_);
    for (const auto& [id, transport] : transportsByDevice_) {
        (void)id;
        if (transport) transports.push_back(transport);
    }
    IOLockUnlock(lock_);
    // Pending transactions fail closed across the reset. Linux does the same
    // before stream recovery (references/linux-sound-firewire-stack/firewire/fcp.c:292-314).
    for (const auto& transport : transports) transport->OnBusReset(newGeneration);
}

void AVCDiscovery::Shutdown() {
    if (shuttingDown_.exchange(true, std::memory_order_acq_rel)) return;
    deviceManager_.UnregisterUnitObserver(this);
    std::map<Discovery::UnitInstanceId, std::shared_ptr<AVCUnit>> units;
    decltype(transportsByDevice_) transports;
    if (lock_) {
        IOLockLock(lock_);
        units.swap(units_);
        transports.swap(transportsByDevice_);
        IOLockUnlock(lock_);
    }
    for (auto& [id, unit] : units) {
        (void)id;
        if (unit) unit->Shutdown();
    }
    for (auto& [id, transport] : transports) {
        (void)id;
        if (transport) transport->Shutdown();
    }
}

} // namespace ASFW::Protocols::AVC
