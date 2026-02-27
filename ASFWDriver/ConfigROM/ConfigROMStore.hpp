#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "../Discovery/DiscoveryTypes.hpp"

namespace ASFW::Discovery {

// Immutable Config ROM storage with generation-aware lookup.
// Stores parsed ROM objects deduplicated by GUID and indexed by (generation, nodeId).
// Implements state management matching Apple IOFireWireROMCache patterns.
class ConfigROMStore {
public:
    ConfigROMStore();
    ~ConfigROMStore() = default;

    // Insert parsed ROM (deduplicates by GUID within generation)
    void Insert(const ConfigROM& rom);

    // Lookup by generation + nodeId (returns most recent ROM for that node in that gen)
    const ConfigROM* FindByNode(Generation gen, uint8_t nodeId) const;

    // Enhanced lookup with state filtering
    const ConfigROM* FindByNode(Generation gen, uint8_t nodeId, bool allowSuspended) const;

    // Lookup the most recent ROM cached for a node across any generation.
    const ConfigROM* FindLatestForNode(uint8_t nodeId) const;

    // Lookup by GUID (returns most recent ROM across all generations)
    const ConfigROM* FindByGuid(Guid64 guid) const;

    // Export immutable snapshot of all ROMs for a given generation
    std::vector<ConfigROM> Snapshot(Generation gen) const;

    // Export snapshot filtered by ROM state
    std::vector<ConfigROM> SnapshotByState(Generation gen, ROMState state) const;

    // Clear all stored ROMs (e.g., on driver stop)
    void Clear();

    // ========================================================================
    // State Management (Apple IOFireWireROMCache-inspired)
    // ========================================================================

    // Mark all ROMs as suspended (called on bus reset)
    void SuspendAll(Generation newGen);

    // Validate ROM after bus reset (device reappeared)
    void ValidateROM(Guid64 guid, Generation gen, uint8_t nodeId);

    // Mark ROM as invalid (device disappeared or ROM changed)
    void InvalidateROM(Guid64 guid);

    // Remove all invalid ROMs from storage
    void PruneInvalid();

private:
    // Packed key layout: generation in upper bits, node ID in low 8 bits.
    using GenNodeKey = uint32_t;
    static GenNodeKey MakeKey(Generation gen, uint8_t nodeId);
    static std::optional<uint8_t> ValidateNodeIdForKey(uint16_t nodeId);

    std::map<GenNodeKey, ConfigROM> romsByGenNode_;
    std::map<Guid64, ConfigROM> romsByGuid_;
};

// Explicit parser boundary for wire-format Config ROM decoding.
class ConfigROMParser {
public:
    // Parse Bus Info Block from 4 quadlets (16 bytes) in BIG-ENDIAN wire format.
    static std::optional<BusInfoBlock> ParseBIB(const uint32_t* bibQuadlets);

    // Parse root directory entries from BIG-ENDIAN wire format quadlets.
    static std::vector<RomEntry> ParseRootDirectory(const uint32_t* dirQuadlets,
                                                    uint32_t maxQuadlets);

    // Parse text descriptor from a leaf at the given ROM offset.
    static std::string ParseTextDescriptorLeaf(std::span<const uint32_t> allQuadlets,
                                               uint32_t leafOffsetQuadlets,
                                               const std::string& endianness);

    // Calculate total Config ROM size from BIB crc_length field.
    static uint32_t CalculateROMSize(const BusInfoBlock& bib);

private:
    static uint16_t CRCStep(uint16_t crc, uint16_t data);
    static uint16_t ComputeCRC16_1212(std::span<const uint32_t> quadletsHost);
    static bool IsLeafOrDirectory(uint8_t keyType);
    static uint32_t ComputeScanLimit(uint16_t dirLength, uint32_t maxQuadlets);
    static std::optional<uint32_t> ComputeTargetOffsetQuadlets(uint8_t keyType,
                                                               uint32_t value,
                                                               uint32_t index);
    static void AppendRecognizedEntry(std::vector<RomEntry>& entries,
                                      uint8_t keyType,
                                      uint8_t keyId,
                                      uint32_t value,
                                      uint32_t targetOffsetQuadlets);

    static constexpr uint32_t kMaxDirectoryEntriesToScan = 64;
};

} // namespace ASFW::Discovery
