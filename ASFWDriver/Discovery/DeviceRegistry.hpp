#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include <DriverKit/IOLib.h>

#include "DeviceRouteToken.hpp"
#include "DiscoveryTypes.hpp"

namespace ASFW::Discovery {

// Stable GUID-keyed device registry with per-generation live mapping.
// Maintains device identity across bus resets and performs audio classification.
// See documentation:
//   - documentation/TOKEN_BASED_LIFECYCLE.md
//   - ASFWDriver/Service/Lifecycle/RUNTIME_LIFECYCLE_CONTRACT.md
//
class DeviceRegistry {
public:
    DeviceRegistry();
    ~DeviceRegistry();

    // Create or update device record from parsed ROM. Returns a stable snapshot;
    // no registry-owned mutable record escapes its lock.
    // Pure metadata: device-specific runtime protocol creation is owned by the Audio layer
    // (Audio::AudioRuntimeRegistry), triggered from the controller discovery path.
    [[nodiscard]] DeviceRecord UpsertFromROM(const ConfigROM& rom, const LinkPolicy& link);

    // Mark device as discovered (seen in Self-ID, before ROM fetch)
    void MarkDiscovered(Generation gen, uint8_t nodeId);

    // Handle duplicate GUID detection within same generation
    void MarkDuplicateGuid(Generation gen, Guid64 guid, uint8_t nodeId);

    // Mark device as lost (not present in current generation)
    void MarkLost(Generation gen, uint8_t nodeId);

    // Retire an actually removed device. A later replug of the same GUID gets a
    // new device incarnation and cannot validate old asynchronous callbacks.
    void RetireDevice(Guid64 guid);

    // A bus reset invalidates every generation/node mapping immediately. Device
    // identity remains cached by GUID and is rebound only after a new ROM scan.
    void InvalidateLiveMappingsForBusReset();

    // Locked value snapshots. Callers must act after the lock is released; no
    // registry lock is ever held while invoking backend or DriverKit code.
    [[nodiscard]] std::optional<DeviceRecord> SnapshotByGuid(Guid64 guid) const;
    [[nodiscard]] std::optional<DeviceRecord> SnapshotByNode(Generation gen, uint8_t nodeId) const;

    [[nodiscard]] std::optional<DeviceRouteToken> CurrentRoute(Guid64 guid) const;
    [[nodiscard]] bool IsCurrent(const DeviceRouteToken& token) const noexcept;

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
    mutable IOLock* lock_{nullptr};
    std::map<Guid64, DeviceRecord> devicesByGuid_;

    // Preserve the last incarnation after a record is retired so replugging the
    // same GUID cannot accidentally validate callbacks from its old service.
    std::map<Guid64, uint64_t> lastDeviceIncarnationByGuid_;
    uint64_t nextRouteEpoch_{0};

    // Secondary index: (generation, nodeId) → GUID for fast per-generation lookup
    using GenNodeKey = uint32_t;
    static GenNodeKey MakeKey(Generation gen, uint8_t nodeId);
    std::map<GenNodeKey, Guid64> genNodeToGuid_;

    [[nodiscard]] uint64_t AllocateRouteEpochLocked() noexcept;
    [[nodiscard]] static bool HasLiveRoute(const DeviceRecord& device) noexcept;
    [[nodiscard]] static DeviceRouteToken MakeRouteToken(const DeviceRecord& device) noexcept;
};

} // namespace ASFW::Discovery
