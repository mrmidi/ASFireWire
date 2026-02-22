#include "DeviceManager.hpp"
#include <algorithm>

namespace ASFW::Discovery {

DeviceManager::DeviceManager() : mutex_(IOLockAlloc()) {
    if (!mutex_) {
        // Handle allocation failure - in DriverKit, this might panic or we need to handle it
        // For now, assume it succeeds
    }
}

DeviceManager::~DeviceManager() {
    // Terminate all devices on shutdown
    for (auto& [guid, device] : devicesByGuid_) {
        if (device) {
            device->Terminate();
        }
    }
    
    if (mutex_) {
        IOLockFree(mutex_);
        mutex_ = nullptr;
    }
}

// === IUnitRegistry Implementation ===

std::vector<std::shared_ptr<FWUnit>> DeviceManager::FindUnitsBySpec(
    uint32_t specId,
    std::optional<uint32_t> swVersion) const
{
    IOLockLock(mutex_);
    std::vector<std::shared_ptr<FWUnit>> matches;

    for (const auto& [guid, device] : devicesByGuid_) {
        if (!device || device->IsTerminated()) {
            continue;
        }

        auto deviceMatches = device->FindUnitsBySpec(specId, swVersion);
        matches.insert(matches.end(), deviceMatches.begin(), deviceMatches.end());
    }

    IOLockUnlock(mutex_);
    return matches;
}

std::vector<std::shared_ptr<FWUnit>> DeviceManager::GetAllUnits() const
{
    IOLockLock(mutex_);
    std::vector<std::shared_ptr<FWUnit>> allUnits;

    for (const auto& [guid, device] : devicesByGuid_) {
        if (!device || device->IsTerminated()) {
            continue;
        }

        const auto& units = device->GetUnits();
        allUnits.insert(allUnits.end(), units.begin(), units.end());
    }

    IOLockUnlock(mutex_);
    return allUnits;
}

std::vector<std::shared_ptr<FWUnit>> DeviceManager::GetReadyUnits() const
{
    IOLockLock(mutex_);
    std::vector<std::shared_ptr<FWUnit>> readyUnits;

    for (const auto& [guid, device] : devicesByGuid_) {
        if (!device || device->IsTerminated()) {
            continue;
        }

        for (const auto& unit : device->GetUnits()) {
            if (unit && unit->IsReady()) {
                readyUnits.push_back(unit);
            }
        }
    }

    IOLockUnlock(mutex_);
    return readyUnits;
}

void DeviceManager::RegisterUnitObserver(IUnitObserver* observer)
{
    if (!observer) {
        return;
    }

    IOLockLock(mutex_);
    unitObservers_.insert(observer);
    IOLockUnlock(mutex_);
}

void DeviceManager::UnregisterUnitObserver(IUnitObserver* observer)
{
    IOLockLock(mutex_);
    unitObservers_.erase(observer);
    IOLockUnlock(mutex_);
}

DeviceManager::CallbackHandle DeviceManager::RegisterUnitCallback(
    uint32_t specId,
    std::optional<uint32_t> swVersion,
    UnitCallback callback)
{
    if (!callback) {
        return 0;
    }

    IOLockLock(mutex_);
    // Assign handle
    CallbackHandle handle = nextCallbackHandle_.fetch_add(1);

    // Store callback
    unitCallbacks_.push_back({handle, specId, swVersion, std::move(callback)});

    // Invoke callback for existing matching units
    auto& entry = unitCallbacks_.back();
    for (const auto& [guid, device] : devicesByGuid_) {
        if (!device || device->IsTerminated()) {
            continue;
        }

        for (const auto& unit : device->GetUnits()) {
            if (unit && unit->IsReady() && UnitMatchesCallback(unit, specId, swVersion)) {
                entry.callback(unit);
            }
        }
    }

    IOLockUnlock(mutex_);
    return handle;
}

void DeviceManager::UnregisterCallback(CallbackHandle handle)
{
    IOLockLock(mutex_);

    auto it = std::remove_if(
        unitCallbacks_.begin(),
        unitCallbacks_.end(),
        [handle](const UnitCallbackEntry& entry) {
            return entry.handle == handle;
        }
    );

    unitCallbacks_.erase(it, unitCallbacks_.end());
    IOLockUnlock(mutex_);
}

// === IDeviceManager Implementation ===

std::shared_ptr<FWDevice> DeviceManager::GetDeviceByGUID(Guid64 guid) const
{
    IOLockLock(mutex_);

    auto it = devicesByGuid_.find(guid);
    if (it != devicesByGuid_.end()) {
        auto device = it->second;
        IOLockUnlock(mutex_);
        return device;
    }

    IOLockUnlock(mutex_);
    return nullptr;
}

std::shared_ptr<FWDevice> DeviceManager::GetDeviceByNode(
    Generation gen,
    uint8_t nodeId) const
{
    IOLockLock(mutex_);
    GenNodeKey key = MakeKey(gen, nodeId);
    auto it = genNodeToGuid_.find(key);
    if (it == genNodeToGuid_.end()) {
        IOLockUnlock(mutex_);
        return nullptr;
    }

    Guid64 guid = it->second;
    auto devIt = devicesByGuid_.find(guid);
    if (devIt != devicesByGuid_.end()) {
        auto device = devIt->second;
        IOLockUnlock(mutex_);
        return device;
    }

    IOLockUnlock(mutex_);
    return nullptr;
}

std::vector<std::shared_ptr<FWDevice>> DeviceManager::GetDevicesByGeneration(
    Generation gen) const
{
    IOLockLock(mutex_);
    std::vector<std::shared_ptr<FWDevice>> devices;

    for (const auto& [guid, device] : devicesByGuid_) {
        if (device && device->GetGeneration() == gen && !device->IsTerminated()) {
            devices.push_back(device);
        }
    }

    IOLockUnlock(mutex_);
    return devices;
}

std::vector<std::shared_ptr<FWDevice>> DeviceManager::GetAllDevices() const
{
    IOLockLock(mutex_);
    std::vector<std::shared_ptr<FWDevice>> devices;

    for (const auto& [guid, device] : devicesByGuid_) {
        if (device && !device->IsTerminated()) {
            devices.push_back(device);
        }
    }

    IOLockUnlock(mutex_);
    return devices;
}

std::vector<std::shared_ptr<FWDevice>> DeviceManager::GetReadyDevices() const
{
    IOLockLock(mutex_);
    std::vector<std::shared_ptr<FWDevice>> devices;

    for (const auto& [guid, device] : devicesByGuid_) {
        if (device && device->IsReady()) {
            devices.push_back(device);
        }
    }

    IOLockUnlock(mutex_);
    return devices;
}

void DeviceManager::RegisterDeviceObserver(IDeviceObserver* observer)
{
    if (!observer) {
        return;
    }

    IOLockLock(mutex_);
    deviceObservers_.insert(observer);
    IOLockUnlock(mutex_);
}

void DeviceManager::UnregisterDeviceObserver(IDeviceObserver* observer)
{
    IOLockLock(mutex_);
    deviceObservers_.erase(observer);
    IOLockUnlock(mutex_);
}

// === Internal API ===

std::shared_ptr<FWDevice> DeviceManager::UpsertDevice(
    const DeviceRecord& record,
    const ConfigROM& rom)
{
    IOLockLock(mutex_);
    Guid64 guid = record.guid;
    auto it = devicesByGuid_.find(guid);

    if (it != devicesByGuid_.end()) {
        // Device exists - resume it
        auto device = it->second;
        if (device) {
            if (device->IsSuspended()) {
                // Resume existing device with new generation info
                device->Resume(record.gen, record.nodeId, record.link);

                // Update secondary index
                GenNodeKey key = MakeKey(record.gen, record.nodeId);
                genNodeToGuid_[key] = guid;

                // Notify observers
                NotifyDeviceResumed(device);

                // Notify unit observers for resumed units
                for (const auto& unit : device->GetUnits()) {
                    if (unit && unit->IsReady()) {
                        NotifyUnitResumed(unit);
                    }
                }
            }
            // If already Ready, this is redundant discovery - ignore
            IOLockUnlock(mutex_);
            return device;
        }
    }

    // New device - create it
    auto device = FWDevice::Create(record, rom);
    if (!device) {
        IOLockUnlock(mutex_);
        return nullptr;
    }

    // Store in primary map
    devicesByGuid_[guid] = device;

    // Store in secondary index
    GenNodeKey key = MakeKey(record.gen, record.nodeId);
    genNodeToGuid_[key] = guid;

    // Publish device and units
    device->Publish();

    // Notify observers
    NotifyDeviceAdded(device);

    // Notify unit observers for published units
    for (const auto& unit : device->GetUnits()) {
        if (unit && unit->IsReady()) {
            NotifyUnitPublished(unit);
        }
    }

    IOLockUnlock(mutex_);
    return device;
}

void DeviceManager::MarkDeviceLost(Guid64 guid)
{
    // Immediate-unplug policy for audio stability/cleanup.
    // TODO: check suspend+timeout policy if needed
    TerminateDevice(guid);
}

void DeviceManager::TerminateDevice(Guid64 guid)
{
    IOLockLock(mutex_);
    auto it = devicesByGuid_.find(guid);
    if (it == devicesByGuid_.end()) {
        IOLockUnlock(mutex_);
        return;
    }

    auto device = it->second;
    if (!device) {
        IOLockUnlock(mutex_);
        return;
    }

    // Notify unit observers for terminated units (before termination)
    for (const auto& unit : device->GetUnits()) {
        if (unit && !unit->IsTerminated()) {
            NotifyUnitTerminated(unit);
        }
    }

    // Terminate device
    device->Terminate();

    // Remove from secondary index
    auto genNodeIt = genNodeToGuid_.begin();
    while (genNodeIt != genNodeToGuid_.end()) {
        if (genNodeIt->second == guid) {
            genNodeIt = genNodeToGuid_.erase(genNodeIt);
        } else {
            ++genNodeIt;
        }
    }

    // Notify observers
    NotifyDeviceRemoved(guid);

    // Remove from primary map
    devicesByGuid_.erase(it);

    IOLockUnlock(mutex_);
}

// === Helper Methods ===

void DeviceManager::NotifyDeviceAdded(std::shared_ptr<FWDevice> device)
{
    for (auto* observer : deviceObservers_) {
        observer->OnDeviceAdded(device);
    }
}

void DeviceManager::NotifyDeviceResumed(std::shared_ptr<FWDevice> device)
{
    for (auto* observer : deviceObservers_) {
        observer->OnDeviceResumed(device);
    }
}

void DeviceManager::NotifyDeviceSuspended(std::shared_ptr<FWDevice> device)
{
    for (auto* observer : deviceObservers_) {
        observer->OnDeviceSuspended(device);
    }
}

void DeviceManager::NotifyDeviceRemoved(Guid64 guid)
{
    for (auto* observer : deviceObservers_) {
        observer->OnDeviceRemoved(guid);
    }
}

void DeviceManager::NotifyUnitPublished(std::shared_ptr<FWUnit> unit)
{
    // Notify observers
    for (auto* observer : unitObservers_) {
        observer->OnUnitPublished(unit);
    }

    // Invoke matching callbacks
    for (auto& entry : unitCallbacks_) {
        if (UnitMatchesCallback(unit, entry.specId, entry.swVersion)) {
            entry.callback(unit);
        }
    }
}

void DeviceManager::NotifyUnitSuspended(std::shared_ptr<FWUnit> unit)
{
    for (auto* observer : unitObservers_) {
        observer->OnUnitSuspended(unit);
    }
}

void DeviceManager::NotifyUnitResumed(std::shared_ptr<FWUnit> unit)
{
    // Notify observers
    for (auto* observer : unitObservers_) {
        observer->OnUnitResumed(unit);
    }

    // Invoke matching callbacks
    for (auto& entry : unitCallbacks_) {
        if (UnitMatchesCallback(unit, entry.specId, entry.swVersion)) {
            entry.callback(unit);
        }
    }
}

void DeviceManager::NotifyUnitTerminated(std::shared_ptr<FWUnit> unit)
{
    for (auto* observer : unitObservers_) {
        observer->OnUnitTerminated(unit);
    }
}

bool DeviceManager::UnitMatchesCallback(
    const std::shared_ptr<FWUnit>& unit,
    uint32_t specId,
    std::optional<uint32_t> swVersion) const
{
    return unit && unit->Matches(specId, swVersion);
}

DeviceManager::GenNodeKey DeviceManager::MakeKey(Generation gen, uint8_t nodeId)
{
    return (static_cast<uint32_t>(gen) << 8) | nodeId;
}

} // namespace ASFW::Discovery
