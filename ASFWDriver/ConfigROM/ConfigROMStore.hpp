#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../Discovery/DiscoveryTypes.hpp"

struct IOLock;

namespace ASFW::Discovery {

/**
 * @class ConfigROMStore
 * @brief Generation-aware Config ROM cache with lookup/state management.
 *
 * Stores parsed IEEE 1212 / 1394 Configuration ROM objects, deduplicating them by
 * GUID (Extended Unique Identifier, EUI-64) and indexing them by generation and node ID.
 * Implements state management for tracking devices across bus resets, mirroring
 * Apple IOFireWireROMCache patterns.
 */
class ConfigROMStore {
  public:
    ConfigROMStore();
    ~ConfigROMStore();

    ConfigROMStore(const ConfigROMStore&) = delete;
    ConfigROMStore& operator=(const ConfigROMStore&) = delete;
    ConfigROMStore(ConfigROMStore&&) = delete;
    ConfigROMStore& operator=(ConfigROMStore&&) = delete;

    /**
     * @brief Inserts a parsed ROM into the store.
     *
     * Deduplicates by GUID (EUI-64) within the given generation.
     * @param rom The ConfigROM object to insert.
     */
    void Insert(const ConfigROM& rom);

    /**
     * @brief Looks up a Config ROM by generation and node ID.
     *
     * Returns the most recent ROM for that node in the specified generation.
     *
     * @param gen The IEEE 1394 bus generation.
     * @param nodeId The target node ID.
     * @return Pointer to the ConfigROM, or nullptr if not found.
     */
    const ConfigROM* FindByNode(Generation gen, uint8_t nodeId) const;

    /**
     * @brief Enhanced lookup by generation and node ID, with state filtering.
     *
     * @param gen The IEEE 1394 bus generation.
     * @param nodeId The target node ID.
     * @param allowSuspended If false, ignores ROMs in the Suspended state.
     * @return Pointer to the ConfigROM, or nullptr if not found/filtered out.
     */
    const ConfigROM* FindByNode(Generation gen, uint8_t nodeId, bool allowSuspended) const;

    /**
     * @brief Looks up the most recently cached ROM for a node across any generation.
     *
     * @param nodeId The target node ID.
     * @return Pointer to the ConfigROM, or nullptr if not found.
     */
    const ConfigROM* FindLatestForNode(uint8_t nodeId) const;

    /**
     * @brief Looks up a Config ROM by its 64-bit GUID.
     *
     * Returns the most recent ROM across all generations for this EUI-64.
     *
     * @param guid The 64-bit GUID (EUI-64).
     * @return Pointer to the ConfigROM, or nullptr if not found.
     */
    const ConfigROM* FindByGuid(Guid64 guid) const;

    /**
     * @brief Exports an immutable snapshot of all ROMs for a given generation.
     *
     * @param gen The target bus generation.
     * @return A vector of all active ConfigROMs in that generation.
     */
    std::vector<ConfigROM> Snapshot(Generation gen) const;

    /**
     * @brief Exports a snapshot of ROMs filtered by generation and state.
     *
     * @param gen The target bus generation.
     * @param state The required ROMState.
     * @return A vector of filtered ConfigROMs.
     */
    std::vector<ConfigROM> SnapshotByState(Generation gen, ROMState state) const;

    /**
     * @brief Clears all stored ROMs (e.g., on driver stop).
     */
    void Clear();

    // ========================================================================
    // State Management (Apple IOFireWireROMCache-inspired)
    // ========================================================================

    /**
     * @brief Marks all valid ROMs as suspended.
     *
     * Called when an IEEE 1394 bus reset occurs and a new generation begins.
     * @param newGen The newly started generation.
     */
    void SuspendAll(Generation newGen);

    /**
     * @brief Validates a ROM after a bus reset (device reappeared).
     *
     * @param guid The 64-bit GUID.
     * @param gen The current generation.
     * @param nodeId The new node ID of the device.
     */
    void ValidateROM(Guid64 guid, Generation gen, uint8_t nodeId);

    /**
     * @brief Marks a ROM as invalid (device disappeared or ROM content changed).
     *
     * @param guid The 64-bit GUID to invalidate.
     */
    void InvalidateROM(Guid64 guid);

    /**
     * @brief Removes all invalid ROMs from storage.
     */
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
