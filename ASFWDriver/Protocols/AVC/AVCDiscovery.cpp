//
// AVCDiscovery.cpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C Discovery implementation
//

#include "AVCDiscovery.hpp"
#include "../../Logging/Logging.hpp"

using namespace ASFW::Protocols::AVC;

//==============================================================================
// Constants
//==============================================================================

/// 1394 Trade Association spec ID (24-bit)
constexpr uint32_t kAVCSpecID = 0x00A02D;

//==============================================================================
// Constructor / Destructor
//==============================================================================

AVCDiscovery::AVCDiscovery(Discovery::IDeviceManager& deviceManager,
                           Async::AsyncSubsystem& asyncSubsystem)
    : deviceManager_(deviceManager), asyncSubsystem_(asyncSubsystem) {

    // Allocate lock
    lock_ = IOLockAlloc();
    if (!lock_) {
        os_log_error(log_, "AVCDiscovery: Failed to allocate lock");
    }

    // Register as unit observer
    deviceManager_.RegisterUnitObserver(this);

    os_log_info(log_, "AVCDiscovery: Initialized");
}

AVCDiscovery::~AVCDiscovery() {
    // Unregister observer
    deviceManager_.UnregisterUnitObserver(this);

    // Clean up lock
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }

    os_log_info(log_, "AVCDiscovery: Destroyed");
}

//==============================================================================
// IUnitObserver Interface
//==============================================================================

void AVCDiscovery::OnUnitPublished(std::shared_ptr<Discovery::FWUnit> unit) {
    if (!IsAVCUnit(unit)) {
        return;
    }

    uint64_t guid = GetUnitGUID(unit);

    ASFW_LOG(Async,
             "⚠️  AV/C DETECTED: GUID=%llx, specID=0x%06x - SCAN DEFERRED",
             guid, unit->GetUnitSpecID());

    /*
    // Get parent device
    auto device = unit->GetDevice();
    if (!device) {
        os_log_error(log_, "AVCDiscovery: Unit has no parent device");
        return;
    }

    // Create AVCUnit
    auto avcUnit = std::make_shared<AVCUnit>(device, unit, asyncSubsystem_);

    // Initialize (probe subunits, plugs)
    auto* avcUnitPtr = avcUnit.get();
    avcUnit->Initialize([this, avcUnitPtr, guid](bool success) {
        if (success) {
            os_log_info(log_,
                        "AVCDiscovery: AVCUnit initialized: GUID=%llx, "
                        "%zu subunits, %d inputs, %d outputs",
                        guid,
                        avcUnitPtr->GetSubunits().size(),
                        avcUnitPtr->IsInitialized() ? 2 : 0,  // Placeholder
                        avcUnitPtr->IsInitialized() ? 2 : 0); // Placeholder
        } else {
            os_log_error(log_,
                         "AVCDiscovery: AVCUnit initialization failed: GUID=%llx",
                         guid);
        }
    });

    // Store AVCUnit
    IOLockLock(lock_);
    units_[guid] = std::move(avcUnit);
    IOLockUnlock(lock_);

    // Rebuild node ID map (unit now has transport)
    RebuildNodeIDMap();
    */
}

void AVCDiscovery::OnUnitSuspended(std::shared_ptr<Discovery::FWUnit> unit) {
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
    uint64_t guid = GetUnitGUID(unit);

    IOLockLock(lock_);
    auto it = units_.find(guid);
    if (it != units_.end()) {
        os_log_info(log_,
                    "AVCDiscovery: AV/C unit resumed: GUID=%llx",
                    guid);
        // Unit is now available again
    }
    IOLockUnlock(lock_);

    // Rebuild node ID map (resumed units back in routing)
    RebuildNodeIDMap();
}

void AVCDiscovery::OnUnitTerminated(std::shared_ptr<Discovery::FWUnit> unit) {
    uint64_t guid = GetUnitGUID(unit);

    IOLockLock(lock_);
    auto it = units_.find(guid);
    if (it != units_.end()) {
        os_log_info(log_,
                    "AVCDiscovery: AV/C unit terminated: GUID=%llx",
                    guid);
        units_.erase(it);
    }
    IOLockUnlock(lock_);

    // Rebuild node ID map (terminated unit removed)
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

FCPTransport* AVCDiscovery::GetFCPTransportForNodeID(uint16_t nodeID) {
    IOLockLock(lock_);

    auto it = fcpTransportsByNodeID_.find(nodeID);
    FCPTransport* result = (it != fcpTransportsByNodeID_.end())
                               ? it->second
                               : nullptr;

    IOLockUnlock(lock_);

    return result;
}

//==============================================================================
// Bus Reset Handling
//==============================================================================

void AVCDiscovery::OnBusReset(uint32_t newGeneration) {
    os_log_info(log_,
                "AVCDiscovery: Bus reset (generation %u)",
                newGeneration);

    // Notify all AVCUnits of bus reset
    IOLockLock(lock_);

    for (auto& [guid, avcUnit] : units_) {
        avcUnit->OnBusReset(newGeneration);
    }

    IOLockUnlock(lock_);

    // Rebuild node ID map (node IDs changed)
    RebuildNodeIDMap();
}

//==============================================================================
// Private Helpers
//==============================================================================

bool AVCDiscovery::IsAVCUnit(std::shared_ptr<Discovery::FWUnit> unit) const {
    if (!unit) {
        return false;
    }

    // Check unit spec ID (24-bit, should be 0x00A02D for AV/C)
    uint32_t specID = unit->GetUnitSpecID() & 0xFFFFFF;

    return specID == kAVCSpecID;
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

        uint16_t nodeID = device->GetNodeID();
        fcpTransportsByNodeID_[nodeID] = &avcUnit->GetFCPTransport();

        os_log_debug(log_,
                     "AVCDiscovery: Mapped nodeID %u → FCPTransport (GUID=%llx)",
                     nodeID, guid);
    }

    IOLockUnlock(lock_);
}
