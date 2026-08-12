#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include <DriverKit/IOLib.h>

#include "DeviceRouteToken.hpp"
#include "DiscoveryTypes.hpp"

namespace ASFW::Discovery {

// Registry for runtime device instances and their current generation/node
// routes.  EUI-64/GUID values are retained as observed Config-ROM evidence and
// deliberately are not primary keys.
class DeviceRegistry {
public:
    DeviceRegistry();
    ~DeviceRegistry();

    DeviceRegistry(const DeviceRegistry&) = delete;
    DeviceRegistry& operator=(const DeviceRegistry&) = delete;

    // Production discovery entry point.  The whole generation is reconciled at
    // once so duplicate and zero EUI-64 values are handled before any route is
    // exposed to a protocol layer.
    [[nodiscard]] std::vector<DeviceRecord>
    ReconcileGeneration(Generation gen, std::span<const DeviceObservation> observations);

    // Single-observation helper used by focused host tests and explicit route
    // fixtures. Production discovery uses ReconcileGeneration().
    [[nodiscard]] DeviceRecord UpsertFromROM(const ConfigROM& rom, const LinkPolicy& link);

    void RetireDevice(DeviceInstanceId instanceId);
    void Quarantine(DeviceInstanceId instanceId, QuarantineReason reason);

    // A reset invalidates every route immediately. Instances remain suspended
    // candidates and may be rebound by the next complete generation scan.
    void InvalidateLiveMappingsForBusReset();

    [[nodiscard]] std::optional<DeviceRecord> Snapshot(DeviceInstanceId instanceId) const;
    [[nodiscard]] std::optional<DeviceRecord> SnapshotByNode(Generation gen,
                                                              uint8_t nodeId) const;
    [[nodiscard]] std::vector<DeviceRecord>
    FindInstancesByObservedGuid(Guid64 observedGuid) const;

    [[nodiscard]] std::optional<DeviceRouteToken>
    CurrentRoute(DeviceInstanceId instanceId) const;
    [[nodiscard]] bool IsCurrent(const DeviceRouteToken& token) const noexcept;

    [[nodiscard]] std::vector<DeviceRecord> LiveDevices(Generation gen) const;
    [[nodiscard]] std::vector<DeviceRecord> AllDevices() const;

    void Clear();

private:
    using GenNodeKey = uint32_t;

    [[nodiscard]] DeviceRecord UpsertOneLocked(const ConfigROM& rom,
                                               const LinkPolicy& link,
                                               size_t observedGuidCount,
                                               bool batchReconcile);
    [[nodiscard]] std::optional<DeviceInstanceId>
    ReusableInstanceLocked(Guid64 observedGuid) const;
    void RetirePriorCollisionGroupLocked(Guid64 observedGuid, Generation currentGeneration);
    void EraseInstanceLocked(DeviceInstanceId instanceId);

    [[nodiscard]] DeviceKind ClassifyDevice(const ConfigROM& rom) const;
    [[nodiscard]] bool IsAudioCandidate(const ConfigROM& rom) const;

    [[nodiscard]] DeviceInstanceId AllocateDeviceInstanceIdLocked() noexcept;
    [[nodiscard]] uint64_t AllocateRouteEpochLocked() noexcept;
    [[nodiscard]] static bool HasLiveRoute(const DeviceRecord& device) noexcept;
    [[nodiscard]] static DeviceRouteToken MakeRouteToken(const DeviceRecord& device) noexcept;
    [[nodiscard]] static GenNodeKey MakeKey(Generation gen, uint8_t nodeId);

    mutable IOLock* lock_{nullptr};
    std::map<DeviceInstanceId, DeviceRecord> devicesByInstance_;
    std::map<GenNodeKey, DeviceInstanceId> genNodeToInstance_;
    uint64_t nextDeviceInstanceId_{0};
    uint64_t nextRouteEpoch_{0};
};

} // namespace ASFW::Discovery
