#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "../../Discovery/DiscoveryTypes.hpp"

namespace ASFW::Discovery {

// Explicit parser boundary for wire-format Config ROM decoding.
class ConfigROMParser {
  public:
    // Parse Bus Info Block from BIG-ENDIAN wire format quadlets.
    enum class ErrorCode : uint8_t {
        NullInput,
        TooShort,
        OutOfBounds,
        InvalidHeader,
        UnsupportedTextDescriptor,
        UnsupportedTextEncoding,
    };

    struct Error {
        ErrorCode code{ErrorCode::InvalidHeader};
        uint32_t offsetQuadlets{0};
    };

    enum class CRCStatus : uint8_t {
        NotPresent,
        NotCheckable,
        Ok,
        Mismatch,
    };

    struct BIBParseResult {
        BusInfoBlock bib{};
        CRCStatus crcStatus{CRCStatus::NotCheckable};
        std::optional<uint16_t> computed;
    };

    struct DirectoryEntry {
        uint32_t index{0};
        uint8_t keyType{0};
        uint8_t keyId{0};
        uint32_t value{0};
        bool hasTarget{false};
        uint32_t targetRel{0};
    };

    [[nodiscard]] static std::expected<BIBParseResult, Error>
    ParseBIB(std::span<const uint32_t> bibQuadletsBE);

    // Parse root directory entries from BIG-ENDIAN wire format quadlets.
    [[nodiscard]] static std::expected<std::vector<RomEntry>, Error>
    ParseRootDirectory(std::span<const uint32_t> dirQuadletsBE, uint32_t maxQuadlets);

    [[nodiscard]] static std::expected<std::vector<DirectoryEntry>, Error>
    ParseDirectory(std::span<const uint32_t> dirQuadletsBE, uint32_t entryCap);

    // Parse a textual descriptor leaf (IEEE 1212) from a bounded BIG-ENDIAN quadlet span.
    [[nodiscard]] static std::expected<std::string, Error>
    ParseTextDescriptorLeaf(std::span<const uint32_t> allQuadletsBE, uint32_t leafOffsetQuadlets);

    // Calculate total Config ROM size in bytes from BIB crc_length field.
    static uint32_t CalculateROMSize(const BusInfoBlock& bib);

  private:
    static uint16_t CRCStep(uint16_t crc, uint16_t data);
    static uint16_t ComputeCRC16_1212(std::span<const uint32_t> quadletsHost);
    static bool IsLeafOrDirectory(uint8_t keyType);
    static uint32_t ComputeScanLimit(uint16_t dirLength, uint32_t maxQuadlets);
    static std::optional<uint32_t> ComputeTargetOffsetQuadlets(uint8_t keyType, uint32_t value,
                                                               uint32_t index);
    static void AppendRecognizedEntry(std::vector<RomEntry>& entries, uint8_t keyType,
                                      uint8_t keyId, uint32_t value, uint32_t targetOffsetQuadlets);

    static constexpr uint32_t kMaxDirectoryEntriesToScan = 64;
};

} // namespace ASFW::Discovery
