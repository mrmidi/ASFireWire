#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../Discovery/DiscoveryTypes.hpp"

struct IOLock;

namespace ASFW::Discovery {

// Immutable Config ROM storage with generation-aware lookup.
// Stores parsed ROM objects deduplicated by GUID and indexed by (generation, nodeId).
// Implements state management matching Apple IOFireWireROMCache patterns.
class ConfigROMStore {
  public:
    ConfigROMStore();
    ~ConfigROMStore();

    ConfigROMStore(const ConfigROMStore&) = delete;
    ConfigROMStore& operator=(const ConfigROMStore&) = delete;
    ConfigROMStore(ConfigROMStore&&) = delete;
    ConfigROMStore& operator=(ConfigROMStore&&) = delete;

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

    mutable IOLock* lock_{nullptr};

    std::map<GenNodeKey, ConfigROM> romsByGenNode_;
    std::map<Guid64, ConfigROM> romsByGuid_;
};

} // namespace ASFW::Discovery
