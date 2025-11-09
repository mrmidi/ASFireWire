#include "DeviceRegistry.hpp"
#include <algorithm>
#include "../Logging/Logging.hpp"

namespace ASFW::Discovery {

// Well-known Unit_Spec_Id values for device classification
// IEEE 1394 Trade Association: 0x00A02D
constexpr uint32_t kUnitSpecId_TA = 0x00A02D;

// AV/C specification IDs (used for audio/video devices)
constexpr uint32_t kUnitSpecId_AVC = 0x00A02D;  // Same as TA for AV/C

DeviceRegistry::DeviceRegistry() = default;

DeviceRecord& DeviceRegistry::UpsertFromROM(const ConfigROM& rom, const LinkPolicy& link) {
    const Guid64 guid = rom.bib.guid;
    
    // Find or create device record
    auto& device = devicesByGuid_[guid];
    
    // Update stable identity
    device.guid = guid;

    // Extract vendor ID, model ID, and other metadata from root directory entries
    // NOTE: Vendor ID is in root directory (key 0x03), NOT in BIB per IEEE 1212
    for (const auto& entry : rom.rootDirMinimal) {
        if (entry.key == CfgKey::VendorId) {
            device.vendorId = entry.value;
        } else if (entry.key == CfgKey::ModelId) {
            device.modelId = entry.value;
        } else if (entry.key == CfgKey::Unit_Spec_Id) {
            device.unitSpecId = static_cast<uint8_t>(entry.value & 0xFF);
        } else if (entry.key == CfgKey::Unit_Sw_Version) {
            device.unitSwVersion = static_cast<uint8_t>(entry.value & 0xFF);
        }
    }

    // Copy text descriptors from ROM (vendor/model names from text descriptor leafs)
    device.vendorName = rom.vendorName;
    device.modelName = rom.modelName;
    
    // Classify device
    device.kind = ClassifyDevice(rom);
    device.isAudioCandidate = IsAudioCandidate(rom);
    
    // Update live mapping
    device.gen = rom.gen;
    device.nodeId = rom.nodeId;
    device.link = link;
    device.state = LifeState::Identified;
    
    // Update secondary index
    GenNodeKey key = MakeKey(rom.gen, rom.nodeId);
    genNodeToGuid_[key] = guid;
    
    const char* kindStr = "Unknown";
    switch (device.kind) {
        case DeviceKind::AV_C: kindStr = "AV_C"; break;
        case DeviceKind::TA_61883: kindStr = "TA_61883"; break;
        case DeviceKind::VendorSpecificAudio: kindStr = "VendorAudio"; break;
        case DeviceKind::Storage: kindStr = "Storage"; break;
        case DeviceKind::Camera: kindStr = "Camera"; break;
        default: break;
    }

    // Log device with vendor/model names if available
    if (!device.vendorName.empty() && !device.modelName.empty()) {
        ASFW_LOG(Discovery, "Device upsert: GUID=0x%016llx vendor=0x%06x(%{public}s) model=0x%06x(%{public}s) "
                 "kind=%{public}s audioCandidate=%d node=%u gen=%u",
                 guid, device.vendorId, device.vendorName.c_str(),
                 device.modelId, device.modelName.c_str(), kindStr,
                 device.isAudioCandidate, rom.nodeId, rom.gen);
    } else {
        ASFW_LOG(Discovery, "Device upsert: GUID=0x%016llx vendor=0x%06x model=0x%06x "
                 "kind=%{public}s audioCandidate=%d node=%u gen=%u",
                 guid, device.vendorId, device.modelId, kindStr,
                 device.isAudioCandidate, rom.nodeId, rom.gen);
    }
    
    return device;
}

void DeviceRegistry::MarkDiscovered(Generation gen, uint8_t nodeId) {
    // Check if we already know this (gen, nodeId)
    GenNodeKey key = MakeKey(gen, nodeId);
    auto it = genNodeToGuid_.find(key);
    
    if (it != genNodeToGuid_.end()) {
        // Update existing device
        Guid64 guid = it->second;
        auto devIt = devicesByGuid_.find(guid);
        if (devIt != devicesByGuid_.end()) {
            devIt->second.state = LifeState::Discovered;
            devIt->second.gen = gen;
            devIt->second.nodeId = nodeId;
        }
    }
    // If not found, we'll create it later when ROM arrives
}

void DeviceRegistry::MarkDuplicateGuid(Generation gen, Guid64 guid, uint8_t nodeId) {
    auto it = devicesByGuid_.find(guid);
    if (it != devicesByGuid_.end()) {
        it->second.state = LifeState::Quarantined;
        ASFW_LOG(Discovery, "⚠️  Duplicate GUID detected: 0x%016llx node=%u gen=%u (quarantined)",
                 guid, nodeId, gen);
    }
}

void DeviceRegistry::MarkLost(Generation gen, uint8_t nodeId) {
    GenNodeKey key = MakeKey(gen, nodeId);
    auto it = genNodeToGuid_.find(key);
    
    if (it != genNodeToGuid_.end()) {
        Guid64 guid = it->second;
        auto devIt = devicesByGuid_.find(guid);
        if (devIt != devicesByGuid_.end()) {
            devIt->second.state = LifeState::Lost;
            devIt->second.nodeId = 0xFF;  // Clear nodeId
            ASFW_LOG(Discovery, "Device lost: GUID=0x%016llx node=%u gen=%u",
                     guid, nodeId, gen);
        }
        // Remove from secondary index
        genNodeToGuid_.erase(it);
    }
}

DeviceRecord* DeviceRegistry::FindByGuid(Guid64 guid) {
    auto it = devicesByGuid_.find(guid);
    return (it != devicesByGuid_.end()) ? &it->second : nullptr;
}

const DeviceRecord* DeviceRegistry::FindByGuid(Guid64 guid) const {
    auto it = devicesByGuid_.find(guid);
    return (it != devicesByGuid_.end()) ? &it->second : nullptr;
}

DeviceRecord* DeviceRegistry::FindByNode(Generation gen, uint8_t nodeId) {
    GenNodeKey key = MakeKey(gen, nodeId);
    auto it = genNodeToGuid_.find(key);
    
    if (it != genNodeToGuid_.end()) {
        return FindByGuid(it->second);
    }
    return nullptr;
}

const DeviceRecord* DeviceRegistry::FindByNode(Generation gen, uint8_t nodeId) const {
    GenNodeKey key = MakeKey(gen, nodeId);
    auto it = genNodeToGuid_.find(key);
    
    if (it != genNodeToGuid_.end()) {
        return FindByGuid(it->second);
    }
    return nullptr;
}

std::vector<DeviceRecord> DeviceRegistry::LiveDevices(Generation gen) const {
    std::vector<DeviceRecord> result;
    
    for (const auto& [guid, device] : devicesByGuid_) {
        if (device.gen == gen && device.nodeId != 0xFF) {
            result.push_back(device);
        }
    }
    
    return result;
}

void DeviceRegistry::Clear() {
    devicesByGuid_.clear();
    genNodeToGuid_.clear();
}

DeviceKind DeviceRegistry::ClassifyDevice(const ConfigROM& rom) const {
    for (const auto& entry : rom.rootDirMinimal) {
        if (entry.key == CfgKey::Unit_Spec_Id) {
            const uint32_t specId = entry.value;
            
            // Check for known specifications
            if (specId == kUnitSpecId_TA) {
                return DeviceKind::TA_61883;
            }
            // Could add more classification rules here
        }
    }
    
    return DeviceKind::Unknown;
}

bool DeviceRegistry::IsAudioCandidate(const ConfigROM& rom) const {
    // Device is audio candidate if:
    // 1. Unit_Spec_Id == 0x00A02D (1394 TA / AV/C)
    // 2. Has appropriate Unit_Sw_Version for audio
    
    bool hasAudioSpec = false;
    
    for (const auto& entry : rom.rootDirMinimal) {
        if (entry.key == CfgKey::Unit_Spec_Id) {
            if (entry.value == kUnitSpecId_TA || entry.value == kUnitSpecId_AVC) {
                hasAudioSpec = true;
            }
        }
    }
    
    return hasAudioSpec;
}

DeviceRegistry::GenNodeKey DeviceRegistry::MakeKey(Generation gen, uint8_t nodeId) {
    return (static_cast<uint32_t>(gen) << 8) | static_cast<uint32_t>(nodeId);
}

} // namespace ASFW::Discovery

