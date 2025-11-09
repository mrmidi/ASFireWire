#include "ConfigROMStore.hpp"
#include "DiscoveryValues.hpp"  // For BIBFields constants
#include <algorithm>
#include <DriverKit/DriverKit.h>
#include "../Logging/Logging.hpp"

namespace ASFW::Discovery {

ConfigROMStore::ConfigROMStore() = default;

void ConfigROMStore::Insert(const ConfigROM& rom) {
    if (rom.bib.guid == 0) {
        // Invalid ROM, skip
        ASFW_LOG(Discovery, "ConfigROMStore::Insert: Invalid ROM (GUID=0), skipping");
        return;
    }

    // Create a copy to potentially modify state
    ConfigROM romCopy = rom;

    // If firstSeen is not set, this is a new ROM
    if (romCopy.firstSeen == 0) {
        romCopy.firstSeen = rom.gen;
    }

    // If lastValidated is not set, set it to current generation
    if (romCopy.lastValidated == 0) {
        romCopy.lastValidated = rom.gen;
    }

    // Store by (generation, nodeId)
    GenNodeKey key = MakeKey(romCopy.gen, romCopy.nodeId);
    romsByGenNode_[key] = romCopy;

    // Store by GUID (keep most recent)
    auto it = romsByGuid_.find(romCopy.bib.guid);
    if (it == romsByGuid_.end() || it->second.gen < romCopy.gen) {
        romsByGuid_[romCopy.bib.guid] = romCopy;

        ASFW_LOG(Discovery, "ConfigROMStore::Insert: GUID=0x%016llx gen=%u node=%u state=%u",
                 romCopy.bib.guid, romCopy.gen, romCopy.nodeId,
                 static_cast<uint8_t>(romCopy.state));
    }
}

const ConfigROM* ConfigROMStore::FindByNode(Generation gen, uint8_t nodeId) const {
    GenNodeKey key = MakeKey(gen, nodeId);
    auto it = romsByGenNode_.find(key);
    return (it != romsByGenNode_.end()) ? &it->second : nullptr;
}

const ConfigROM* ConfigROMStore::FindByGuid(Guid64 guid) const {
    auto it = romsByGuid_.find(guid);
    return (it != romsByGuid_.end()) ? &it->second : nullptr;
}

std::vector<ConfigROM> ConfigROMStore::Snapshot(Generation gen) const {
    std::vector<ConfigROM> result;
    
    for (const auto& [key, rom] : romsByGenNode_) {
        if (rom.gen == gen) {
            result.push_back(rom);
        }
    }
    
    return result;
}

void ConfigROMStore::Clear() {
    romsByGenNode_.clear();
    romsByGuid_.clear();
}

const ConfigROM* ConfigROMStore::FindByNode(Generation gen, uint8_t nodeId,
                                             bool allowSuspended) const {
    GenNodeKey key = MakeKey(gen, nodeId);
    auto it = romsByGenNode_.find(key);

    if (it != romsByGenNode_.end()) {
        const auto& rom = it->second;

        // Filter by state if requested
        if (!allowSuspended && rom.state == ROMState::Suspended) {
            return nullptr;  // ROM is suspended, don't return it
        }

        return &rom;
    }

    return nullptr;
}

std::vector<ConfigROM> ConfigROMStore::SnapshotByState(Generation gen,
                                                        ROMState state) const {
    std::vector<ConfigROM> result;

    for (const auto& [key, rom] : romsByGenNode_) {
        if (rom.gen == gen && rom.state == state) {
            result.push_back(rom);
        }
    }

    return result;
}

// ============================================================================
// State Management (Apple IOFireWireROMCache-inspired)
// ============================================================================

void ConfigROMStore::SuspendAll(Generation newGen) {
    // Called on bus reset - mark all ROMs as suspended
    uint32_t suspendedCount = 0;

    for (auto& [key, rom] : romsByGenNode_) {
        if (rom.state == ROMState::Fresh || rom.state == ROMState::Validated) {
            rom.state = ROMState::Suspended;
            suspendedCount++;
        }
    }

    for (auto& [guid, rom] : romsByGuid_) {
        if (rom.state == ROMState::Fresh || rom.state == ROMState::Validated) {
            rom.state = ROMState::Suspended;
        }
    }

    ASFW_LOG(Discovery, "ConfigROMStore::SuspendAll: Suspended %u ROMs for generation %u",
             suspendedCount, newGen);
}

void ConfigROMStore::ValidateROM(Guid64 guid, Generation gen, uint8_t nodeId) {
    // Device reappeared at same/different node - validate ROM
    auto guidIt = romsByGuid_.find(guid);
    if (guidIt != romsByGuid_.end()) {
        auto& rom = guidIt->second;

        if (rom.state == ROMState::Suspended) {
            // Update node mapping if changed
            if (rom.nodeId != nodeId) {
                ASFW_LOG(Discovery, "ConfigROMStore::ValidateROM: GUID 0x%016llx moved node %u→%u in gen %u",
                         guid, rom.nodeId, nodeId, gen);
                rom.nodeId = nodeId;
            }

            rom.gen = gen;
            rom.state = ROMState::Validated;
            rom.lastValidated = gen;

            // Update genNode index
            GenNodeKey newKey = MakeKey(gen, nodeId);
            romsByGenNode_[newKey] = rom;

            ASFW_LOG(Discovery, "ConfigROMStore::ValidateROM: Validated GUID 0x%016llx at node %u gen %u",
                     guid, nodeId, gen);
        } else {
            ASFW_LOG(Discovery, "ConfigROMStore::ValidateROM: GUID 0x%016llx not in suspended state (state=%u)",
                     guid, static_cast<uint8_t>(rom.state));
        }
    } else {
        ASFW_LOG(Discovery, "ConfigROMStore::ValidateROM: GUID 0x%016llx not found", guid);
    }
}

void ConfigROMStore::InvalidateROM(Guid64 guid) {
    auto it = romsByGuid_.find(guid);
    if (it != romsByGuid_.end()) {
        it->second.state = ROMState::Invalid;
        it->second.nodeId = 0xFF;  // Mark as not present

        ASFW_LOG(Discovery, "ConfigROMStore::InvalidateROM: Invalidated GUID 0x%016llx", guid);
    }
}

void ConfigROMStore::PruneInvalid() {
    // Remove invalid ROMs from maps
    std::vector<Guid64> toRemove;

    for (const auto& [guid, rom] : romsByGuid_) {
        if (rom.state == ROMState::Invalid) {
            toRemove.push_back(guid);
        }
    }

    for (Guid64 guid : toRemove) {
        romsByGuid_.erase(guid);
        ASFW_LOG(Discovery, "ConfigROMStore::PruneInvalid: Pruned GUID 0x%016llx from romsByGuid_", guid);
    }

    // Also prune from genNode index
    std::vector<GenNodeKey> toRemoveKeys;
    for (const auto& [key, rom] : romsByGenNode_) {
        if (rom.state == ROMState::Invalid) {
            toRemoveKeys.push_back(key);
        }
    }

    for (auto key : toRemoveKeys) {
        romsByGenNode_.erase(key);
    }

    ASFW_LOG(Discovery, "ConfigROMStore::PruneInvalid: Pruned %zu invalid ROMs",
             toRemove.size());
}

ConfigROMStore::GenNodeKey ConfigROMStore::MakeKey(Generation gen, uint8_t nodeId) {
    return (static_cast<uint32_t>(gen) << 8) | static_cast<uint32_t>(nodeId);
}

// ============================================================================
// ROM Parser Implementation
// ============================================================================

namespace ROMParser {

uint32_t SwapBE32(uint32_t be) {
    // DriverKit provides OSSwapBigToHostInt32 for big-endian to host conversion
    return OSSwapBigToHostInt32(be);
}

std::optional<BusInfoBlock> ParseBIB(const uint32_t* bibQuadlets) {
    if (bibQuadlets == nullptr) {
        return std::nullopt;
    }
    
    // Convert all quadlets from big-endian to host-endian
    const uint32_t q0 = SwapBE32(bibQuadlets[0]);
    // Q1 = bus name "1394" (not used, skipped per Apple pattern in ROMReader)
    // Q2 = capabilities (not currently parsed)
    const uint32_t q3 = SwapBE32(bibQuadlets[3]);
    const uint32_t q4 = SwapBE32(bibQuadlets[4]);

    BusInfoBlock bib{};

    // Quadlet 0: [info_length:16][crc_value:16] with link speed in upper bits
    // IEEE 1394-1995 §8.3.2.1: link speed is bits 31:28 (kFWBIBLinkSpeed)
    // Use constants from DiscoveryValues.hpp
    bib.linkSpeedCode = static_cast<uint8_t>((q0 & BIBFields::kLinkSpeedMask) >> BIBFields::kLinkSpeedShift);
    bib.crcLength = static_cast<uint8_t>((q0 >> 16) & 0xFF);
    bib.infoVersion = 1;  // Assume version 1 for now

    // Quadlet 1: Bus name (typically "1394", 0x31333934) - not parsed
    // NOTE: Vendor ID is NOT in BIB - it's in the root directory (key 0x03)
    bib.vendorId = 0;  // Will be populated from root directory parsing

    // Quadlets 3-4: GUID (64-bit) - IEEE 1394-1995 §8.3.2.2
    bib.guid = (static_cast<uint64_t>(q3) << 32) | static_cast<uint64_t>(q4);
    
    ASFW_LOG(Discovery, "Parsed BIB: GUID=0x%016llx linkSpeed=%u (vendor from root dir)",
             bib.guid, bib.linkSpeedCode);
    
    return bib;
}

std::vector<RomEntry> ParseRootDirectory(const uint32_t* dirQuadlets,
                                         uint32_t maxQuadlets) {
    std::vector<RomEntry> entries;
    
    if (dirQuadlets == nullptr || maxQuadlets == 0) {
        ASFW_LOG(Discovery, "ParseRootDirectory: null data or zero length");
        return entries;
    }
    
    // First quadlet is header: [length:16][crc:16]
    const uint32_t header = SwapBE32(dirQuadlets[0]);
    const uint16_t dirLength = static_cast<uint16_t>((header >> 16) & 0xFFFF);
    
    ASFW_LOG(Discovery, "ParseRootDirectory: header=0x%08x dirLength=%u maxQuadlets=%u",
             header, dirLength, maxQuadlets);
    
    // Bound the scan to the minimum of: actual length, max requested, and safety limit
    uint32_t scanLimit = static_cast<uint32_t>(dirLength);
    if (maxQuadlets > 1 && (maxQuadlets - 1) < scanLimit) {
        scanLimit = maxQuadlets - 1;  // -1 because first quadlet is header
    }
    if (16 < scanLimit) {
        scanLimit = 16;  // Safety: never scan more than 16 entries
    }
    
    ASFW_LOG(Discovery, "ParseRootDirectory: scanning %u entries (dirLength=%u maxQuadlets=%u)",
             scanLimit, dirLength, maxQuadlets);
    
    // Parse entries (start at quadlet 1, after header)
    for (uint32_t i = 1; i <= scanLimit && i < maxQuadlets; ++i) {
        const uint32_t entry = SwapBE32(dirQuadlets[i]);
        
        ASFW_LOG(Discovery, "  Q[%u]: raw=0x%08x", i, entry);
        
        // Entry format: [key_type:2][key_id:6][value:24]
        // key_type (bits 30-31): 0=immediate, 1=CSR offset, 2=leaf, 3=directory
        // key_id (bits 24-29): identifies the entry type (vendor, model, etc.)
        const uint8_t keyType = static_cast<uint8_t>((entry >> 30) & 0x3);
        const uint8_t keyId = static_cast<uint8_t>((entry >> 24) & 0x3F);
        const uint32_t value = entry & 0x00FFFFFF;
        
        ASFW_LOG(Discovery, "       keyType=%u keyId=0x%02x value=0x%06x",
                 keyType, keyId, value);
        
        // Calculate absolute ROM offset for leaf/directory entries
        uint32_t targetOffsetQuadlets = 0;
        if (keyType == EntryType::kLeaf || keyType == EntryType::kDirectory) {
            // value is signed 24-bit offset in quadlets from current entry
            const int32_t signedValue = (value & 0x800000) ? (value | 0xFF000000) : value;
            targetOffsetQuadlets = i + signedValue;  // Absolute offset in root dir quadlets
            ASFW_LOG(Discovery, "       → Leaf/Dir offset: %d quadlets from entry %u = absolute %u",
                     signedValue, i, targetOffsetQuadlets);
        }

        // Recognize important keys for device classification
        switch (keyId) {
            case 0x01:  // Textual descriptor (leaf)
                if (keyType == EntryType::kLeaf) {
                    entries.push_back(RomEntry{CfgKey::TextDescriptor, value, keyType, targetOffsetQuadlets});
                    ASFW_LOG(Discovery, "       → TextDescriptor (leaf at offset %u)", targetOffsetQuadlets);
                }
                break;
            case 0x03:  // Vendor ID
                if (keyType == EntryType::kImmediate) {
                    entries.push_back(RomEntry{CfgKey::VendorId, value, keyType, 0});
                    ASFW_LOG(Discovery, "       → VendorId=0x%06x", value);
                }
                break;
            case 0x17:  // Model ID
                if (keyType == EntryType::kImmediate) {
                    entries.push_back(RomEntry{CfgKey::ModelId, value, keyType, 0});
                    ASFW_LOG(Discovery, "       → ModelId=0x%06x", value);
                }
                break;
            case 0x12:  // Unit_Spec_Id
                if (keyType == EntryType::kImmediate) {
                    entries.push_back(RomEntry{CfgKey::Unit_Spec_Id, value, keyType, 0});
                    ASFW_LOG(Discovery, "       → Unit_Spec_Id=0x%06x", value);
                }
                break;
            case 0x13:  // Unit_Sw_Version
                if (keyType == EntryType::kImmediate) {
                    entries.push_back(RomEntry{CfgKey::Unit_Sw_Version, value, keyType, 0});
                    ASFW_LOG(Discovery, "       → Unit_Sw_Version=0x%06x", value);
                }
                break;
            case 0x14:  // Logical_Unit_Number
                if (keyType == EntryType::kImmediate) {
                    entries.push_back(RomEntry{CfgKey::Logical_Unit_Number, value, keyType, 0});
                    ASFW_LOG(Discovery, "       → Logical_Unit_Number=0x%06x", value);
                }
                break;
            case 0x0C:  // Node_Capabilities
                if (keyType == EntryType::kImmediate) {
                    entries.push_back(RomEntry{CfgKey::Node_Capabilities, value, keyType, 0});
                    ASFW_LOG(Discovery, "       → Node_Capabilities=0x%06x", value);
                }
                break;
            default:
                ASFW_LOG(Discovery, "       → Unrecognized keyId=0x%02x, skipping", keyId);
                break;  // Unrecognized key, skip
        }
    }
    
    ASFW_LOG(Discovery, "Parsed root directory: %zu entries found", entries.size());
    for (const auto& entry : entries) {
        ASFW_LOG(Discovery, "  Entry: key=0x%02x value=0x%06x",
                 static_cast<uint8_t>(entry.key), entry.value);
    }
    
    return entries;
}

// Parse text descriptor from a leaf at the given ROM offset
// Returns decoded ASCII text, or empty string if not a valid text descriptor
std::string ParseTextDescriptorLeaf(const uint32_t* allQuadlets, uint32_t totalQuadlets,
                                    uint32_t leafOffsetQuadlets, const std::string& endianness) {
    ASFW_LOG(Discovery, "    ParseTextDescriptorLeaf: offset=%u total=%u endian=%{public}s",
             leafOffsetQuadlets, totalQuadlets, endianness.c_str());

    // Validate offset
    if (leafOffsetQuadlets + 2 >= totalQuadlets) {
        ASFW_LOG(Discovery, "    ❌ Validation failed: offset+2 (%u) >= total (%u)",
                 leafOffsetQuadlets + 2, totalQuadlets);
        return "";  // Not enough data
    }

    // IEEE 1212 (spec 7.5): ALL directory and leaf data is ALWAYS big-endian
    // This includes: leaf headers, directory entries, descriptor headers, AND text data
    // The BIB endianness flag does NOT affect IEEE 1212 structural parsing
    //
    // rawQuadlets are in HOST byte order, so we must swap to read as big-endian
    auto readBE32 = [&](uint32_t idx) -> uint32_t {
        if (idx >= totalQuadlets) return 0;
        return SwapBE32(allQuadlets[idx]);  // Always read structural elements as big-endian
    };

    // Read leaf header: [length:16|CRC:16] (always big-endian per IEEE 1212)
    const uint32_t header = readBE32(leafOffsetQuadlets);
    const uint16_t leafLength = (header >> 16) & 0xFFFF;  // Upper 16 bits = length

    ASFW_LOG(Discovery, "    Leaf header: 0x%08x → length=%u quadlets", header, leafLength);

    // Need at least 2 quadlets: descriptor header + type/specifier
    if (leafLength < 2 || leafOffsetQuadlets + 1 + leafLength >= totalQuadlets) {
        ASFW_LOG(Discovery, "    ❌ Length check failed: leafLength=%u offset+1+len=%u total=%u",
                 leafLength, leafOffsetQuadlets + 1 + leafLength, totalQuadlets);
        return "";
    }

    // Read descriptor type/specifier (second quadlet of leaf payload, always big-endian)
    const uint32_t typeSpec = readBE32(leafOffsetQuadlets + 2);
    const uint8_t descriptorType = (typeSpec >> 24) & 0xFF;
    const uint32_t specifierId = typeSpec & 0xFFFFFF;

    ASFW_LOG(Discovery, "    Type/Spec: 0x%08x → type=%u specifier=0x%06x",
             typeSpec, descriptorType, specifierId);

    // Only handle textual descriptors (type=0, specifier=0)
    if (descriptorType != 0 || specifierId != 0) {
        ASFW_LOG(Discovery, "    ❌ Not a text descriptor: type=%u spec=0x%06x",
                 descriptorType, specifierId);
        return "";
    }

    // Text starts at quadlet 3 of leaf (after header + desc_header + type/spec)
    const uint32_t textStartQuadlet = leafOffsetQuadlets + 3;
    const uint32_t textQuadlets = (leafLength >= 2) ? (leafLength - 2) : 0;

    if (textQuadlets == 0 || textStartQuadlet + textQuadlets > totalQuadlets) {
        return "";
    }

    // Extract text bytes
    // NOTE: TEXT DATA (unlike structural elements) is stored in big-endian byte order
    // rawQuadlets are in host byte order, so we read as big-endian and extract bytes MSB first
    std::string text;
    text.reserve(textQuadlets * 4);

    for (uint32_t i = 0; i < textQuadlets; ++i) {
        const uint32_t quadlet = readBE32(textStartQuadlet + i);

        // Extract bytes in big-endian order (MSB to LSB)
        for (int j = 3; j >= 0; --j) {
            const uint8_t byte = (quadlet >> (j * 8)) & 0xFF;
            if (byte != 0) {
                text += static_cast<char>(byte);
            }
        }
    }

    return text;
}

// Calculate total Config ROM size from Bus Info Block
// Uses crc_length field from BIB header quadlet
uint32_t CalculateROMSize(const BusInfoBlock& bib) {
    // crc_length is number of quadlets CRC covers (from BIB Q0 bits 23:16)
    // Total ROM = (crc_length + 1) quadlets * 4 bytes/quadlet
    uint32_t totalQuadlets = static_cast<uint32_t>(bib.crcLength) + 1;
    uint32_t totalBytes = totalQuadlets * 4;

    // Clamp to IEEE 1394-1995 maximum Config ROM size (1024 bytes = 256 quadlets)
    const uint32_t kMaxROMBytes = 1024;
    if (totalBytes > kMaxROMBytes) {
        ASFW_LOG(Discovery, "⚠️  ROM size %u exceeds IEEE 1394 max (%u), clamping",
                 totalBytes, kMaxROMBytes);
        totalBytes = kMaxROMBytes;
    }

    ASFW_LOG(Discovery, "Calculated ROM size from BIB: crcLength=%u → %u bytes (%u quadlets)",
             bib.crcLength, totalBytes, totalBytes / 4);

    return totalBytes;
}

} // namespace ROMParser

} // namespace ASFW::Discovery

