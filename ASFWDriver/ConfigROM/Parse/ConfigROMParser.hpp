#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "../../Discovery/DiscoveryTypes.hpp"

namespace ASFW::Discovery {

/**
 * @class ConfigROMParser
 * @brief Explicit parser boundary for wire-format Config ROM decoding.
 *
 * Provides methods for parsing the Bus Information Block (BIB), directories,
 * and leaves according to the IEEE 1212-2001 (CSR Architecture) and
 * IEEE 1394-1995 standards.
 */
class ConfigROMParser {
  public:
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
        uint32_t offsetQuadlets{0}; ///< Best-effort quadlet offset for diagnostics.
    };

    enum class CRCStatus : uint8_t {
        NotPresent,   ///< crc_length == 0.
        NotCheckable, ///< crc_length > 4 (need more than BIB to compute).
        Ok,           ///< CRC computed and matches header.
        Mismatch,     ///< CRC computed but mismatched (warning only; parsing still succeeds).
    };

    /**
     * @brief Result of parsing a Bus Information Block.
     */
    struct BIBParseResult {
        BusInfoBlock bib{};
        CRCStatus crcStatus{CRCStatus::NotCheckable};
        std::optional<uint16_t> computed; ///< Set only when CRC was computed (crc_length <= 4).
    };

    /**
     * @brief Decoded entry from a Configuration ROM Directory (IEEE 1212 §7.5.1).
     */
    struct DirectoryEntry {
        uint32_t index{0};     ///< 1-based entry index within the directory.
        uint8_t keyType{0};    ///< Top 2 bits of key byte (EntryType::*).
        uint8_t keyId{0};      ///< Low 6 bits of key byte (ConfigKey::*).
        uint32_t value{0};     ///< Low 24 bits, host order.
        bool hasTarget{false}; ///< True when leaf/directory targetRel was computed.
        uint32_t targetRel{0}; ///< Quadlets relative to the directory header quadlet.
    };

    /**
     * @brief Parses a Bus Information Block from BIG-ENDIAN wire format quadlets.
     *
     * Extracts fields defined in IEEE 1394-1995 §8.3.2.5, including the GUID,
     * max_rec, and bus capabilities. CRC-16 is computed when checkable and
     * returned as CRCStatus (CRC mismatch is warning-only).
     *
     * @param bibQuadletsBE Span of quadlets in big-endian byte order (at least 5 quadlets).
     * @return Parsed bib + CRC status on success, or an Error on failure.
     */
    [[nodiscard]] static std::expected<BIBParseResult, Error>
    ParseBIB(std::span<const uint32_t> bibQuadletsBE);

    /**
     * @brief Parses root directory entries from BIG-ENDIAN wire format quadlets.
     *
     * Reads the Root Directory (IEEE 1212 §7.6.1), decoding known entries into
     * recognized RomEntry items.
     *
     * @param dirQuadletsBE Span of quadlets containing the root directory.
     * @param maxQuadlets Maximum number of quadlets to scan.
     * @return A vector of parsed RomEntry structures.
     */
    [[nodiscard]] static std::expected<std::vector<RomEntry>, Error>
    ParseRootDirectory(std::span<const uint32_t> dirQuadletsBE, uint32_t maxQuadlets);

    /**
     * @brief Parses generic directory entries (IEEE 1212 §7.5.1).
     *
     * Output DirectoryEntry values are decoded to host order.
     *
     * @param dirQuadletsBE Span of quadlets containing the directory.
     * @param entryCap Maximum number of entries to process.
     * @return A vector of generic DirectoryEntry objects.
     */
    [[nodiscard]] static std::expected<std::vector<DirectoryEntry>, Error>
    ParseDirectory(std::span<const uint32_t> dirQuadletsBE, uint32_t entryCap);

    /**
     * @brief Parses a Textual Descriptor Leaf (IEEE 1212 §7.5.4.1).
     *
     * Converts minimal ASCII text stored in a leaf into a string. Unsupported
     * encodings are returned as UnsupportedTextDescriptor/UnsupportedTextEncoding.
     *
     * @param allQuadletsBE The complete Config ROM read so far in big-endian.
     * @param leafOffsetQuadlets The absolute offset of the leaf header from the start of ROM.
     * @return The extracted string on success, or an Error on failure.
     */
    [[nodiscard]] static std::expected<std::string, Error>
    ParseTextDescriptorLeaf(std::span<const uint32_t> allQuadletsBE, uint32_t leafOffsetQuadlets);

    /**
     * @brief Calculates total Config ROM size in bytes based on the BIB crc_length.
     *
     * Result is clamped to ASFW::ConfigROM::kMaxROMBytes.
     *
     * @param bib The parsed Bus Information Block.
     * @return The length of the ROM in bytes.
     */
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
