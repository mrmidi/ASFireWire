#include "DeviceRegistry.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>

#include "../Logging/Logging.hpp"

namespace ASFW::Discovery {

namespace {

constexpr uint32_t kUnitSpecIdTA = 0x00A02D;
constexpr uint32_t kUnitSpecIdSBP2 = 0x00609E;
constexpr uint32_t kUnitSwVersionSBP2 = 0x010483;

[[nodiscard]] constexpr bool IsSBP2Unit(const UnitDirectory& unit) noexcept {
    return unit.unitSpecId == kUnitSpecIdSBP2 &&
           unit.unitSwVersion == kUnitSwVersionSBP2;
}

[[nodiscard]] DeviceIdentityEvidence BuildIdentityEvidence(const ConfigROM& rom) {
    // Preserve bus-information, root-directory, and every unit-directory view
    // independently. The TCAT parser models these as separate structures and
    // enumerates all units; it never promotes one into a canonical identity:
    // references/alsa-userspace-control-protocols-impl/protocols/dice/src/tcat/config_rom.rs:11-50,71-130.
    DeviceIdentityEvidence evidence{};
    evidence.observedGuid = rom.bib.guid;
    evidence.nodeVendorOui = static_cast<uint32_t>((rom.bib.guid >> 40U) & 0x00FF'FFFFU);
    evidence.rootVendorName = rom.vendorName;
    evidence.rootModelName = rom.modelName;

    const size_t busInfoQuadletCount = std::min(
        rom.rawQuadlets.size(), static_cast<size_t>(rom.bib.busInfoLength) + 1U);
    evidence.rawBusInfoQuadlets.assign(rom.rawQuadlets.begin(),
                                       rom.rawQuadlets.begin() + busInfoQuadletCount);

    for (const auto& entry : rom.rootDirMinimal) {
        if (entry.key == CfgKey::VendorId && !evidence.rootVendorId.has_value()) {
            evidence.rootVendorId = entry.value;
        } else if (entry.key == CfgKey::ModelId && !evidence.rootModelId.has_value()) {
            evidence.rootModelId = entry.value;
        }
    }

    evidence.units.reserve(rom.unitDirectories.size());
    for (const auto& unit : rom.unitDirectories) {
        UnitIdentityEvidence unitEvidence{};
        unitEvidence.unitDirectoryOffset = unit.offsetQuadlets;
        unitEvidence.vendorId = unit.vendorId;
        unitEvidence.modelId = unit.modelId;
        if (unit.unitSpecId != 0) {
            unitEvidence.specifierId = unit.unitSpecId;
        }
        if (unit.unitSwVersion != 0) {
            unitEvidence.version = unit.unitSwVersion;
        }
        unitEvidence.logicalUnitNumber = unit.logicalUnitNumber;
        unitEvidence.vendorName = unit.vendorName;
        unitEvidence.modelName = unit.modelName;
        evidence.units.push_back(std::move(unitEvidence));
    }

    // Some minimal devices place unit keys directly in the root directory.
    // Preserve that as a real offset-zero unit without flattening it into the
    // physical-device identity.
    if (evidence.units.empty()) {
        UnitIdentityEvidence rootUnit{};
        bool hasUnitEvidence = false;
        for (const auto& entry : rom.rootDirMinimal) {
            switch (entry.key) {
                case CfgKey::Unit_Spec_Id:
                    rootUnit.specifierId = entry.value;
                    hasUnitEvidence = true;
                    break;
                case CfgKey::Unit_Sw_Version:
                    rootUnit.version = entry.value;
                    hasUnitEvidence = true;
                    break;
                case CfgKey::Logical_Unit_Number:
                    rootUnit.logicalUnitNumber = entry.value;
                    hasUnitEvidence = true;
                    break;
                default:
                    break;
            }
        }
        if (hasUnitEvidence) {
            evidence.units.push_back(std::move(rootUnit));
        }
    }

    return evidence;
}

[[nodiscard]] const char* QuarantineReasonString(QuarantineReason reason) noexcept {
    switch (reason) {
        case QuarantineReason::ZeroObservedGuid:
            return "zero-observed-guid";
        case QuarantineReason::DuplicateObservedGuid:
            return "duplicate-observed-guid";
        case QuarantineReason::HazardousNoProbe:
            return "hazardous-no-probe";
        case QuarantineReason::AmbiguousIdentity:
            return "ambiguous-identity";
        case QuarantineReason::InsufficientSafeEvidence:
            return "insufficient-safe-evidence";
        case QuarantineReason::UnsupportedFamily:
            return "unsupported-family";
        case QuarantineReason::None:
        default:
            return "none";
    }
}

} // namespace

DeviceRegistry::DeviceRegistry() : lock_(IOLockAlloc()) {}

DeviceRegistry::~DeviceRegistry() {
    if (lock_ != nullptr) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

std::vector<DeviceRecord>
DeviceRegistry::ReconcileGeneration(Generation gen,
                                    std::span<const DeviceObservation> observations) {
    std::unordered_map<Guid64, size_t> guidCounts;
    guidCounts.reserve(observations.size());
    for (const auto& observation : observations) {
        if (observation.rom != nullptr && observation.rom->gen == gen) {
            ++guidCounts[observation.rom->bib.guid];
        }
    }

    IOLockLock(lock_);

    // A previous collision group can never be correlated safely with a new
    // generation. Likewise, a previously unique instance must not be reused
    // when the same observed EUI-64 is now present on multiple nodes. FFADO's
    // ProFire workaround adds node ID to 0x000d6c0404000002
    // (references/libffado-2.5.0/src/libieee1394/configrom.cpp:207-218);
    // ASFW deliberately retains the observed value and quarantines instead.
    for (const auto& [observedGuid, count] : guidCounts) {
        if (observedGuid == 0) {
            continue;
        }
        if (count > 1U) {
            RetirePriorCollisionGroupLocked(observedGuid, gen);
            continue;
        }

        std::vector<DeviceInstanceId> staleCollisionInstances;
        for (const auto& [instanceId, device] : devicesByInstance_) {
            if (device.ObservedGuid() == observedGuid && device.gen != gen &&
                device.quarantineReason == QuarantineReason::DuplicateObservedGuid) {
                staleCollisionInstances.push_back(instanceId);
            }
        }
        for (const auto instanceId : staleCollisionInstances) {
            EraseInstanceLocked(instanceId);
        }
    }

    std::vector<DeviceRecord> reconciled;
    reconciled.reserve(observations.size());
    for (const auto& observation : observations) {
        if (observation.rom == nullptr || observation.rom->gen != gen ||
            !TryOperationalNodeId(observation.rom->nodeId).has_value()) {
            continue;
        }
        const size_t count = guidCounts[observation.rom->bib.guid];
        reconciled.push_back(
            UpsertOneLocked(*observation.rom, observation.link, count, true));
    }

    IOLockUnlock(lock_);
    return reconciled;
}

DeviceRecord DeviceRegistry::UpsertFromROM(const ConfigROM& rom, const LinkPolicy& link) {
    IOLockLock(lock_);
    DeviceRecord result = UpsertOneLocked(rom, link, 1U, false);
    IOLockUnlock(lock_);
    return result;
}

DeviceRecord DeviceRegistry::UpsertOneLocked(const ConfigROM& rom,
                                             const LinkPolicy& link,
                                             size_t observedGuidCount,
                                             bool batchReconcile) {
    const auto operationalNodeId = TryOperationalNodeId(rom.nodeId);
    const GenNodeKey key = operationalNodeId.has_value()
                               ? MakeKey(rom.gen, *operationalNodeId)
                               : GenNodeKey{0};

    DeviceInstanceId instanceId{};
    if (operationalNodeId.has_value()) {
        if (const auto current = genNodeToInstance_.find(key);
            current != genNodeToInstance_.end()) {
            const auto existing = devicesByInstance_.find(current->second);
            if (existing != devicesByInstance_.end() &&
                existing->second.ObservedGuid() == rom.bib.guid) {
                instanceId = existing->first;
            } else {
                genNodeToInstance_.erase(current);
            }
        }
    }

    const bool duplicated = batchReconcile && rom.bib.guid != 0 && observedGuidCount > 1U;
    if (!instanceId && rom.bib.guid != 0 && !duplicated) {
        instanceId = ReusableInstanceLocked(rom.bib.guid).value_or(DeviceInstanceId{});
    }
    if (!instanceId) {
        instanceId = AllocateDeviceInstanceIdLocked();
    }

    auto [recordIt, inserted] = devicesByInstance_.try_emplace(instanceId);
    auto& device = recordIt->second;
    if (inserted) {
        device.instanceId = instanceId;
        device.routeEpoch = AllocateRouteEpochLocked();
    } else if (device.gen != rom.gen || device.nodeId != rom.nodeId ||
               !HasLiveRoute(device)) {
        device.routeEpoch = AllocateRouteEpochLocked();
    }

    device.identity = BuildIdentityEvidence(rom);
    device.kind = ClassifyDevice(rom);
    device.isAudioCandidate = IsAudioCandidate(rom);
    device.gen = rom.gen;
    device.nodeId = rom.nodeId;
    device.link = link;
    device.quarantineReason = QuarantineReason::None;

    if (rom.bib.guid == 0) {
        device.state = LifeState::Quarantined;
        device.quarantineReason = QuarantineReason::ZeroObservedGuid;
    } else if (duplicated) {
        device.state = LifeState::Quarantined;
        device.quarantineReason = QuarantineReason::DuplicateObservedGuid;
    } else {
        // Reconciliation runs only after the complete Config-ROM observation
        // batch is available. A non-quarantined record is therefore routable
        // immediately; no later component may silently promote identity state.
        device.state = LifeState::Ready;
    }

    const uint32_t maxFromRec32 = ASFW::FW::MaxAsyncPayloadBytesFromMaxRec(rom.bib.maxRec);
    const uint16_t maxFromRec = maxFromRec32 > std::numeric_limits<uint16_t>::max()
                                    ? std::numeric_limits<uint16_t>::max()
                                    : static_cast<uint16_t>(maxFromRec32);
    if (device.link.maxPayloadBytes > maxFromRec) {
        device.link.maxPayloadBytes = maxFromRec;
    }

    if (operationalNodeId.has_value()) {
        genNodeToInstance_[key] = instanceId;
    }

    ASFW_LOG(Discovery,
             "[DeviceIdentity] instance=%llu observedGuid=0x%016llx gen=%u node=%u "
             "rootVendor=0x%06x rootModel=0x%06x units=%zu quarantine=%{public}s",
             instanceId.value, device.ObservedGuid(), rom.gen.value, rom.nodeId,
             device.RootVendorIdOrZero(), device.RootModelIdOrZero(),
             device.identity.units.size(), QuarantineReasonString(device.quarantineReason));
    return device;
}

std::optional<DeviceInstanceId>
DeviceRegistry::ReusableInstanceLocked(Guid64 observedGuid) const {
    std::optional<DeviceInstanceId> match;
    for (const auto& [instanceId, device] : devicesByInstance_) {
        if (device.ObservedGuid() != observedGuid ||
            device.quarantineReason == QuarantineReason::DuplicateObservedGuid ||
            device.quarantineReason == QuarantineReason::ZeroObservedGuid) {
            continue;
        }
        if (match.has_value()) {
            return std::nullopt;
        }
        match = instanceId;
    }
    return match;
}

void DeviceRegistry::RetirePriorCollisionGroupLocked(Guid64 observedGuid,
                                                      Generation currentGeneration) {
    std::vector<DeviceInstanceId> staleInstances;
    for (const auto& [instanceId, device] : devicesByInstance_) {
        if (device.ObservedGuid() == observedGuid && device.gen != currentGeneration) {
            staleInstances.push_back(instanceId);
        }
    }
    for (const auto instanceId : staleInstances) {
        EraseInstanceLocked(instanceId);
    }
}

void DeviceRegistry::RetireDevice(DeviceInstanceId instanceId) {
    IOLockLock(lock_);
    EraseInstanceLocked(instanceId);
    IOLockUnlock(lock_);
}

void DeviceRegistry::Quarantine(DeviceInstanceId instanceId, QuarantineReason reason) {
    if (reason == QuarantineReason::None) {
        return;
    }
    IOLockLock(lock_);
    if (auto it = devicesByInstance_.find(instanceId); it != devicesByInstance_.end()) {
        it->second.state = LifeState::Quarantined;
        it->second.quarantineReason = reason;
        it->second.routeEpoch = AllocateRouteEpochLocked();
    }
    IOLockUnlock(lock_);
}

void DeviceRegistry::EraseInstanceLocked(DeviceInstanceId instanceId) {
    devicesByInstance_.erase(instanceId);
    for (auto it = genNodeToInstance_.begin(); it != genNodeToInstance_.end();) {
        it = it->second == instanceId ? genNodeToInstance_.erase(it) : std::next(it);
    }
}

void DeviceRegistry::InvalidateLiveMappingsForBusReset() {
    IOLockLock(lock_);
    size_t invalidatedCount = 0;
    for (auto& [instanceId, device] : devicesByInstance_) {
        if (!TryOperationalNodeId(device.nodeId).has_value()) {
            continue;
        }
        device.nodeId = kInvalidNodeId;
        device.routeEpoch = AllocateRouteEpochLocked();
        ++invalidatedCount;
    }
    genNodeToInstance_.clear();
    IOLockUnlock(lock_);

    ASFW_LOG(Discovery,
             "Bus reset: invalidated %zu runtime instance routes pending ROM rescan",
             invalidatedCount);
}

std::optional<DeviceRecord> DeviceRegistry::Snapshot(DeviceInstanceId instanceId) const {
    IOLockLock(lock_);
    const auto it = devicesByInstance_.find(instanceId);
    const auto snapshot = it != devicesByInstance_.end()
                              ? std::optional<DeviceRecord>{it->second}
                              : std::nullopt;
    IOLockUnlock(lock_);
    return snapshot;
}

std::optional<DeviceRecord> DeviceRegistry::SnapshotByNode(Generation gen,
                                                            uint8_t nodeId) const {
    IOLockLock(lock_);
    const auto route = genNodeToInstance_.find(MakeKey(gen, nodeId));
    const auto record = route != genNodeToInstance_.end()
                            ? devicesByInstance_.find(route->second)
                            : devicesByInstance_.end();
    const auto snapshot = record != devicesByInstance_.end()
                              ? std::optional<DeviceRecord>{record->second}
                              : std::nullopt;
    IOLockUnlock(lock_);
    return snapshot;
}

std::vector<DeviceRecord>
DeviceRegistry::FindInstancesByObservedGuid(Guid64 observedGuid) const {
    IOLockLock(lock_);
    std::vector<DeviceRecord> matches;
    for (const auto& [instanceId, device] : devicesByInstance_) {
        if (device.ObservedGuid() == observedGuid) {
            matches.push_back(device);
        }
    }
    IOLockUnlock(lock_);
    return matches;
}

std::optional<DeviceRouteToken>
DeviceRegistry::CurrentRoute(DeviceInstanceId instanceId) const {
    IOLockLock(lock_);
    const auto it = devicesByInstance_.find(instanceId);
    const auto token = it != devicesByInstance_.end() && HasLiveRoute(it->second)
                           ? std::optional<DeviceRouteToken>{MakeRouteToken(it->second)}
                           : std::nullopt;
    IOLockUnlock(lock_);
    return token;
}

bool DeviceRegistry::IsCurrent(const DeviceRouteToken& token) const noexcept {
    if (!token) {
        return false;
    }
    IOLockLock(lock_);
    const auto it = devicesByInstance_.find(token.deviceInstanceId);
    const bool current = it != devicesByInstance_.end() && HasLiveRoute(it->second) &&
                         it->second.routeEpoch == token.routeEpoch &&
                         it->second.gen == token.generation &&
                         it->second.nodeId == token.nodeId;
    IOLockUnlock(lock_);
    return current;
}

std::vector<DeviceRecord> DeviceRegistry::LiveDevices(Generation gen) const {
    IOLockLock(lock_);
    std::vector<DeviceRecord> result;
    for (const auto& [instanceId, device] : devicesByInstance_) {
        if (device.gen == gen && TryOperationalNodeId(device.nodeId).has_value()) {
            result.push_back(device);
        }
    }
    IOLockUnlock(lock_);
    return result;
}

std::vector<DeviceRecord> DeviceRegistry::AllDevices() const {
    IOLockLock(lock_);
    std::vector<DeviceRecord> result;
    result.reserve(devicesByInstance_.size());
    for (const auto& [instanceId, device] : devicesByInstance_) {
        result.push_back(device);
    }
    IOLockUnlock(lock_);
    return result;
}

void DeviceRegistry::Clear() {
    IOLockLock(lock_);
    devicesByInstance_.clear();
    genNodeToInstance_.clear();
    nextDeviceInstanceId_ = 0;
    nextRouteEpoch_ = 0;
    IOLockUnlock(lock_);
}

DeviceKind DeviceRegistry::ClassifyDevice(const ConfigROM& rom) const {
    for (const auto& unit : rom.unitDirectories) {
        if (IsSBP2Unit(unit)) {
            return DeviceKind::Storage;
        }
        if (unit.unitSpecId == kUnitSpecIdTA) {
            return DeviceKind::TA_61883;
        }
    }
    return DeviceKind::Unknown;
}

bool DeviceRegistry::IsAudioCandidate(const ConfigROM& rom) const {
    return std::ranges::any_of(rom.unitDirectories, [](const UnitDirectory& unit) {
        return unit.unitSpecId == kUnitSpecIdTA;
    });
}

DeviceInstanceId DeviceRegistry::AllocateDeviceInstanceIdLocked() noexcept {
    ++nextDeviceInstanceId_;
    if (nextDeviceInstanceId_ == 0) {
        ++nextDeviceInstanceId_;
    }
    return DeviceInstanceId{nextDeviceInstanceId_};
}

uint64_t DeviceRegistry::AllocateRouteEpochLocked() noexcept {
    ++nextRouteEpoch_;
    if (nextRouteEpoch_ == 0) {
        ++nextRouteEpoch_;
    }
    return nextRouteEpoch_;
}

bool DeviceRegistry::HasLiveRoute(const DeviceRecord& device) noexcept {
    return device.state != LifeState::Lost && device.state != LifeState::Quarantined &&
           TryOperationalNodeId(device.nodeId).has_value();
}

DeviceRouteToken DeviceRegistry::MakeRouteToken(const DeviceRecord& device) noexcept {
    return DeviceRouteToken{.deviceInstanceId = device.instanceId,
                            .routeEpoch = device.routeEpoch,
                            .generation = device.gen,
                            .nodeId = device.nodeId};
}

DeviceRegistry::GenNodeKey DeviceRegistry::MakeKey(Generation gen, uint8_t nodeId) {
    return (gen.value << 8U) | static_cast<uint32_t>(nodeId);
}

} // namespace ASFW::Discovery
