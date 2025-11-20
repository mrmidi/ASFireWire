#include "FWDevice.hpp"
#include "FWUnit.hpp"
#include <algorithm>
#include <DriverKit/DriverKit.h>
#include "../Logging/Logging.hpp"

namespace ASFW::Discovery {

// Private constructor
FWDevice::FWDevice(const DeviceRecord& record)
    : guid_(record.guid)
    , vendorId_(record.vendorId)
    , modelId_(record.modelId)
    , kind_(record.kind)
    , vendorName_(record.vendorName)
    , modelName_(record.modelName)
    , isAudioCandidate_(record.isAudioCandidate)
    , supportsAMDTP_(record.supportsAMDTP)
    , generation_(record.gen)
    , nodeId_(record.nodeId)
    , linkPolicy_(record.link)
{
}

// Factory method
std::shared_ptr<FWDevice> FWDevice::Create(
    const DeviceRecord& record,
    const ConfigROM& rom)
{
    if (record.guid == 0) {
        return nullptr; // Invalid device
    }

    // Use new + shared_ptr constructor (can't use make_shared with private ctor)
    auto device = std::shared_ptr<FWDevice>(new FWDevice(record));

    // Parse unit directories from ROM
    device->ParseUnits(rom);

    return device;
}

void FWDevice::ParseUnits(const ConfigROM& rom)
{
    // IEEE 1212 directory structure:
    // Root directory contains entries, some of which point to unit directories
    // Entry type 3 = directory offset
    // Key type determines what kind of directory (unit, vendor info, etc.)

    constexpr uint8_t kEntryTypeDirectory = 3;

    // Scan root directory for unit directory references
    for (const auto& entry : rom.rootDirMinimal) {
        // Check if this is a Unit_Directory entry (key=0xD1, type=directory)
        if (entry.key == CfgKey::Unit_Directory && entry.entryType == kEntryTypeDirectory) {
            uint32_t unitDirOffset = entry.leafOffsetQuadlets;

            if (unitDirOffset == 0) {
                continue; // Invalid offset
            }

            ASFW_LOG(Discovery, "Found Unit_Directory at offset %u, extracting...", unitDirOffset);

            // Extract unit directory entries from ROM
            auto unitEntries = ExtractUnitDirectory(rom, unitDirOffset);

            if (unitEntries.empty()) {
                ASFW_LOG(Discovery, "Failed to extract unit directory at offset %u", unitDirOffset);
                continue; // Failed to extract
            }

            ASFW_LOG(Discovery, "Extracted %zu entries from unit directory", unitEntries.size());

            // Create FWUnit from parsed entries
            auto unit = FWUnit::Create(shared_from_this(), unitDirOffset, unitEntries);

            if (unit) {
                units_.push_back(std::move(unit));
                ASFW_LOG(Discovery, "Created FWUnit successfully");
            }
        }
    }

    // If no units found, create a default unit representing the device itself
    // This matches Apple's behavior where some devices have implicit unit directories
    if (units_.empty()) {
        // Create synthetic unit directory from root directory
        // Use root directory entries as unit entries
        auto unit = FWUnit::Create(shared_from_this(), 0, rom.rootDirMinimal);

        if (unit) {
            units_.push_back(std::move(unit));
        }
    }
}

std::vector<RomEntry> FWDevice::ExtractUnitDirectory(
    const ConfigROM& rom,
    uint32_t offsetQuadlets) const
{
    // Per IEEE 1394-1995 §8.3: BIB block is 5 quadlets, root directory starts at offset 5
    // offsetQuadlets is relative to root directory start, so add 5 to get absolute ROM offset
    constexpr uint32_t kBIBQuadlets = 5;
    const uint32_t absoluteROMOffset = kBIBQuadlets + offsetQuadlets;

    ASFW_LOG(Discovery, "ExtractUnitDirectory: root-dir-rel=%u absolute-ROM=%u total=%zu",
             offsetQuadlets, absoluteROMOffset, rom.rawQuadlets.size());

    // Validate offset is within ROM bounds
    if (absoluteROMOffset >= rom.rawQuadlets.size()) {
        ASFW_LOG(Discovery, "ExtractUnitDirectory: offset out of bounds");
        return {};
    }

    // Read directory header: [length:16|CRC:16]
    const uint32_t header = OSSwapBigToHostInt32(rom.rawQuadlets[absoluteROMOffset]);
    const uint16_t dirLength = (header >> 16) & 0xFFFF;

    ASFW_LOG(Discovery, "Unit directory header: 0x%08x length=%u", header, dirLength);

    // Validate length
    if (dirLength == 0 || absoluteROMOffset + 1 + dirLength > rom.rawQuadlets.size()) {
        ASFW_LOG(Discovery, "ExtractUnitDirectory: invalid length or out of bounds");
        return {};
    }

    // Parse directory entries (same format as root directory)
    std::vector<RomEntry> entries;
    for (uint32_t i = 1; i <= dirLength && (absoluteROMOffset + i) < rom.rawQuadlets.size(); ++i) {
        const uint32_t entry = OSSwapBigToHostInt32(rom.rawQuadlets[absoluteROMOffset + i]);

        const uint8_t keyType = static_cast<uint8_t>((entry >> 30) & 0x3);
        const uint8_t keyId = static_cast<uint8_t>((entry >> 24) & 0x3F);
        const uint32_t value = entry & 0x00FFFFFF;

        ASFW_LOG(Discovery, "  Unit dir entry[%u]: keyType=%u keyId=0x%02x value=0x%06x",
                 i, keyType, keyId, value);

        // Recognize important keys for unit classification
        switch (keyId) {
            case 0x12:  // Unit_Spec_Id
                if (keyType == 0) {  // Immediate
                    entries.push_back(RomEntry{CfgKey::Unit_Spec_Id, value, keyType, 0});
                    ASFW_LOG(Discovery, "    → Unit_Spec_Id=0x%06x", value);
                }
                break;
            case 0x13:  // Unit_Sw_Version
                if (keyType == 0) {  // Immediate
                    entries.push_back(RomEntry{CfgKey::Unit_Sw_Version, value, keyType, 0});
                    ASFW_LOG(Discovery, "    → Unit_Sw_Version=0x%06x", value);
                }
                break;
            case 0x14:  // Logical_Unit_Number
                if (keyType == 0) {  // Immediate
                    entries.push_back(RomEntry{CfgKey::Logical_Unit_Number, value, keyType, 0});
                    ASFW_LOG(Discovery, "    → Logical_Unit_Number=0x%06x", value);
                }
                break;
            default:
                ASFW_LOG(Discovery, "    → Unrecognized unit entry keyId=0x%02x", keyId);
                break;
        }
    }

    return entries;
}

std::vector<std::shared_ptr<FWUnit>> FWDevice::FindUnitsBySpec(
    uint32_t specId,
    std::optional<uint32_t> swVersion) const
{
    std::vector<std::shared_ptr<FWUnit>> matches;

    for (const auto& unit : units_) {
        if (unit && unit->Matches(specId, swVersion)) {
            matches.push_back(unit);
        }
    }

    return matches;
}

// === Lifecycle Methods ===

void FWDevice::Publish()
{
    // Only transition from Created state
    if (state_ != State::Created) {
        return;
    }

    state_ = State::Ready;

    // Publish all units
    for (auto& unit : units_) {
        if (unit) {
            unit->Publish();
        }
    }

    // TODO: Notify observers that device is published
    // This will be implemented when we add IDeviceObserver in Phase 3
}

void FWDevice::Suspend()
{
    // Only transition from Ready state
    if (state_ != State::Ready) {
        return;
    }

    state_ = State::Suspended;

    // Suspend all units
    for (auto& unit : units_) {
        if (unit) {
            unit->Suspend();
        }
    }

    // Mark as not present in current generation
    nodeId_ = 0xFF;

    // TODO: Notify observers that device is suspended
}

void FWDevice::Resume(Generation newGen, uint8_t newNodeId, const LinkPolicy& newLink)
{
    // Only transition from Suspended state
    if (state_ != State::Suspended) {
        return;
    }

    // Update current generation info
    generation_ = newGen;
    nodeId_ = newNodeId;
    linkPolicy_ = newLink;

    state_ = State::Ready;

    // Resume all units
    for (auto& unit : units_) {
        if (unit) {
            unit->Resume();
        }
    }

    // TODO: Notify observers that device is resumed
}

void FWDevice::Terminate()
{
    // Can transition from any state to Terminated
    if (state_ == State::Terminated) {
        return; // Already terminated
    }

    state_ = State::Terminated;

    // Terminate all units
    for (auto& unit : units_) {
        if (unit) {
            unit->Terminate();
        }
    }

    // TODO: Notify observers that device is terminated

    // Clear units vector to release references
    units_.clear();
}

} // namespace ASFW::Discovery
