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
 * Stores parsed IEEE 1212 / 1394 Configuration ROM objects by generation and
 * node ID. EUI-64 values are observed metadata and may be zero or duplicated.
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
     * Coalesces repeated reads only for the same generation/node route.
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
     * @return A copied ROM snapshot, or std::nullopt if not found.
     */
    std::optional<ConfigROM> FindByNode(Generation gen, uint8_t nodeId) const;

    /**
     * @brief Enhanced lookup by generation and node ID, with state filtering.
     *
     * @param gen The IEEE 1394 bus generation.
     * @param nodeId The target node ID.
     * @param allowSuspended If false, ignores ROMs in the Suspended state.
     * @return A copied ROM snapshot, or std::nullopt if not found/filtered out.
     */
    std::optional<ConfigROM> FindByNode(Generation gen, uint8_t nodeId,
                                        bool allowSuspended) const;

    /**
     * @brief Looks up the most recently cached ROM for a node across any generation.
     *
     * @param nodeId The target node ID.
     * This is a route-oriented diagnostic query. It never substitutes data
     * from an older generation based on an observed GUID.
     * @return A copied ROM snapshot, or std::nullopt if not found.
     */
    std::optional<ConfigROM> FindLatestForNode(uint8_t nodeId) const;

    /**
     * @brief Returns every cached ROM carrying an observed EUI-64.
     * This is diagnostic only and deliberately has multi-result semantics.
     */
    std::vector<ConfigROM> FindByObservedGuid(Guid64 observedGuid) const;

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

    /** @brief Marks one exact generation/node observation invalid. */
    void InvalidateNode(Generation gen, uint8_t nodeId);

    /**
     * @brief Removes all invalid ROMs from storage.
     */
    void PruneInvalid();

  private:
    // Packed key layout: generation in upper bits, node ID in low 8 bits.
    using GenNodeKey = uint32_t;
    static GenNodeKey MakeKey(Generation gen, uint8_t nodeId);

    mutable IOLock* lock_{nullptr};

    std::map<GenNodeKey, ConfigROM> romsByGenNode_;
};

} // namespace ASFW::Discovery
