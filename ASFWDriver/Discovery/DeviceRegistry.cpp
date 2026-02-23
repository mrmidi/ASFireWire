#include "DeviceRegistry.hpp"
#include <algorithm>
#include <limits>
#include "../Logging/Logging.hpp"
#include "../Protocols/Audio/DeviceProtocolFactory.hpp"

namespace ASFW::Discovery {

constexpr uint32_t kUnitSpecId_TA = 0x00A02D;
constexpr uint32_t kUnitSpecId_AVC = 0x00A02D;

DeviceRegistry::DeviceRegistry() = default;

DeviceRecord& DeviceRegistry::UpsertFromROM(const ConfigROM& rom, const LinkPolicy& link,
                                             Async::AsyncSubsystem* asyncSubsystem) {
    const Guid64 guid = rom.bib.guid;
    
    auto& device = devicesByGuid_[guid];
    
    device.guid = guid;

    for (const auto& entry : rom.rootDirMinimal) {
        if (entry.key == CfgKey::VendorId) {
            device.vendorId = entry.value;
        } else if (entry.key == CfgKey::ModelId) {
            device.modelId = entry.value;
        }
    }

    // Prefer Unit_Directory-derived spec/version (24-bit) over any legacy root dir keys.
    device.unitSpecId.reset();
    device.unitSwVersion.reset();
    for (const auto& unit : rom.unitDirectories) {
        if (unit.unitSpecId != 0) {
            device.unitSpecId = unit.unitSpecId;
        }
        if (unit.unitSwVersion != 0) {
            device.unitSwVersion = unit.unitSwVersion;
        }
        if (device.unitSpecId.has_value() && device.unitSwVersion.has_value()) {
            break;
        }
    }

    device.vendorName = rom.vendorName;
    device.modelName = rom.modelName;
    
    // Known device profiles can choose their integration mode:
    // - kHardcodedNub: vendor-specific audio backend (DICE/TCAT, no AV/C).
    // - kAVCDriven: AV/C discovery drives audio topology; vendor protocol is for extra controls only.
    const auto integrationMode = Audio::DeviceProtocolFactory::LookupIntegrationMode(device.vendorId, device.modelId);

    if (integrationMode != Audio::DeviceIntegrationMode::kNone) {
        ASFW_LOG(Discovery,
                 "Known device profile available for vendor=0x%06x model=0x%06x integration=%u",
                 device.vendorId,
                 device.modelId,
                 static_cast<unsigned>(integrationMode));

        if (integrationMode == Audio::DeviceIntegrationMode::kHardcodedNub) {
            device.kind = DeviceKind::VendorSpecificAudio;
            device.isAudioCandidate = true;
        } else {
            // kAVCDriven: keep AV/C classification so CMP/plug discovery remains applicable.
            device.kind = ClassifyDevice(rom);
            device.isAudioCandidate = IsAudioCandidate(rom);
        }
        
#if !defined(ASFW_HOST_TEST)
        // Create protocol instance if we don't have one yet AND AsyncSubsystem is available
        if (!device.protocol && asyncSubsystem) {
            ASFW_LOG(Discovery, "Creating protocol instance for GUID=0x%016llx node=%u",
                     guid, rom.nodeId);
            device.protocol = Audio::DeviceProtocolFactory::Create(
                device.vendorId, device.modelId, *asyncSubsystem, rom.nodeId);
            
            if (device.protocol) {
                ASFW_LOG(Discovery, "✅ Protocol created: %{public}s - starting initialization",
                         device.protocol->GetName());
                // Start async initialization to discover device capabilities
                device.protocol->Initialize();
            } else {
                ASFW_LOG(Discovery, "❌ Protocol creation failed for vendor=0x%06x model=0x%06x",
                         device.vendorId, device.modelId);
            }
        } else if (!device.protocol) {
            ASFW_LOG(Discovery, "Protocol instance needed for device GUID=0x%016llx node=%u - "
                     "AsyncSubsystem not provided", guid, rom.nodeId);
        }
#endif
    } else {
        device.kind = ClassifyDevice(rom);
        device.isAudioCandidate = IsAudioCandidate(rom);
    }

    // TODO: Generic AV/C devices should work purely via MusicSubunit discovery; vendor protocols are only for extra controls.
    // TODO: Generic DICE/TCAT discovery (non-hardcoded vendor/model) is not implemented yet.
    
    device.gen = rom.gen;
    device.nodeId = rom.nodeId;
    device.link = link;

    // Clamp max async payload by remote MaxRec code (BIB bus options).
    const uint32_t maxFromRec32 = ASFW::FW::MaxAsyncPayloadBytesFromMaxRec(rom.bib.maxRec);
    const uint16_t maxFromRec = (maxFromRec32 > std::numeric_limits<uint16_t>::max())
                                    ? std::numeric_limits<uint16_t>::max()
                                    : static_cast<uint16_t>(maxFromRec32);
    if (device.link.maxPayloadBytes > maxFromRec) {
        device.link.maxPayloadBytes = maxFromRec;
    }
    device.state = LifeState::Identified;
    
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
    for (const auto& unit : rom.unitDirectories) {
        if (unit.unitSpecId == kUnitSpecId_TA) {
            return DeviceKind::TA_61883;
        }
    }
    
    return DeviceKind::Unknown;
}

bool DeviceRegistry::IsAudioCandidate(const ConfigROM& rom) const {
    // Device is audio candidate if:
    // 1. Unit_Spec_Id == 0x00A02D (1394 TA / AV/C)
    // 2. Has appropriate Unit_Sw_Version for audio
    
    bool hasAudioSpec = false;
    
    for (const auto& unit : rom.unitDirectories) {
        if (unit.unitSpecId == kUnitSpecId_TA || unit.unitSpecId == kUnitSpecId_AVC) {
            hasAudioSpec = true;
        }
    }
    
    return hasAudioSpec;
}

DeviceRegistry::GenNodeKey DeviceRegistry::MakeKey(Generation gen, uint8_t nodeId) {
    return (static_cast<uint32_t>(gen) << 8) | static_cast<uint32_t>(nodeId);
}

} // namespace ASFW::Discovery
