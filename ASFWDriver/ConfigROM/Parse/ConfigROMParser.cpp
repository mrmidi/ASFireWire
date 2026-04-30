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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
uint32_t ConfigROMParser::ComputeScanLimit(uint16_t dirLength, uint32_t maxQuadlets) {
    auto scanLimit = static_cast<uint32_t>(dirLength);
    if (maxQuadlets > 1 && (maxQuadlets - 1) < scanLimit) {
        scanLimit = maxQuadlets - 1; // -1 because first quadlet is header
    }
    scanLimit = std::min(scanLimit, ConfigROMParser::kMaxDirectoryEntriesToScan);
    return scanLimit;
}

std::optional<uint32_t>
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
ConfigROMParser::ComputeTargetOffsetQuadlets(uint8_t keyType, uint32_t value, uint32_t index) {
    if (!ConfigROMParser::IsLeafOrDirectory(keyType)) {
        return std::nullopt;
    }

    const int32_t signedValue = ((value & 0x800000U) != 0U)
                                    ? static_cast<int32_t>(value | 0xFF000000U)
                                    : static_cast<int32_t>(value);
    const int32_t rel = static_cast<int32_t>(index) + signedValue;
    if (rel < 0) {
        return std::nullopt;
    }

    return static_cast<uint32_t>(rel);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void ConfigROMParser::AppendRecognizedEntry(std::vector<RomEntry>& entries, uint8_t keyType,
                                            uint8_t keyId, uint32_t value,
                                            uint32_t targetOffsetQuadlets) {
    switch (keyId) {
    case 0x01: // Textual descriptor (leaf or descriptor directory)
        if (!ConfigROMParser::IsLeafOrDirectory(keyType)) {
            return;
        }
        if (targetOffsetQuadlets == 0) {
            return;
        }
        entries.push_back(RomEntry{.key = CfgKey::TextDescriptor,
                                   .value = value,
                                   .entryType = keyType,
                                   .leafOffsetQuadlets = targetOffsetQuadlets});
        return;

    case 0x03: // Vendor ID
        if (keyType == EntryType::kImmediate) {
            entries.push_back(
                RomEntry{.key = CfgKey::VendorId, .value = value, .entryType = keyType});
        }
        return;

    case 0x17: // Model ID
        if (keyType == EntryType::kImmediate) {
            entries.push_back(
                RomEntry{.key = CfgKey::ModelId, .value = value, .entryType = keyType});
        }
        return;

    case 0x12: // Unit_Spec_Id
        if (keyType == EntryType::kImmediate) {
            entries.push_back(
                RomEntry{.key = CfgKey::Unit_Spec_Id, .value = value, .entryType = keyType});
        }
        return;

    case 0x13: // Unit_Sw_Version
        if (keyType == EntryType::kImmediate) {
            entries.push_back(
                RomEntry{.key = CfgKey::Unit_Sw_Version, .value = value, .entryType = keyType});
        }
        return;

    case 0x14: // Logical_Unit_Number or SBP-2 Management_Agent_Offset
        if (keyType == EntryType::kImmediate) {
            entries.push_back(
                RomEntry{.key = CfgKey::Logical_Unit_Number, .value = value, .entryType = keyType});
        } else if (keyType == EntryType::kCSROffset) {
            entries.push_back(RomEntry{.key = CfgKey::Management_Agent_Offset,
                                       .value = value,
                                       .entryType = keyType});
        }
        return;

    case 0x0C: // Node_Capabilities
        if (keyType == EntryType::kImmediate) {
            entries.push_back(
                RomEntry{.key = CfgKey::Node_Capabilities, .value = value, .entryType = keyType});
        }
        return;

    case 0x11: // Unit_Directory (IEEE 1212 key 0xD1, keyId portion is 0x11 when keyType=3)
        if (keyType == EntryType::kDirectory) {
            entries.push_back(RomEntry{.key = CfgKey::Unit_Directory,
                                       .value = value,
                                       .entryType = keyType,
                                       .leafOffsetQuadlets = targetOffsetQuadlets});
        }
        return;

    case 0x38: // Legacy non-standard fallback for Management_Agent_Offset
        if (keyType == EntryType::kCSROffset) {
            entries.push_back(RomEntry{.key = CfgKey::Management_Agent_Offset,
                                       .value = value,
                                       .entryType = keyType});
        }
        return;

    default:
        return;
    }
}

std::expected<ConfigROMParser::BIBParseResult, ConfigROMParser::Error>
ConfigROMParser::ParseBIB(std::span<const uint32_t> bibQuadletsBE) {
    if (bibQuadletsBE.size() < ASFW::ConfigROM::kBIBQuadletCount) {
        return std::unexpected(Error{.code = ErrorCode::TooShort, .offsetQuadlets = 0});
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

    BIBParseResult out{};
    out.bib = bib;

    if (bib.crcLength == 0) {
        out.crcStatus = CRCStatus::NotPresent;
        return out;
    }

    if (bib.crcLength > 4) {
        out.crcStatus = CRCStatus::NotCheckable;
        return out;
    }

    const std::array<uint32_t, 4> bibAfterHeader{q1, q2, q3, q4};
    const auto crcSpan =
        std::span<const uint32_t>(bibAfterHeader.data(), static_cast<size_t>(bib.crcLength));
    const uint16_t computed = ConfigROMParser::ComputeCRC16_1212(crcSpan);

    out.computed = computed;
    out.crcStatus = (computed == bib.crc) ? CRCStatus::Ok : CRCStatus::Mismatch;
    return out;
}

std::expected<std::vector<ConfigROMParser::DirectoryEntry>, ConfigROMParser::Error>
ConfigROMParser::ParseDirectory(std::span<const uint32_t> dirQuadletsBE, uint32_t entryCap) {
    if (dirQuadletsBE.empty()) {
        return std::unexpected(Error{.code = ErrorCode::TooShort, .offsetQuadlets = 0});
    }

    const uint32_t hdr = OSSwapBigToHostInt32(dirQuadletsBE[0]);
    const uint32_t len = (hdr >> 16) & 0xFFFFU;
    const auto available = static_cast<uint32_t>(dirQuadletsBE.size() - 1);
    const uint32_t count = std::min({len, available, entryCap});

    std::vector<DirectoryEntry> out;
    out.reserve(count);
    for (uint32_t i = 1; i <= count; ++i) {
        const uint32_t entry = OSSwapBigToHostInt32(dirQuadletsBE[i]);
        DirectoryEntry entryOut{};
        entryOut.index = i;
        entryOut.keyType = static_cast<uint8_t>((entry >> 30) & 0x3U);
        entryOut.keyId = static_cast<uint8_t>((entry >> 24) & 0x3FU);
        entryOut.value = entry & 0x00FFFFFFU;

        if (entryOut.keyType == ASFW::FW::EntryType::kLeaf ||
            entryOut.keyType == ASFW::FW::EntryType::kDirectory) {
            const int32_t signedValue = ((entryOut.value & 0x00800000U) != 0U)
                                            ? static_cast<int32_t>(entryOut.value | 0xFF000000U)
                                            : static_cast<int32_t>(entryOut.value);
            const int32_t rel = static_cast<int32_t>(i) + signedValue;
            if (rel >= 0) {
                entryOut.hasTarget = true;
                entryOut.targetRel = static_cast<uint32_t>(rel);
            }
        }

        out.push_back(entryOut);
    }

    return out;
}

std::expected<std::vector<RomEntry>, ConfigROMParser::Error>
ConfigROMParser::ParseRootDirectory(std::span<const uint32_t> dirQuadletsBE, uint32_t maxQuadlets) {
    if (maxQuadlets == 0) {
        return std::unexpected(Error{.code = ErrorCode::TooShort, .offsetQuadlets = 0});
    }

    const auto cappedMax = std::min<size_t>(static_cast<size_t>(maxQuadlets), dirQuadletsBE.size());
    if (cappedMax == 0) {
        return std::unexpected(Error{.code = ErrorCode::TooShort, .offsetQuadlets = 0});
    }

    const auto bounded = dirQuadletsBE.subspan(0, cappedMax);

    auto parsed =
        ConfigROMParser::ParseDirectory(bounded, ConfigROMParser::kMaxDirectoryEntriesToScan);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    std::vector<RomEntry> entries;
    for (const auto& entry : *parsed) {
        ConfigROMParser::AppendRecognizedEntry(entries, entry.keyType, entry.keyId, entry.value,
                                               entry.hasTarget ? entry.targetRel : 0);
    }

    return entries;
}

std::expected<std::string, ConfigROMParser::Error>
ConfigROMParser::ParseTextDescriptorLeaf(std::span<const uint32_t> allQuadletsBE,
                                         uint32_t leafOffsetQuadlets) {
    const auto totalQuadlets = static_cast<uint32_t>(allQuadletsBE.size());

    if (leafOffsetQuadlets + 2 >= totalQuadlets) {
        return std::unexpected(
            Error{.code = ErrorCode::OutOfBounds, .offsetQuadlets = leafOffsetQuadlets});
    }

    auto readBE32 = [&](uint32_t idx) -> uint32_t {
        return OSSwapBigToHostInt32(allQuadletsBE[idx]);
    };

    const uint32_t header = readBE32(leafOffsetQuadlets);
    const uint16_t leafLength = (header >> 16) & 0xFFFF;

    if (const auto leafEndExclusive = leafOffsetQuadlets + 1U + static_cast<uint32_t>(leafLength);
        leafLength < 2 || leafEndExclusive > totalQuadlets) {
        return std::unexpected(
            Error{.code = ErrorCode::InvalidHeader, .offsetQuadlets = leafOffsetQuadlets});
    }

    const uint32_t typeSpec = readBE32(leafOffsetQuadlets + 1);
    const uint8_t descriptorType = (typeSpec >> 24) & 0xFF;
    const uint32_t specifierId = typeSpec & 0xFFFFFF;

    if (descriptorType != 0 || specifierId != 0) {
        return std::unexpected(Error{.code = ErrorCode::UnsupportedTextDescriptor,
                                     .offsetQuadlets = leafOffsetQuadlets + 1});
    }

    if (const auto widthCharsetLang = readBE32(leafOffsetQuadlets + 2); widthCharsetLang != 0) {
        return std::unexpected(Error{.code = ErrorCode::UnsupportedTextEncoding,
                                     .offsetQuadlets = leafOffsetQuadlets + 2});
    }

    const uint32_t textStartQuadlet = leafOffsetQuadlets + 3;
    const uint32_t textQuadlets = (leafLength >= 2) ? (leafLength - 2) : 0;

    if (textQuadlets == 0 || textStartQuadlet + textQuadlets > totalQuadlets) {
        return std::unexpected(
            Error{.code = ErrorCode::TooShort, .offsetQuadlets = leafOffsetQuadlets});
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
