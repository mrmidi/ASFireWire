#include "FWDevice.hpp"
#include "FWUnit.hpp"
#include <algorithm>
#include <DriverKit/DriverKit.h>
#include "../Logging/Logging.hpp"
#include "../Logging/LogConfig.hpp"

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
    constexpr uint8_t kEntryTypeDirectory = 3;

    for (const auto& entry : rom.rootDirMinimal) {
        if (entry.key == CfgKey::Unit_Directory && entry.entryType == kEntryTypeDirectory) {
            uint32_t unitDirOffset = entry.leafOffsetQuadlets;

            if (unitDirOffset == 0) {
                continue;
            }

            auto unitEntries = ExtractUnitDirectory(rom, unitDirOffset);

            if (unitEntries.empty()) {
                ASFW_LOG_V1(Discovery, "Failed to extract unit directory at offset %u", unitDirOffset);
                continue;
            }

            auto unit = FWUnit::Create(shared_from_this(), unitDirOffset, unitEntries);

            if (unit) {
                units_.push_back(std::move(unit));
            }
        }
    }

    if (units_.empty()) {
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
    const uint32_t rootDirStartQuadlets = 1u + static_cast<uint32_t>(rom.bib.busInfoLength);
    const uint32_t absoluteROMOffset = rootDirStartQuadlets + offsetQuadlets;

    if (absoluteROMOffset >= rom.rawQuadlets.size()) {
        ASFW_LOG_V1(Discovery, "ExtractUnitDirectory: offset out of bounds");
        return {};
    }

    const uint32_t header = OSSwapBigToHostInt32(rom.rawQuadlets[absoluteROMOffset]);
    const uint16_t dirLength = (header >> 16) & 0xFFFF;

    if (dirLength == 0 || absoluteROMOffset + 1 + dirLength > rom.rawQuadlets.size()) {
        ASFW_LOG_V1(Discovery, "ExtractUnitDirectory: invalid length or out of bounds");
        return {};
    }

    std::vector<RomEntry> entries;
    for (uint32_t i = 1; i <= dirLength && (absoluteROMOffset + i) < rom.rawQuadlets.size(); ++i) {
        const uint32_t entry = OSSwapBigToHostInt32(rom.rawQuadlets[absoluteROMOffset + i]);

        const uint8_t keyType = static_cast<uint8_t>((entry >> 30) & 0x3);
        const uint8_t keyId = static_cast<uint8_t>((entry >> 24) & 0x3F);
        const uint32_t value = entry & 0x00FFFFFF;

        switch (keyId) {
            case 0x12:  // Unit_Spec_Id
                if (keyType == 0) {  // Immediate
                    entries.push_back(RomEntry{CfgKey::Unit_Spec_Id, value, keyType, 0});
                }
                break;
            case 0x13:  // Unit_Sw_Version
                if (keyType == 0) {  // Immediate
                    entries.push_back(RomEntry{CfgKey::Unit_Sw_Version, value, keyType, 0});
                }
                break;
            case 0x14:  // Logical_Unit_Number
                if (keyType == 0) {  // Immediate
                    entries.push_back(RomEntry{CfgKey::Logical_Unit_Number, value, keyType, 0});
                }
                break;
            default:
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
    if (state_ != State::Created) {
        return;
    }

    state_ = State::Ready;

    for (auto& unit : units_) {
        if (unit) {
            unit->Publish();
        }
    }
}

void FWDevice::Suspend()
{
    if (state_ != State::Ready) {
        return;
    }

    state_ = State::Suspended;

    for (auto& unit : units_) {
        if (unit) {
            unit->Suspend();
        }
    }

    nodeId_ = 0xFFFF;
}

void FWDevice::Resume(Generation newGen, uint16_t newNodeId, const LinkPolicy& newLink)
{
    if (state_ != State::Suspended) {
        return;
    }

    generation_ = newGen;
    nodeId_ = newNodeId;
    linkPolicy_ = newLink;

    state_ = State::Ready;

    for (auto& unit : units_) {
        if (unit) {
            unit->Resume();
        }
    }
}

void FWDevice::Terminate()
{
    if (state_ == State::Terminated) {
        return;
    }

    state_ = State::Terminated;

    for (auto& unit : units_) {
        if (unit) {
            unit->Terminate();
        }
    }

    units_.clear();
}

} // namespace ASFW::Discovery
