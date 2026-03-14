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
    const Guid64 guid = record.guid;

    if (auto it = devicesByGuid_.find(guid); it != devicesByGuid_.end()) {
        if (auto device = ResumeExistingDevice(it->second, record)) {
            IOLockUnlock(mutex_);
            return device;
        }
    }

    auto device = CreateAndRegisterDevice(record, rom);
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

void DeviceManager::UpdateOperationalIndex(Guid64 guid,
                                           Generation gen,
                                           uint16_t nodeId,
                                           const char* action)
{
    if (const auto operationalNodeId = TryOperationalNodeId(nodeId); operationalNodeId.has_value()) {
        const GenNodeKey key = MakeKey(gen, *operationalNodeId);
        genNodeToGuid_[key] = guid;
        return;
    }

    ASFW_LOG(Discovery, "Skipping device index %s for GUID=0x%016llx with invalid nodeId=%u",
             action, guid, nodeId);
}

void DeviceManager::NotifyPublishedUnits(const std::shared_ptr<FWDevice>& device)
{
    if (!device) {
        return;
    }

    for (const auto& unit : device->GetUnits()) {
        if (unit && unit->IsReady()) {
            NotifyUnitPublished(unit);
        }
    }
}

void DeviceManager::NotifyResumedUnits(const std::shared_ptr<FWDevice>& device)
{
    if (!device) {
        return;
    }

    for (const auto& unit : device->GetUnits()) {
        if (unit && unit->IsReady()) {
            NotifyUnitResumed(unit);
        }
    }
}

std::shared_ptr<FWDevice> DeviceManager::ResumeExistingDevice(const std::shared_ptr<FWDevice>& device,
                                                              const DeviceRecord& record)
{
    if (!device) {
        return nullptr;
    }

    if (device->IsSuspended()) {
        device->Resume(record.gen, record.nodeId, record.link);
        UpdateOperationalIndex(record.guid, record.gen, record.nodeId, "update");
        NotifyDeviceResumed(device);
        NotifyResumedUnits(device);
    }

    // If already Ready, this is redundant discovery - ignore.
    return device;
}

std::shared_ptr<FWDevice> DeviceManager::CreateAndRegisterDevice(const DeviceRecord& record,
                                                                 const ConfigROM& rom)
{
    auto device = FWDevice::Create(record, rom);
    if (!device) {
        return nullptr;
    }

    devicesByGuid_[record.guid] = device;
    UpdateOperationalIndex(record.guid, record.gen, record.nodeId, "insert");
    device->Publish();
    NotifyDeviceAdded(device);
    NotifyPublishedUnits(device);
    return device;
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
    return (gen.value << 8) | static_cast<uint32_t>(nodeId);
}

} // namespace ASFW::Discovery
