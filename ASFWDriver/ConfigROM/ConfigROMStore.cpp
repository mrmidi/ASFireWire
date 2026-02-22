#include "ConfigROMStore.hpp"
#include "../Common/FWCommon.hpp"
#include <algorithm>
#include <DriverKit/DriverKit.h>
#include "../Logging/Logging.hpp"
#include "../Logging/LogConfig.hpp"

namespace ASFW::Discovery {

ConfigROMStore::ConfigROMStore() = default;

void ConfigROMStore::Insert(const ConfigROM& rom) {
    if (rom.bib.guid == 0) {
        // Invalid ROM, skip
        ASFW_LOG_V0(ConfigROM, "ConfigROMStore::Insert: Invalid ROM (GUID=0), skipping");
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

        ASFW_LOG_V2(ConfigROM, "ConfigROMStore::Insert: GUID=0x%016llx gen=%u node=%u state=%u",
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

const ConfigROM* ConfigROMStore::FindLatestForNode(uint8_t nodeId) const {
    const ConfigROM* latest = nullptr;
    for (const auto& [key, rom] : romsByGenNode_) {
        if (rom.nodeId != nodeId) {
            continue;
        }
        if (!latest || rom.gen > latest->gen) {
            latest = &rom;
        }
    }
    return latest;
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

    ASFW_LOG(ConfigROM, "ConfigROMStore::SuspendAll: Suspended %u ROMs for generation %u",
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
                ASFW_LOG(ConfigROM, "ConfigROMStore::ValidateROM: GUID 0x%016llx moved node %u→%u in gen %u",
                         guid, rom.nodeId, nodeId, gen);
                rom.nodeId = nodeId;
            }

            rom.gen = gen;
            rom.state = ROMState::Validated;
            rom.lastValidated = gen;

            // Update genNode index
            GenNodeKey newKey = MakeKey(gen, nodeId);
            romsByGenNode_[newKey] = rom;

            ASFW_LOG(ConfigROM, "ConfigROMStore::ValidateROM: Validated GUID 0x%016llx at node %u gen %u",
                     guid, nodeId, gen);
        } else {
            ASFW_LOG(ConfigROM, "ConfigROMStore::ValidateROM: GUID 0x%016llx not in suspended state (state=%u)",
                     guid, static_cast<uint8_t>(rom.state));
        }
    } else {
        ASFW_LOG(ConfigROM, "ConfigROMStore::ValidateROM: GUID 0x%016llx not found", guid);
    }
}

void ConfigROMStore::InvalidateROM(Guid64 guid) {
    auto it = romsByGuid_.find(guid);
    if (it != romsByGuid_.end()) {
        it->second.state = ROMState::Invalid;
        it->second.nodeId = 0xFF;  // Mark as not present

        ASFW_LOG(ConfigROM, "ConfigROMStore::InvalidateROM: Invalidated GUID 0x%016llx", guid);
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
        ASFW_LOG(ConfigROM, "ConfigROMStore::PruneInvalid: Pruned GUID 0x%016llx from romsByGuid_", guid);
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

    ASFW_LOG(ConfigROM, "ConfigROMStore::PruneInvalid: Pruned %zu invalid ROMs",
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

namespace {
uint16_t CRCStep(uint16_t crc, uint16_t data) {
    crc = static_cast<uint16_t>(crc ^ data);
    for (int bit = 0; bit < 16; ++bit) {
        if (crc & 0x8000u) {
            crc = static_cast<uint16_t>((crc << 1) ^ ASFW::FW::kConfigROMCRCPolynomial);
        } else {
            crc = static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

uint16_t ComputeCRC16_1212(const uint32_t* quadletsHost, uint32_t quadletCount) {
    uint16_t crc = 0;
    for (uint32_t i = 0; i < quadletCount; ++i) {
        const uint32_t q = quadletsHost[i];
        crc = CRCStep(crc, static_cast<uint16_t>((q >> 16) & 0xFFFFu));
        crc = CRCStep(crc, static_cast<uint16_t>(q & 0xFFFFu));
    }
    return crc;
}
} // namespace

std::optional<BusInfoBlock> ParseBIB(const uint32_t* bibQuadlets) {
    if (bibQuadlets == nullptr) {
        return std::nullopt;
    }
    
    // Convert all quadlets from big-endian to host-endian
    const uint32_t q0 = SwapBE32(bibQuadlets[0]);
    const uint32_t q1 = SwapBE32(bibQuadlets[1]);
    const uint32_t q2 = SwapBE32(bibQuadlets[2]);
    const uint32_t q3 = SwapBE32(bibQuadlets[3]);
    const uint32_t q4 = SwapBE32(bibQuadlets[4]);

    BusInfoBlock bib{};

    // Quadlet 0: IEEE 1212 header: [bus_info_length:8][crc_length:8][crc:16]
    bib.busInfoLength = static_cast<uint8_t>((q0 & ASFW::FW::ConfigROMHeaderFields::kBusInfoLengthMask) >>
                                             ASFW::FW::ConfigROMHeaderFields::kBusInfoLengthShift);
    bib.crcLength = static_cast<uint8_t>((q0 & ASFW::FW::ConfigROMHeaderFields::kCRCLengthMask) >>
                                         ASFW::FW::ConfigROMHeaderFields::kCRCLengthShift);
    bib.crc = static_cast<uint16_t>(q0 & ASFW::FW::ConfigROMHeaderFields::kCRCMask);

    // Quadlet 1: bus name (usually "1394")
    if (q1 != ASFW::FW::kBusNameQuadlet) {
        ASFW_LOG(ConfigROM, "⚠️  BIB bus name mismatch: q1=0x%08x expected=0x%08x",
                 q1, ASFW::FW::kBusNameQuadlet);
    }

    // Quadlet 2: bus options (TA 1999027)
    const auto decoded = ASFW::FW::DecodeBusOptions(q2);
    bib.irmc = decoded.irmc;
    bib.cmc = decoded.cmc;
    bib.isc = decoded.isc;
    bib.bmc = decoded.bmc;
    bib.pmc = decoded.pmc;
    bib.cycClkAcc = decoded.cycClkAcc;
    bib.maxRec = decoded.maxRec;
    bib.maxRom = decoded.maxRom;
    bib.generation = decoded.generation;
    bib.linkSpd = decoded.linkSpd;

    // Quadlets 3-4: GUID (64-bit) - IEEE 1394-1995 §8.3.2.2
    bib.guid = (static_cast<uint64_t>(q3) << 32) | static_cast<uint64_t>(q4);

    // CRC verification (log-only). We can only validate when crc_length covers <= 4 quadlets,
    // since the initial BIB read only captures quadlets 1..4.
    if (bib.crcLength == 0) {
        ASFW_LOG_V2(ConfigROM, "BIB CRC not verified: crc_length=0 (GUID=0x%016llx)", bib.guid);
    } else if (bib.crcLength <= 4) {
        const uint32_t bibAfterHeader[4] = {q1, q2, q3, q4};
        const uint16_t computed = ComputeCRC16_1212(bibAfterHeader, bib.crcLength);
        if (computed != bib.crc) {
            ASFW_LOG(ConfigROM,
                     "⚠️  BIB CRC mismatch: computed=0x%04x expected=0x%04x (crc_length=%u GUID=0x%016llx)",
                     computed, bib.crc, bib.crcLength, bib.guid);
        } else {
            ASFW_LOG_V2(ConfigROM, "BIB CRC OK: 0x%04x (crc_length=%u GUID=0x%016llx)",
                        bib.crc, bib.crcLength, bib.guid);
        }
    } else {
        ASFW_LOG_V2(ConfigROM,
                    "BIB CRC not verified: crc_length=%u requires more quadlets (GUID=0x%016llx)",
                    bib.crcLength, bib.guid);
    }
    
    ASFW_LOG_V1(ConfigROM,
                "Parsed BIB: GUID=0x%016llx bus_info_len=%u crc_len=%u gen=%u link_spd=%u max_rec=%u max_rom=%u cyc_clk_acc=0x%02x",
                bib.guid, bib.busInfoLength, bib.crcLength, bib.generation, bib.linkSpd,
                bib.maxRec, bib.maxRom, bib.cycClkAcc);
    
    return bib;
}

std::vector<RomEntry> ParseRootDirectory(const uint32_t* dirQuadlets,
                                         uint32_t maxQuadlets) {
    std::vector<RomEntry> entries;
    
    if (dirQuadlets == nullptr || maxQuadlets == 0) {
        ASFW_LOG_V0(ConfigROM, "ParseRootDirectory: null data or zero length");
        return entries;
    }
    
    // First quadlet is header: [length:16][crc:16]
    const uint32_t header = SwapBE32(dirQuadlets[0]);
    const uint16_t dirLength = static_cast<uint16_t>((header >> 16) & 0xFFFF);
    
    ASFW_LOG_V3(ConfigROM, "ParseRootDirectory: header=0x%08x dirLength=%u maxQuadlets=%u",
             header, dirLength, maxQuadlets);
    
    // Bound the scan to the minimum of: actual length, max requested, and safety limit
    uint32_t scanLimit = static_cast<uint32_t>(dirLength);
    if (maxQuadlets > 1 && (maxQuadlets - 1) < scanLimit) {
        scanLimit = maxQuadlets - 1;  // -1 because first quadlet is header
    }
    if (64 < scanLimit) {
        scanLimit = 64;  // Safety: never scan more than 64 entries
    }
    
    ASFW_LOG_V3(ConfigROM, "ParseRootDirectory: scanning %u entries (dirLength=%u maxQuadlets=%u)",
             scanLimit, dirLength, maxQuadlets);
    
    // Parse entries (start at quadlet 1, after header)
    for (uint32_t i = 1; i <= scanLimit && i < maxQuadlets; ++i) {
        const uint32_t entry = SwapBE32(dirQuadlets[i]);
        
        ASFW_LOG_V3(ConfigROM, "  Q[%u]: raw=0x%08x", i, entry);
        
        // Entry format: [key_type:2][key_id:6][value:24]
        // key_type (bits 30-31): 0=immediate, 1=CSR offset, 2=leaf, 3=directory
        // key_id (bits 24-29): identifies the entry type (vendor, model, etc.)
        const uint8_t keyType = static_cast<uint8_t>((entry >> 30) & 0x3);
        const uint8_t keyId = static_cast<uint8_t>((entry >> 24) & 0x3F);
        const uint32_t value = entry & 0x00FFFFFF;
        
        ASFW_LOG_V3(ConfigROM, "       keyType=%u keyId=0x%02x value=0x%06x",
                 keyType, keyId, value);
        
        // Calculate target offset for leaf/directory entries.
        // NOTE: This is relative to the start of THIS directory (header quadlet at index 0).
        uint32_t targetOffsetQuadlets = 0;
        if (keyType == EntryType::kLeaf || keyType == EntryType::kDirectory) {
            // value is signed 24-bit offset in quadlets from current entry
            const int32_t signedValue = (value & 0x800000) ? (value | 0xFF000000) : value;
            const int32_t rel = static_cast<int32_t>(i) + signedValue;
            if (rel >= 0) {
                targetOffsetQuadlets = static_cast<uint32_t>(rel);
                ASFW_LOG_V3(ConfigROM, "       → Leaf/Dir offset: %d quadlets from entry %u = dirRel %u",
                         signedValue, i, targetOffsetQuadlets);
            } else {
                ASFW_LOG(ConfigROM, "       ⚠️  Leaf/Dir offset underflow: entry=%u signed=%d", i, signedValue);
            }
        }

        // Recognize important keys for device classification
        switch (keyId) {
            case 0x01:  // Textual descriptor (leaf or descriptor directory)
                if (keyType == EntryType::kLeaf || keyType == EntryType::kDirectory) {
                    if (targetOffsetQuadlets != 0) {
                        entries.push_back(RomEntry{CfgKey::TextDescriptor, value, keyType, targetOffsetQuadlets});
                        ASFW_LOG_V3(ConfigROM, "       → TextDescriptor (type=%u at dirRel offset %u)",
                                    keyType, targetOffsetQuadlets);
                    } else {
                        ASFW_LOG_V3(ConfigROM, "       → TextDescriptor present but has zero/invalid offset");
                    }
                }
                break;
            case 0x03:  // Vendor ID
                if (keyType == EntryType::kImmediate) {
                    entries.push_back(RomEntry{CfgKey::VendorId, value, keyType, 0});
                    ASFW_LOG_V3(ConfigROM, "       → VendorId=0x%06x", value);
                }
                break;
            case 0x17:  // Model ID
                if (keyType == EntryType::kImmediate) {
                    entries.push_back(RomEntry{CfgKey::ModelId, value, keyType, 0});
                    ASFW_LOG_V3(ConfigROM, "       → ModelId=0x%06x", value);
                }
                break;
            case 0x12:  // Unit_Spec_Id
                if (keyType == EntryType::kImmediate) {
                    entries.push_back(RomEntry{CfgKey::Unit_Spec_Id, value, keyType, 0});
                    ASFW_LOG_V3(ConfigROM, "       → Unit_Spec_Id=0x%06x", value);
                }
                break;
            case 0x13:  // Unit_Sw_Version
                if (keyType == EntryType::kImmediate) {
                    entries.push_back(RomEntry{CfgKey::Unit_Sw_Version, value, keyType, 0});
                    ASFW_LOG_V3(ConfigROM, "       → Unit_Sw_Version=0x%06x", value);
                }
                break;
            case 0x14:  // Logical_Unit_Number
                if (keyType == EntryType::kImmediate) {
                    entries.push_back(RomEntry{CfgKey::Logical_Unit_Number, value, keyType, 0});
                    ASFW_LOG_V3(ConfigROM, "       → Logical_Unit_Number=0x%06x", value);
                }
                break;
            case 0x0C:  // Node_Capabilities
                if (keyType == EntryType::kImmediate) {
                    entries.push_back(RomEntry{CfgKey::Node_Capabilities, value, keyType, 0});
                    ASFW_LOG_V3(ConfigROM, "       → Node_Capabilities=0x%06x", value);
                }
                break;
            case 0x11:  // Unit_Directory (IEEE 1212 key 0xD1, keyId portion is 0x11 when keyType=3)
                if (keyType == EntryType::kDirectory) {
                    entries.push_back(RomEntry{CfgKey::Unit_Directory, value, keyType, targetOffsetQuadlets});
                    ASFW_LOG_V3(ConfigROM, "       → Unit_Directory (dir at offset %u)", targetOffsetQuadlets);
                }
                break;
            default:
                ASFW_LOG_V3(ConfigROM, "       → Unrecognized keyId=0x%02x, skipping", keyId);
                break;  // Unrecognized key, skip
        }
    }
    
    ASFW_LOG_V1(ConfigROM, "Parsed root directory: %zu entries found", entries.size());
    for (const auto& entry : entries) {
        ASFW_LOG_V2(ConfigROM, "  Entry: key=0x%02x value=0x%06x",
                 static_cast<uint8_t>(entry.key), entry.value);
    }
    
    return entries;
}

// Parse text descriptor from a leaf at the given ROM offset
// Returns decoded ASCII text, or empty string if not a valid text descriptor
std::string ParseTextDescriptorLeaf(const uint32_t* allQuadlets, uint32_t totalQuadlets,
                                    uint32_t leafOffsetQuadlets, const std::string& endianness) {
    ASFW_LOG_V3(ConfigROM, "    ParseTextDescriptorLeaf: offset=%u total=%u endian=%{public}s",
             leafOffsetQuadlets, totalQuadlets, endianness.c_str());

    if (leafOffsetQuadlets + 2 >= totalQuadlets) {
        ASFW_LOG_V2(ConfigROM, "    ❌ Validation failed: offset+2 (%u) >= total (%u)",
                 leafOffsetQuadlets + 2, totalQuadlets);
        return "";
    }

    auto readBE32 = [&](uint32_t idx) -> uint32_t {
        if (idx >= totalQuadlets) return 0;
        return SwapBE32(allQuadlets[idx]);
    };

    const uint32_t header = readBE32(leafOffsetQuadlets);
    const uint16_t leafLength = (header >> 16) & 0xFFFF;

    ASFW_LOG_V3(ConfigROM, "    Leaf header: 0x%08x → length=%u quadlets", header, leafLength);

    const uint32_t leafEndExclusive = leafOffsetQuadlets + 1u + static_cast<uint32_t>(leafLength);
    if (leafLength < 2 || leafEndExclusive > totalQuadlets) {
        ASFW_LOG_V2(ConfigROM, "    ❌ Length check failed: leafLength=%u offset+1+len=%u total=%u",
                    leafLength, leafEndExclusive, totalQuadlets);
        return "";
    }

    // IEEE 1212-2001 Figure 28: textual descriptor leaf:
    //   +0: [leaf_length:16][CRC:16]
    //   +1: [descriptor_type:8][specifier_ID:24]
    //   +2: [width:8][character_set:8][language:16]
    //   +3..: textual data (1-byte chars for minimal ASCII form)
    const uint32_t typeSpec = readBE32(leafOffsetQuadlets + 1);
    const uint8_t descriptorType = (typeSpec >> 24) & 0xFF;
    const uint32_t specifierId = typeSpec & 0xFFFFFF;

    ASFW_LOG_V3(ConfigROM, "    Type/Spec: 0x%08x → type=%u specifier=0x%06x",
             typeSpec, descriptorType, specifierId);

    if (descriptorType != 0 || specifierId != 0) {
        ASFW_LOG_V2(ConfigROM, "    ❌ Not a text descriptor: type=%u spec=0x%06x",
                 descriptorType, specifierId);
        return "";
    }

    // Minimal ASCII form only for now (width/character_set/language quadlet must be 0).
    const uint32_t widthCharsetLang = readBE32(leafOffsetQuadlets + 2);
    if (widthCharsetLang != 0) {
        ASFW_LOG_V2(ConfigROM, "    ❌ Unsupported width/charset/lang quadlet: 0x%08x", widthCharsetLang);
        return "";
    }

    const uint32_t textStartQuadlet = leafOffsetQuadlets + 3;
    const uint32_t textQuadlets = (leafLength >= 2) ? (leafLength - 2) : 0;

    if (textQuadlets == 0 || textStartQuadlet + textQuadlets > totalQuadlets) {
        return "";
    }

    std::string text;
    text.reserve(textQuadlets * 4);

    for (uint32_t i = 0; i < textQuadlets; ++i) {
        const uint32_t quadlet = readBE32(textStartQuadlet + i);

        for (int j = 3; j >= 0; --j) {
            const uint8_t byte = (quadlet >> (j * 8)) & 0xFF;
            if (byte == 0) {
                return text;  // NUL-terminated string (strip trailing NULs)
            }
            text += static_cast<char>(byte);
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
        ASFW_LOG_V1(ConfigROM, "⚠️  ROM size %u exceeds IEEE 1394 max (%u), clamping",
                 totalBytes, kMaxROMBytes);
        totalBytes = kMaxROMBytes;
    }

    ASFW_LOG_V2(ConfigROM, "Calculated ROM size from BIB: crcLength=%u → %u bytes (%u quadlets)",
             bib.crcLength, totalBytes, totalBytes / 4);

    return totalBytes;
}

} // namespace ROMParser

} // namespace ASFW::Discovery
