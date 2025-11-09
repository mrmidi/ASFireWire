#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "DiscoveryTypes.hpp"

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
    // Key: (generation << 8) | nodeId
    using GenNodeKey = uint32_t;
    static GenNodeKey MakeKey(Generation gen, uint8_t nodeId);

    std::map<GenNodeKey, ConfigROM> romsByGenNode_;
    std::map<Guid64, ConfigROM> romsByGuid_;
};

// ============================================================================
// ROM Parser Utilities (minimal bounded parser for BIB + root directory)
// ============================================================================

namespace ROMParser {

// Parse Bus Info Block from 4 quadlets (16 bytes) in BIG-ENDIAN wire format
// Converts to host-endian and extracts link speed, vendorId, GUID
std::optional<BusInfoBlock> ParseBIB(const uint32_t* bibQuadlets);

// Parse root directory entries from N quadlets in BIG-ENDIAN wire format
// Stops after maxEntries or end of directory (whichever comes first)
// Returns vector of recognized key-value entries
std::vector<RomEntry> ParseRootDirectory(const uint32_t* dirQuadlets,
                                         uint32_t maxQuadlets);

// Parse text descriptor from a leaf at the given ROM offset
// Returns decoded ASCII text, or empty string if not a valid text descriptor
std::string ParseTextDescriptorLeaf(const uint32_t* allQuadlets, uint32_t totalQuadlets,
                                    uint32_t leafOffsetQuadlets, const std::string& endianness);

// Calculate total Config ROM size from Bus Info Block crc_length field
// Returns total ROM size in bytes (clamped to IEEE 1394 maximum of 1024 bytes)
uint32_t CalculateROMSize(const BusInfoBlock& bib);

// Utility: Convert big-endian quadlet to host-endian
uint32_t SwapBE32(uint32_t be);

} // namespace ROMParser

} // namespace ASFW::Discovery

