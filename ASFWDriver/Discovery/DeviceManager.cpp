#include "DeviceManager.hpp"
#include <algorithm>

namespace ASFW::Discovery {

namespace {

constexpr uint32_t kSBP2UnitSpecId = 0x00609E;
constexpr uint32_t kSBP2UnitSwVersion = 0x010483;
constexpr uint8_t kSBP2MissingTerminateThreshold = 2;

bool HasSBP2Unit(const std::shared_ptr<FWDevice>& device)
{
    if (!device) {
        return false;
    }

    for (const auto& unit : device->GetUnits()) {
        if (unit && unit->Matches(kSBP2UnitSpecId, kSBP2UnitSwVersion)) {
            return true;
        }
    }
    return false;
}

} // namespace

DeviceManager::DeviceManager() : mutex_(IOLockAlloc()) {
    if (!mutex_) {
        // Handle allocation failure - in DriverKit, this might panic or we need to handle it
        // For now, assume it succeeds
    }
}

DeviceManager::~DeviceManager() {
    // Terminate all devices on shutdown
    for (auto& [instanceId, device] : devicesByInstance_) {
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

    for (const auto& [instanceId, device] : devicesByInstance_) {
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

    for (const auto& [instanceId, device] : devicesByInstance_) {
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

    for (const auto& [instanceId, device] : devicesByInstance_) {
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
    for (const auto& [instanceId, device] : devicesByInstance_) {
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

std::shared_ptr<FWDevice> DeviceManager::GetDevice(DeviceInstanceId instanceId) const
{
    IOLockLock(mutex_);

    auto it = devicesByInstance_.find(instanceId);
    if (it != devicesByInstance_.end()) {
        auto device = it->second;
        IOLockUnlock(mutex_);
        return device;
    }

    IOLockUnlock(mutex_);
    return nullptr;
}

std::vector<std::shared_ptr<FWDevice>>
DeviceManager::FindDevicesByObservedGuid(Guid64 observedGuid) const
{
    IOLockLock(mutex_);
    std::vector<std::shared_ptr<FWDevice>> matches;
    for (const auto& [instanceId, device] : devicesByInstance_) {
        if (device && !device->IsTerminated() && device->GetObservedGuid() == observedGuid) {
            matches.push_back(device);
        }
    }
    IOLockUnlock(mutex_);
    return matches;
}

std::shared_ptr<FWDevice> DeviceManager::GetDeviceByNode(
    Generation gen,
    uint8_t nodeId) const
{
    IOLockLock(mutex_);
    GenNodeKey key = MakeKey(gen, nodeId);
    auto it = genNodeToInstance_.find(key);
    if (it == genNodeToInstance_.end()) {
        IOLockUnlock(mutex_);
        return nullptr;
    }

    const DeviceInstanceId instanceId = it->second;
    auto devIt = devicesByInstance_.find(instanceId);
    if (devIt != devicesByInstance_.end()) {
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

    for (const auto& [instanceId, device] : devicesByInstance_) {
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

    for (const auto& [instanceId, device] : devicesByInstance_) {
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

    for (const auto& [instanceId, device] : devicesByInstance_) {
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
    const DeviceInstanceId instanceId = record.instanceId;
    // Reset main's SBP2 missing-scan counter when a device is (re)upserted so a
    // device that reappears is not prematurely terminated (see MarkDeviceLost).
    missingScanCounts_.erase(instanceId);

    if (auto it = devicesByInstance_.find(instanceId); it != devicesByInstance_.end()) {
        if (auto device = ResumeExistingDevice(it->second, record)) {
            IOLockUnlock(mutex_);
            return device;
        }
    }

    auto device = CreateAndRegisterDevice(record, rom);
    IOLockUnlock(mutex_);
    return device;
}

void DeviceManager::MarkDeviceLost(DeviceInstanceId instanceId)
{
    IOLockLock(mutex_);
    auto it = devicesByInstance_.find(instanceId);
    if (it == devicesByInstance_.end()) {
        IOLockUnlock(mutex_);
        return;
    }

    auto device = it->second;
    if (!device || device->IsTerminated()) {
        IOLockUnlock(mutex_);
        return;
    }

    if (!HasSBP2Unit(device)) {
        missingScanCounts_.erase(instanceId);
        IOLockUnlock(mutex_);
        TerminateDevice(instanceId);
        return;
    }

    const uint8_t missingCount = ++missingScanCounts_[instanceId];
    if (missingCount >= kSBP2MissingTerminateThreshold) {
        IOLockUnlock(mutex_);
        TerminateDevice(instanceId);
        return;
    }

    if (device->IsReady()) {
        device->Suspend();
        NotifyDeviceSuspended(device);
        for (const auto& unit : device->GetUnits()) {
            if (unit && unit->IsSuspended()) {
                NotifyUnitSuspended(unit);
            }
        }
    }

    auto genNodeIt = genNodeToInstance_.begin();
    while (genNodeIt != genNodeToInstance_.end()) {
        if (genNodeIt->second == instanceId) {
            genNodeIt = genNodeToInstance_.erase(genNodeIt);
        } else {
            ++genNodeIt;
        }
    }

    IOLockUnlock(mutex_);
}

void DeviceManager::SuspendAllForBusReset()
{
    IOLockLock(mutex_);

    size_t suspendedCount = 0;
    for (const auto& entry : devicesByInstance_) {
        const auto& device = entry.second;
        if (!device || device->IsTerminated() || !device->IsReady()) {
            continue;
        }

        device->Suspend();
        ++suspendedCount;
        NotifyDeviceSuspended(device);
        for (const auto& unit : device->GetUnits()) {
            if (unit && unit->IsSuspended()) {
                NotifyUnitSuspended(unit);
            }
        }
    }

    // A reset invalidates all node IDs, including those whose numerical node
    // number happens not to change after the topology is rediscovered.
    genNodeToInstance_.clear();
    missingScanCounts_.clear();
    IOLockUnlock(mutex_);

    ASFW_LOG(Discovery, "Bus reset: suspended %zu published devices pending discovery", suspendedCount);
}

void DeviceManager::TerminateDevice(DeviceInstanceId instanceId)
{
    IOLockLock(mutex_);
    auto it = devicesByInstance_.find(instanceId);
    if (it == devicesByInstance_.end()) {
        IOLockUnlock(mutex_);
        return;
    }

    auto device = it->second;
    if (!device) {
        missingScanCounts_.erase(instanceId);
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
    auto genNodeIt = genNodeToInstance_.begin();
    while (genNodeIt != genNodeToInstance_.end()) {
        if (genNodeIt->second == instanceId) {
            genNodeIt = genNodeToInstance_.erase(genNodeIt);
        } else {
            ++genNodeIt;
        }
    }

    // Notify observers
    NotifyDeviceRemoved(instanceId);

    // Remove from primary map
    devicesByInstance_.erase(it);
    missingScanCounts_.erase(instanceId);

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

void DeviceManager::NotifyDeviceRemoved(DeviceInstanceId instanceId)
{
    for (auto* observer : deviceObservers_) {
        observer->OnDeviceRemoved(instanceId);
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

void DeviceManager::UpdateOperationalIndex(DeviceInstanceId instanceId,
                                           Generation gen,
                                           uint16_t nodeId,
                                           const char* action)
{
    if (const auto operationalNodeId = TryOperationalNodeId(nodeId); operationalNodeId.has_value()) {
        const GenNodeKey key = MakeKey(gen, *operationalNodeId);
        genNodeToInstance_[key] = instanceId;
        return;
    }

    ASFW_LOG(Discovery,
             "Skipping device index %{public}s for instance=%llu with invalid nodeId=%u",
             action, instanceId.value, nodeId);
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
        UpdateOperationalIndex(record.instanceId, record.gen, record.nodeId, "update");
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

    devicesByInstance_[record.instanceId] = device;
    UpdateOperationalIndex(record.instanceId, record.gen, record.nodeId, "insert");
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
