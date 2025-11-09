#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "DiscoveryTypes.hpp"

namespace ASFW::Discovery {

// Stable GUID-keyed device registry with per-generation live mapping.
// Maintains device identity across bus resets and performs audio classification.
class DeviceRegistry {
public:
    DeviceRegistry();
    ~DeviceRegistry() = default;

    // Create or update device record from parsed ROM
    // Returns reference to live record
    DeviceRecord& UpsertFromROM(const ConfigROM& rom, const LinkPolicy& link);

    // Mark device as discovered (seen in Self-ID, before ROM fetch)
    void MarkDiscovered(Generation gen, uint8_t nodeId);

    // Handle duplicate GUID detection within same generation
    void MarkDuplicateGuid(Generation gen, Guid64 guid, uint8_t nodeId);

    // Mark device as lost (not present in current generation)
    void MarkLost(Generation gen, uint8_t nodeId);

    // Lookup by GUID (stable across resets)
    DeviceRecord* FindByGuid(Guid64 guid);
    const DeviceRecord* FindByGuid(Guid64 guid) const;

    // Lookup by (generation, nodeId)
    DeviceRecord* FindByNode(Generation gen, uint8_t nodeId);
    const DeviceRecord* FindByNode(Generation gen, uint8_t nodeId) const;

    // Export snapshot of all devices present in given generation
    std::vector<DeviceRecord> LiveDevices(Generation gen) const;

    // Clear all records (e.g., on driver stop)
    void Clear();

private:
    // Classify device kind from ROM entries
    DeviceKind ClassifyDevice(const ConfigROM& rom) const;

    // Check if device is audio candidate based on ROM
    bool IsAudioCandidate(const ConfigROM& rom) const;

    // Primary storage: GUID-keyed device records
    std::map<Guid64, DeviceRecord> devicesByGuid_;

    // Secondary index: (generation, nodeId) → GUID for fast per-generation lookup
    using GenNodeKey = uint32_t;
    static GenNodeKey MakeKey(Generation gen, uint8_t nodeId);
    std::map<GenNodeKey, Guid64> genNodeToGuid_;
};

} // namespace ASFW::Discovery

