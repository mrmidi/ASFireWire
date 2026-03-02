#pragma once

#include <cstdint>
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
    static std::optional<BusInfoBlock> ParseBIB(const uint32_t* bibQuadletsBE);

    // Parse root directory entries from BIG-ENDIAN wire format quadlets.
    static std::vector<RomEntry> ParseRootDirectory(const uint32_t* dirQuadletsBE,
                                                    uint32_t maxQuadlets);

    // Parse a textual descriptor leaf (IEEE 1212) from a bounded BIG-ENDIAN quadlet span.
    // Returns decoded ASCII text or an empty string on failure.
    static std::string ParseTextDescriptorLeaf(std::span<const uint32_t> allQuadletsBE,
                                               uint32_t leafOffsetQuadlets);

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
