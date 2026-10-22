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
 * @brief Config ROM cache keyed by (bus generation, node ID).
 *
 * Holds the ROMs read in the current generation (Fresh) and, after a bus
 * reset, the previous generation's ROMs (Suspended) until the next reset.
 * Generations are compared only for equality: the 8-bit bus generation wraps
 * after 255 (OHCI 1.2 Table 11-2), so "older" and "newer" cannot be read from
 * the value.
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
     * @brief Inserts a parsed ROM under its (generation, node) key.
     *
     * A ROM with fewer quadlets, or without the unit directories the cached
     * one has, does not replace the cached entry for the same key.
     * @param rom The ConfigROM object to insert.
     */
    void Insert(const ConfigROM& rom);

    // Lookups return a copy taken under the store lock. The store is shared by
    // the driver queue (Insert, SuspendAll on the bus-reset edge) and the
    // user-client queue (ExportConfigROM, TriggerROMRead), and SuspendAll erases
    // entries; a pointer into the map would dangle once the lock is released.

    /**
     * @brief Looks up a Config ROM by generation and node ID.
     *
     * There is deliberately no default for allowSuspended: a suspended entry is
     * a ROM from before the last bus reset, and exporting one as current was
     * the selector 14 bug. Every caller states which it wants.
     *
     * @param gen The IEEE 1394 bus generation.
     * @param nodeId The target node ID.
     * @param allowSuspended If false, ignores ROMs in the Suspended state.
     * @return A copy of the ConfigROM, or std::nullopt if not found/filtered out.
     */
    [[nodiscard]] std::optional<ConfigROM> FindByNode(Generation gen, uint8_t nodeId,
                                                      bool allowSuspended) const;

    // ========================================================================
    // State Management (Apple IOFireWireROMCache-inspired)
    // ========================================================================

    /**
     * @brief Marks all valid ROMs as suspended and drops ROMs suspended earlier.
     *
     * Called when an IEEE 1394 bus reset occurs and a new generation begins.
     * The per-node index then holds at most the suspended previous generation
     * plus whatever discovery inserts for the new one.
     * @param newGen The newly started generation.
     */
    void SuspendAll(Generation newGen);

    /**
     * @brief Removes every cached ROM of one device, in any generation.
     *
     * A manual reread uses this so the new read is not refused in favour of a
     * richer cached copy.
     * @param guid The 64-bit GUID (EUI-64) of the device.
     */
    void EraseByGuid(Guid64 guid);

  private:
    // Packed key layout: generation in upper bits, node ID in low 8 bits.
    using GenNodeKey = uint32_t;
    static GenNodeKey MakeKey(Generation gen, uint8_t nodeId);

    mutable IOLock* lock_{nullptr};

    std::map<GenNodeKey, ConfigROM> romsByGenNode_;
};

} // namespace ASFW::Discovery
