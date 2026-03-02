#include "ConfigROMParser.hpp"

#include "../../Common/FWCommon.hpp"
#include "../../Logging/LogConfig.hpp"
#include "../../Logging/Logging.hpp"
#include "../Common/ConfigROMConstants.hpp"

#include <algorithm>
#include <array>

#include <DriverKit/DriverKit.h>

namespace ASFW::Discovery {

uint16_t ConfigROMParser::CRCStep(uint16_t crc, uint16_t data) {
    crc = static_cast<uint16_t>(crc ^ data);
    for (int bit = 0; bit < 16; ++bit) {
        if ((crc & 0x8000U) != 0U) {
            crc = static_cast<uint16_t>((crc << 1) ^ ASFW::FW::kConfigROMCRCPolynomial);
        } else {
            crc = static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

uint16_t ConfigROMParser::ComputeCRC16_1212(std::span<const uint32_t> quadletsHost) {
    uint16_t crc = 0;
    for (uint32_t quadletHost : quadletsHost) {
        crc = ConfigROMParser::CRCStep(crc, static_cast<uint16_t>((quadletHost >> 16) & 0xFFFFU));
        crc = ConfigROMParser::CRCStep(crc, static_cast<uint16_t>(quadletHost & 0xFFFFU));
    }
    return crc;
}

bool ConfigROMParser::IsLeafOrDirectory(uint8_t keyType) {
    return keyType == EntryType::kLeaf || keyType == EntryType::kDirectory;
}

uint32_t ConfigROMParser::ComputeScanLimit(uint16_t dirLength, uint32_t maxQuadlets) {
    auto scanLimit = static_cast<uint32_t>(dirLength);
    if (maxQuadlets > 1 && (maxQuadlets - 1) < scanLimit) {
        scanLimit = maxQuadlets - 1; // -1 because first quadlet is header
    }
    scanLimit = std::min(scanLimit, ConfigROMParser::kMaxDirectoryEntriesToScan);
    return scanLimit;
}

std::optional<uint32_t>
ConfigROMParser::ComputeTargetOffsetQuadlets(uint8_t keyType, uint32_t value, uint32_t index) {
    if (!ConfigROMParser::IsLeafOrDirectory(keyType)) {
        return std::nullopt;
    }

    const int32_t signedValue = ((value & 0x800000U) != 0U)
                                    ? static_cast<int32_t>(value | 0xFF000000U)
                                    : static_cast<int32_t>(value);
    const int32_t rel = static_cast<int32_t>(index) + signedValue;
    if (rel < 0) {
        ASFW_LOG(ConfigROM, "       Leaf/Dir offset underflow: entry=%u signed=%d", index,
                 signedValue);
        return std::nullopt;
    }

    const auto targetOffset = static_cast<uint32_t>(rel);
    ASFW_LOG_V3(ConfigROM, "       Leaf/Dir offset: %d quadlets from entry %u = dirRel %u",
                signedValue, index, targetOffset);
    return targetOffset;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) - dominated by logging macros.
void ConfigROMParser::AppendRecognizedEntry(std::vector<RomEntry>& entries, uint8_t keyType,
                                            uint8_t keyId, uint32_t value,
                                            uint32_t targetOffsetQuadlets) {
    switch (keyId) {
    case 0x01: // Textual descriptor (leaf or descriptor directory)
        if (!ConfigROMParser::IsLeafOrDirectory(keyType)) {
            return;
        }
        if (targetOffsetQuadlets == 0) {
            ASFW_LOG_V3(ConfigROM, "       TextDescriptor present but has zero/invalid offset");
            return;
        }
        entries.push_back(RomEntry{.key = CfgKey::TextDescriptor,
                                   .value = value,
                                   .entryType = keyType,
                                   .leafOffsetQuadlets = targetOffsetQuadlets});
        ASFW_LOG_V3(ConfigROM, "       TextDescriptor (type=%u at dirRel offset %u)", keyType,
                    targetOffsetQuadlets);
        return;

    case 0x03: // Vendor ID
        if (keyType == EntryType::kImmediate) {
            entries.push_back(
                RomEntry{.key = CfgKey::VendorId, .value = value, .entryType = keyType});
            ASFW_LOG_V3(ConfigROM, "       VendorId=0x%06x", value);
        }
        return;

    case 0x17: // Model ID
        if (keyType == EntryType::kImmediate) {
            entries.push_back(
                RomEntry{.key = CfgKey::ModelId, .value = value, .entryType = keyType});
            ASFW_LOG_V3(ConfigROM, "       ModelId=0x%06x", value);
        }
        return;

    case 0x12: // Unit_Spec_Id
        if (keyType == EntryType::kImmediate) {
            entries.push_back(
                RomEntry{.key = CfgKey::Unit_Spec_Id, .value = value, .entryType = keyType});
            ASFW_LOG_V3(ConfigROM, "       Unit_Spec_Id=0x%06x", value);
        }
        return;

    case 0x13: // Unit_Sw_Version
        if (keyType == EntryType::kImmediate) {
            entries.push_back(
                RomEntry{.key = CfgKey::Unit_Sw_Version, .value = value, .entryType = keyType});
            ASFW_LOG_V3(ConfigROM, "       Unit_Sw_Version=0x%06x", value);
        }
        return;

    case 0x14: // Logical_Unit_Number
        if (keyType == EntryType::kImmediate) {
            entries.push_back(
                RomEntry{.key = CfgKey::Logical_Unit_Number, .value = value, .entryType = keyType});
            ASFW_LOG_V3(ConfigROM, "       Logical_Unit_Number=0x%06x", value);
        }
        return;

    case 0x0C: // Node_Capabilities
        if (keyType == EntryType::kImmediate) {
            entries.push_back(
                RomEntry{.key = CfgKey::Node_Capabilities, .value = value, .entryType = keyType});
            ASFW_LOG_V3(ConfigROM, "       Node_Capabilities=0x%06x", value);
        }
        return;

    case 0x11: // Unit_Directory (IEEE 1212 key 0xD1, keyId portion is 0x11 when keyType=3)
        if (keyType == EntryType::kDirectory) {
            entries.push_back(RomEntry{.key = CfgKey::Unit_Directory,
                                       .value = value,
                                       .entryType = keyType,
                                       .leafOffsetQuadlets = targetOffsetQuadlets});
            ASFW_LOG_V3(ConfigROM, "       Unit_Directory (dir at offset %u)",
                        targetOffsetQuadlets);
        }
        return;

    default:
        ASFW_LOG_V3(ConfigROM, "       Unrecognized keyId=0x%02x, skipping", keyId);
        return;
    }
}

std::optional<BusInfoBlock> ConfigROMParser::ParseBIB(const uint32_t* bibQuadletsBE) {
    if (bibQuadletsBE == nullptr) {
        return std::nullopt;
    }

    const uint32_t q0 = OSSwapBigToHostInt32(bibQuadletsBE[0]);
    const uint32_t q1 = OSSwapBigToHostInt32(bibQuadletsBE[1]);
    const uint32_t q2 = OSSwapBigToHostInt32(bibQuadletsBE[2]);
    const uint32_t q3 = OSSwapBigToHostInt32(bibQuadletsBE[3]);
    const uint32_t q4 = OSSwapBigToHostInt32(bibQuadletsBE[4]);

    BusInfoBlock bib{};

    bib.busInfoLength = static_cast<uint8_t>((q0 >> 24) & 0xFF);
    bib.crcLength = static_cast<uint8_t>((q0 >> 16) & 0xFF);
    bib.crc = static_cast<uint16_t>(q0 & 0xFFFF);

    bib.irmc = ((q2 >> 31) & 0x1) != 0;
    bib.cmc = ((q2 >> 30) & 0x1) != 0;
    bib.isc = ((q2 >> 29) & 0x1) != 0;
    bib.bmc = ((q2 >> 28) & 0x1) != 0;
    bib.pmc = ((q2 >> 27) & 0x1) != 0;

    bib.cycClkAcc = static_cast<uint8_t>((q2 >> 16) & 0xFF);
    bib.maxRec = static_cast<uint8_t>((q2 >> 12) & 0xF);
    bib.maxRom = static_cast<uint8_t>((q2 >> 8) & 0x3);
    bib.generation = static_cast<uint8_t>((q2 >> 4) & 0xF);
    bib.linkSpd = static_cast<uint8_t>(q2 & 0x7);

    bib.guid = (static_cast<uint64_t>(q3) << 32) | static_cast<uint64_t>(q4);

    if (bib.crcLength == 0) {
        ASFW_LOG_V2(ConfigROM, "BIB CRC not verified: crc_length=0 (GUID=0x%016llx)", bib.guid);
    } else if (bib.crcLength <= 4) {
        const std::array<uint32_t, 4> bibAfterHeader{q1, q2, q3, q4};
        const uint16_t computed = ConfigROMParser::ComputeCRC16_1212(
            std::span<const uint32_t>(bibAfterHeader.data(), bib.crcLength));
        if (computed != bib.crc) {
            ASFW_LOG(ConfigROM,
                     "⚠️  BIB CRC mismatch: computed=0x%04x expected=0x%04x (crc_length=%u "
                     "GUID=0x%016llx)",
                     computed, bib.crc, bib.crcLength, bib.guid);
        }
    }

    return bib;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) - dominated by logging macros.
std::vector<RomEntry> ConfigROMParser::ParseRootDirectory(const uint32_t* dirQuadletsBE,
                                                          uint32_t maxQuadlets) {
    std::vector<RomEntry> entries;

    if (dirQuadletsBE == nullptr || maxQuadlets == 0) {
        return entries;
    }

    const uint32_t header = OSSwapBigToHostInt32(dirQuadletsBE[0]);
    const auto dirLength = static_cast<uint16_t>((header >> 16) & 0xFFFF);

    ASFW_LOG_V3(ConfigROM, "ParseRootDirectory: header=0x%08x dirLength=%u maxQuadlets=%u", header,
                dirLength, maxQuadlets);

    const auto scanLimit = ConfigROMParser::ComputeScanLimit(dirLength, maxQuadlets);

    ASFW_LOG_V3(ConfigROM, "ParseRootDirectory: scanning %u entries (dirLength=%u maxQuadlets=%u)",
                scanLimit, dirLength, maxQuadlets);

    for (uint32_t i = 1; i <= scanLimit && i < maxQuadlets; ++i) {
        const uint32_t entry = OSSwapBigToHostInt32(dirQuadletsBE[i]);

        ASFW_LOG_V3(ConfigROM, "  Q[%u]: raw=0x%08x", i, entry);

        const auto keyType = static_cast<uint8_t>((entry >> 30) & 0x3);
        const auto keyId = static_cast<uint8_t>((entry >> 24) & 0x3F);
        const uint32_t value = entry & 0x00FFFFFF;

        ASFW_LOG_V3(ConfigROM, "       keyType=%u keyId=0x%02x value=0x%06x", keyType, keyId,
                    value);

        const auto targetOffsetQuadlets =
            ConfigROMParser::ComputeTargetOffsetQuadlets(keyType, value, i).value_or(0);
        ConfigROMParser::AppendRecognizedEntry(entries, keyType, keyId, value,
                                               targetOffsetQuadlets);
    }

    ASFW_LOG_V1(ConfigROM, "Parsed root directory: %zu entries found", entries.size());
    for (const auto& entry : entries) {
        ASFW_LOG_V2(ConfigROM, "  Entry: key=0x%02x value=0x%06x", static_cast<uint8_t>(entry.key),
                    entry.value);
    }

    return entries;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) - dominated by logging macros.
std::string ConfigROMParser::ParseTextDescriptorLeaf(std::span<const uint32_t> allQuadletsBE,
                                                     uint32_t leafOffsetQuadlets) {
    const auto totalQuadlets = static_cast<uint32_t>(allQuadletsBE.size());

    ASFW_LOG_V3(ConfigROM, "    ParseTextDescriptorLeaf: offset=%u total=%u", leafOffsetQuadlets,
                totalQuadlets);

    if (leafOffsetQuadlets + 2 >= totalQuadlets) {
        ASFW_LOG_V2(ConfigROM, "    ❌ Validation failed: offset+2 (%u) >= total (%u)",
                    leafOffsetQuadlets + 2, totalQuadlets);
        return "";
    }

    auto readBE32 = [&](uint32_t idx) -> uint32_t {
        if (idx >= totalQuadlets) {
            return 0;
        }
        return OSSwapBigToHostInt32(allQuadletsBE[idx]);
    };

    const uint32_t header = readBE32(leafOffsetQuadlets);
    const uint16_t leafLength = (header >> 16) & 0xFFFF;

    ASFW_LOG_V3(ConfigROM, "    Leaf header: 0x%08x → length=%u quadlets", header, leafLength);

    if (const auto leafEndExclusive = leafOffsetQuadlets + 1U + static_cast<uint32_t>(leafLength);
        leafLength < 2 || leafEndExclusive > totalQuadlets) {
        ASFW_LOG_V2(ConfigROM, "    ❌ Length check failed: leafLength=%u offset+1+len=%u total=%u",
                    leafLength, leafEndExclusive, totalQuadlets);
        return "";
    }

    const uint32_t typeSpec = readBE32(leafOffsetQuadlets + 1);
    const uint8_t descriptorType = (typeSpec >> 24) & 0xFF;
    const uint32_t specifierId = typeSpec & 0xFFFFFF;

    ASFW_LOG_V3(ConfigROM, "    Type/Spec: 0x%08x → type=%u specifier=0x%06x", typeSpec,
                descriptorType, specifierId);

    if (descriptorType != 0 || specifierId != 0) {
        ASFW_LOG_V2(ConfigROM, "    ❌ Not a text descriptor: type=%u spec=0x%06x", descriptorType,
                    specifierId);
        return "";
    }

    if (const auto widthCharsetLang = readBE32(leafOffsetQuadlets + 2); widthCharsetLang != 0) {
        ASFW_LOG_V2(ConfigROM, "    ❌ Unsupported width/charset/lang quadlet: 0x%08x",
                    widthCharsetLang);
        return "";
    }

    const uint32_t textStartQuadlet = leafOffsetQuadlets + 3;
    const uint32_t textQuadlets = (leafLength >= 2) ? (leafLength - 2) : 0;

    if (textQuadlets == 0 || textStartQuadlet + textQuadlets > totalQuadlets) {
        return "";
    }

    std::string text;
    text.reserve(static_cast<size_t>(textQuadlets) * 4U);

    for (uint32_t i = 0; i < textQuadlets; ++i) {
        const uint32_t quadlet = readBE32(textStartQuadlet + i);

        for (int j = 3; j >= 0; --j) {
            const uint8_t byte = (quadlet >> (j * 8)) & 0xFF;
            if (byte == 0) {
                return text;
            }
            text += static_cast<char>(byte);
        }
    }

    return text;
}

uint32_t ConfigROMParser::CalculateROMSize(const BusInfoBlock& bib) {
    uint32_t totalQuadlets = static_cast<uint32_t>(bib.crcLength) + 1;
    uint32_t totalBytes = totalQuadlets * 4;

    if (totalBytes > ASFW::ConfigROM::kMaxROMBytes) {
        ASFW_LOG_V1(ConfigROM, "⚠️  ROM size %u exceeds IEEE 1394 max (%u), clamping", totalBytes,
                    ASFW::ConfigROM::kMaxROMBytes);
        totalBytes = ASFW::ConfigROM::kMaxROMBytes;
    }

    ASFW_LOG_V2(ConfigROM, "Calculated ROM size from BIB: crcLength=%u → %u bytes (%u quadlets)",
                bib.crcLength, totalBytes, totalBytes / 4);

    return totalBytes;
}

} // namespace ASFW::Discovery
