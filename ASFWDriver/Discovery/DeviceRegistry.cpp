#include "DeviceRegistry.hpp"
#include <algorithm>
#include <limits>
#include "../Logging/Logging.hpp"
#include "../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"

namespace ASFW::Discovery {

constexpr uint32_t kUnitSpecId_TA = 0x00A02D;
constexpr uint32_t kUnitSpecId_AVC = 0x00A02D;
constexpr uint32_t kUnitSpecId_SBP2 = 0x00609E; // SBP-2 Unit_Spec_Id
constexpr uint32_t kUnitSwVersion_SBP2 = 0x010483; // SBP-2 Unit_Sw_Version

[[nodiscard]] constexpr bool IsSBP2Unit(const UnitDirectory& unit) noexcept {
    return unit.unitSpecId == kUnitSpecId_SBP2 && unit.unitSwVersion == kUnitSwVersion_SBP2;
}

namespace {

// Fill DeviceRecord::identity — the raw Config-ROM evidence — from the parsed
// ROM. Presence is recorded as presence: an optional is engaged when the ROM
// carried the key, whatever its value. A device that publishes model_id 0
// (MOTU does) is therefore distinguishable from one that publishes no model_id
// at all, which a uint32_t{0} cannot express.
void PopulateIdentityEvidence(DeviceRecord& device, const ConfigROM& rom) {
    DeviceIdentityEvidence evidence{};
    evidence.observedGuid = rom.bib.guid;
    evidence.nodeVendorOui = static_cast<uint32_t>((rom.bib.guid >> 40) & 0xFFFFFFULL);

    // The bus info block is the leading quadlets of the ROM image, in wire order.
    constexpr size_t kBusInfoQuadlets = 5;
    const size_t busInfoCount = (rom.rawQuadlets.size() < kBusInfoQuadlets)
                                    ? rom.rawQuadlets.size()
                                    : kBusInfoQuadlets;
    evidence.rawBusInfoQuadlets.assign(rom.rawQuadlets.begin(),
                                       rom.rawQuadlets.begin() +
                                           static_cast<std::ptrdiff_t>(busInfoCount));

    for (const auto& entry : rom.rootDirMinimal) {
        if (entry.key == CfgKey::VendorId) {
            evidence.rootVendorId = entry.value;
        } else if (entry.key == CfgKey::ModelId) {
            evidence.rootModelId = entry.value;
        }
    }
    evidence.rootVendorName = rom.vendorName;
    evidence.rootModelName = rom.modelName;

    // Every unit directory, kept separate. The flat unitSpecId/unitSwVersion
    // pair below can only describe one unit, and worse, can take the two halves
    // from different ones; TC Applied Technologies devices publish an audio unit
    // and a MIDI unit.
    evidence.units.reserve(rom.unitDirectories.size());
    for (const auto& unit : rom.unitDirectories) {
        UnitIdentityEvidence unitEvidence{};
        unitEvidence.unitDirectoryOffset = unit.offsetQuadlets;
        if (unit.unitSpecId != 0) {
            unitEvidence.specifierId = unit.unitSpecId;
        }
        if (unit.unitSwVersion != 0) {
            unitEvidence.version = unit.unitSwVersion;
        }
        unitEvidence.modelId = unit.modelId;
        unitEvidence.modelName = unit.modelName;
        unitEvidence.logicalUnitNumber = unit.logicalUnitNumber;
        evidence.units.push_back(std::move(unitEvidence));
    }

    device.identity = std::move(evidence);
}

void PopulateDeviceIdentity(DeviceRecord& device, const ConfigROM& rom) {
    PopulateIdentityEvidence(device, rom);

    // ---- Flat compatibility shim, seeded from the evidence above ----
    // These flat fields are legacy convenience mirrors of the raw ROM evidence
    // kept for non-catalog consumers and logging. Identity resolution and variant
    // determination (including GUID quirks such as Focusrite Saffire Pro 40 TCD3070)
    // are owned exclusively by AudioDeviceCatalog::Resolve() using device.identity.
    device.vendorId = device.identity.rootVendorId.value_or(0);
    device.modelId = device.identity.rootModelId.value_or(0);

    device.unitSpecId.reset();
    device.unitSwVersion.reset();
    for (const auto& unit : rom.unitDirectories) {
        if (unit.unitSpecId != 0) {
            device.unitSpecId = unit.unitSpecId;
        }
        if (unit.unitSwVersion != 0) {
            device.unitSwVersion = unit.unitSwVersion;
        }
        if (device.unitSpecId.has_value() && device.unitSwVersion.has_value()) {
            break;
        }
    }

    device.vendorName = rom.vendorName;
    device.modelName = rom.modelName;
}

const char* DeviceKindString(DeviceKind kind) noexcept {
    switch (kind) {
        case DeviceKind::AV_C:
            return "AV_C";
        case DeviceKind::TA_61883:
            return "TA_61883";
        case DeviceKind::VendorSpecificAudio:
            return "VendorAudio";
        case DeviceKind::Storage:
            return "Storage";
        case DeviceKind::Camera:
            return "Camera";
        default:
            return "Unknown";
    }
}

void LogDeviceUpsert(Guid64 guid, const DeviceRecord& device, const ConfigROM& rom) {
    const char* kindStr = DeviceKindString(device.kind);
    if (!device.vendorName.empty() && !device.modelName.empty()) { // NOSONAR(cpp:S3923): branches log different diagnostic messages
        ASFW_LOG(Discovery, "Device upsert: GUID=0x%016llx vendor=0x%06x(%{public}s) model=0x%06x(%{public}s) "
                 "kind=%{public}s audioCandidate=%d node=%u gen=%u",
                 guid, device.vendorId, device.vendorName.c_str(),
                 device.modelId, device.modelName.c_str(), kindStr,
                 device.isAudioCandidate, rom.nodeId, rom.gen.value);
        return;
    }

    ASFW_LOG(Discovery, "Device upsert: GUID=0x%016llx vendor=0x%06x model=0x%06x "
             "kind=%{public}s audioCandidate=%d node=%u gen=%u",
             guid, device.vendorId, device.modelId, kindStr,
             device.isAudioCandidate, rom.nodeId, rom.gen.value);
}

} // namespace

DeviceRegistry::DeviceRegistry()
    : lock_(IOLockAlloc()) {}

DeviceRegistry::~DeviceRegistry() {
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

DeviceRecord DeviceRegistry::UpsertFromROM(const ConfigROM& rom, const LinkPolicy& link) {
    IOLockLock(lock_);
    const Guid64 guid = rom.bib.guid;
    const auto operationalNodeId = TryOperationalNodeId(rom.nodeId);

    auto [it, inserted] = devicesByGuid_.try_emplace(guid);
    auto& device = it->second;
    if (inserted) {
        device.instanceId = AllocateDeviceInstanceIdLocked();
        device.deviceIncarnation = ++lastDeviceIncarnationByGuid_[guid];
        device.routeEpoch = AllocateRouteEpochLocked();
    } else if (device.gen != rom.gen || device.nodeId != rom.nodeId ||
               !HasLiveRoute(device)) {
        device.routeEpoch = AllocateRouteEpochLocked();
    }
    device.guid = guid;
    PopulateDeviceIdentity(device, rom);

    // Ask the unified AudioDeviceCatalog for resolution (identity enrichment and candidacy).
    const auto endpointPlan =
        DeviceProfiles::Audio::AudioDeviceCatalog::Resolve(device.identity);
    if (endpointPlan.has_value()) {
        device.quarantineReason = QuarantineReason::None;
        if (device.vendorName.empty() && !endpointPlan->vendorName.empty()) {
            device.vendorName = endpointPlan->vendorName;
        }
        if (!endpointPlan->modelName.empty()) {
            device.modelName = endpointPlan->modelName;
        }
        if (endpointPlan->family == DeviceProfiles::Audio::AudioFamilyProviderId::DICE ||
            endpointPlan->family == DeviceProfiles::Audio::AudioFamilyProviderId::MotuRegister) {
            device.kind = DeviceKind::VendorSpecificAudio;
            device.isAudioCandidate = true;
        } else {
            device.kind = ClassifyDevice(rom);
            device.isAudioCandidate = IsAudioCandidate(rom);
        }
        ASFW_LOG(Discovery,
                 "Catalog resolved plan for GUID=0x%016llx: family=%u support=%u builder=%u candidate=%d",
                 guid,
                 static_cast<unsigned>(endpointPlan->family),
                 static_cast<unsigned>(endpointPlan->support),
                 static_cast<unsigned>(endpointPlan->profileBuilder),
                 device.isAudioCandidate);
    } else {
        device.kind = ClassifyDevice(rom);
        if (endpointPlan.error() ==
            DeviceProfiles::Audio::CatalogResolutionError::HazardousIdentity) {
            device.isAudioCandidate = false;
            device.quarantineReason = QuarantineReason::HazardousNoProbe;
            ASFW_LOG(Discovery, "Device GUID=0x%016llx quarantined by catalog safety rule", guid);
        } else if (endpointPlan.error() ==
                   DeviceProfiles::Audio::CatalogResolutionError::AmbiguousIdentity) {
            device.isAudioCandidate = false;
            device.quarantineReason = QuarantineReason::AmbiguousIdentity;
            ASFW_LOG(Discovery, "Device GUID=0x%016llx rejected due to ambiguous catalog identity", guid);
        } else {
            device.quarantineReason = QuarantineReason::None;
            device.isAudioCandidate = IsAudioCandidate(rom);
        }
    }

    // TODO: Generic AV/C devices should work purely via MusicSubunit discovery; vendor protocols are only for extra controls.
    // TODO: Generic DICE/TCAT discovery (non-hardcoded vendor/model) is not implemented yet.
    
    device.gen = rom.gen;
    device.nodeId = rom.nodeId;
    device.link = link;

    // Clamp max async payload by remote MaxRec code (BIB bus options).
    const uint32_t maxFromRec32 = ASFW::FW::MaxAsyncPayloadBytesFromMaxRec(rom.bib.maxRec);
    const uint16_t maxFromRec = (maxFromRec32 > std::numeric_limits<uint16_t>::max())
                                    ? std::numeric_limits<uint16_t>::max()
                                    : static_cast<uint16_t>(maxFromRec32);
    if (device.link.maxPayloadBytes > maxFromRec) {
        device.link.maxPayloadBytes = maxFromRec;
    }
    if (device.quarantineReason != QuarantineReason::None) {
        device.state = LifeState::Quarantined;
    } else {
        device.state = LifeState::Identified;
    }

    if (operationalNodeId.has_value()) {
        GenNodeKey key = MakeKey(rom.gen, *operationalNodeId);
        genNodeToGuid_[key] = guid;
    } else {
        ASFW_LOG(Discovery, "Skipping node-index update for GUID=0x%016llx with invalid nodeId=%u",
                 guid, rom.nodeId);
    }

    LogDeviceUpsert(guid, device, rom);
    DeviceRecord snapshot = device;
    IOLockUnlock(lock_);
    return snapshot;
}

void DeviceRegistry::MarkDiscovered(Generation gen, uint8_t nodeId) {
    IOLockLock(lock_);
    // Check if we already know this (gen, nodeId)
    GenNodeKey key = MakeKey(gen, nodeId);
    auto it = genNodeToGuid_.find(key);
    
    if (it != genNodeToGuid_.end()) {
        // Update existing device
        Guid64 guid = it->second;
        auto devIt = devicesByGuid_.find(guid);
        if (devIt != devicesByGuid_.end()) {
            devIt->second.state = LifeState::Discovered;
            devIt->second.gen = gen;
            devIt->second.nodeId = nodeId;
        }
    }
    // If not found, we'll create it later when ROM arrives
    IOLockUnlock(lock_);
}

void DeviceRegistry::MarkDuplicateGuid(Generation gen, Guid64 guid, uint8_t nodeId) {
    IOLockLock(lock_);
    auto it = devicesByGuid_.find(guid);
    if (it != devicesByGuid_.end()) {
        it->second.state = LifeState::Quarantined;
        ASFW_LOG(Discovery, "⚠️  Duplicate GUID detected: 0x%016llx node=%u gen=%u (quarantined)",
                 guid, nodeId, gen.value);
    }
    IOLockUnlock(lock_);
}

void DeviceRegistry::MarkLost(Generation gen, uint8_t nodeId) {
    IOLockLock(lock_);
    GenNodeKey key = MakeKey(gen, nodeId);
    auto it = genNodeToGuid_.find(key);
    
    if (it != genNodeToGuid_.end()) {
        Guid64 guid = it->second;
        auto devIt = devicesByGuid_.find(guid);
        if (devIt != devicesByGuid_.end()) {
            devIt->second.state = LifeState::Lost;
            devIt->second.nodeId = kInvalidNodeId;
            devIt->second.routeEpoch = AllocateRouteEpochLocked();
            ASFW_LOG(Discovery, "Device lost: GUID=0x%016llx node=%u gen=%u",
                     guid, nodeId, gen.value);
        }
        // Remove from secondary index
        genNodeToGuid_.erase(it);
    }
    IOLockUnlock(lock_);
}

void DeviceRegistry::RetireDevice(Guid64 guid) {
    IOLockLock(lock_);
    auto it = devicesByGuid_.find(guid);
    if (it != devicesByGuid_.end()) {
        lastDeviceIncarnationByGuid_[guid] = it->second.deviceIncarnation;
        devicesByGuid_.erase(it);
    }
    for (auto mapping = genNodeToGuid_.begin(); mapping != genNodeToGuid_.end();) {
        mapping = (mapping->second == guid) ? genNodeToGuid_.erase(mapping) : std::next(mapping);
    }
    IOLockUnlock(lock_);
}

void DeviceRegistry::InvalidateLiveMappingsForBusReset() {
    IOLockLock(lock_);
    size_t invalidatedCount = 0;
    for (auto& entry : devicesByGuid_) {
        auto& device = entry.second;
        if (!TryOperationalNodeId(device.nodeId).has_value()) {
            continue;
        }

        device.nodeId = kInvalidNodeId;
        device.routeEpoch = AllocateRouteEpochLocked();
        ++invalidatedCount;
    }

    genNodeToGuid_.clear();
    ASFW_LOG(Discovery,
             "Bus reset: invalidated %zu live GUID-to-node mappings pending ROM rescan",
             invalidatedCount);
    IOLockUnlock(lock_);
}

std::optional<DeviceRecord> DeviceRegistry::SnapshotByGuid(Guid64 guid) const {
    IOLockLock(lock_);
    auto it = devicesByGuid_.find(guid);
    const auto snapshot = (it != devicesByGuid_.end()) ? std::optional<DeviceRecord>{it->second}
                                                        : std::nullopt;
    IOLockUnlock(lock_);
    return snapshot;
}

std::optional<DeviceRecord> DeviceRegistry::SnapshotByNode(Generation gen, uint8_t nodeId) const {
    IOLockLock(lock_);
    GenNodeKey key = MakeKey(gen, nodeId);
    auto it = genNodeToGuid_.find(key);
    const auto record = (it != genNodeToGuid_.end()) ? devicesByGuid_.find(it->second)
                                                      : devicesByGuid_.end();
    const auto snapshot = (record != devicesByGuid_.end()) ? std::optional<DeviceRecord>{record->second}
                                                            : std::nullopt;
    IOLockUnlock(lock_);
    return snapshot;
}

std::optional<DeviceRouteToken> DeviceRegistry::CurrentRoute(Guid64 guid) const {
    IOLockLock(lock_);
    const auto it = devicesByGuid_.find(guid);
    const auto token = (it != devicesByGuid_.end() && HasLiveRoute(it->second))
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
    const auto it = devicesByGuid_.find(token.guid);
    const bool current = it != devicesByGuid_.end() && HasLiveRoute(it->second) &&
                         it->second.deviceIncarnation == token.deviceIncarnation &&
                         it->second.routeEpoch == token.routeEpoch &&
                         it->second.gen == token.generation && it->second.nodeId == token.nodeId;
    IOLockUnlock(lock_);
    return current;
}

std::vector<DeviceRecord> DeviceRegistry::LiveDevices(Generation gen) const {
    IOLockLock(lock_);
    std::vector<DeviceRecord> result;
    
    for (const auto& entry : devicesByGuid_) {
        const auto& device = entry.second;
        if (device.gen == gen && HasLiveRoute(device)) {
            result.push_back(device);
        }
    }
    
    IOLockUnlock(lock_);
    return result;
}

void DeviceRegistry::Clear() {
    IOLockLock(lock_);
    devicesByGuid_.clear();
    genNodeToGuid_.clear();
    lastDeviceIncarnationByGuid_.clear();
    nextRouteEpoch_ = 0;
    IOLockUnlock(lock_);
}

DeviceKind DeviceRegistry::ClassifyDevice(const ConfigROM& rom) const {
    for (const auto& unit : rom.unitDirectories) {
        if (unit.unitSpecId == kUnitSpecId_TA) {
            return DeviceKind::TA_61883;
        }
        if (IsSBP2Unit(unit)) {
            return DeviceKind::Storage;
        }
    }

    return DeviceKind::Unknown;
}

bool DeviceRegistry::IsAudioCandidate(const ConfigROM& rom) const {
    // Device is audio candidate if:
    // 1. Unit_Spec_Id == 0x00A02D (1394 TA / AV/C)
    // 2. Has appropriate Unit_Sw_Version for audio
    
    bool hasAudioSpec = false;
    
    for (const auto& unit : rom.unitDirectories) {
        if (unit.unitSpecId == kUnitSpecId_TA || unit.unitSpecId == kUnitSpecId_AVC) {
            hasAudioSpec = true;
        }
    }
    
    return hasAudioSpec;
}

DeviceRegistry::GenNodeKey DeviceRegistry::MakeKey(Generation gen, uint8_t nodeId) {
    return (gen.value << 8) | static_cast<uint32_t>(nodeId);
}

uint64_t DeviceRegistry::AllocateRouteEpochLocked() noexcept {
    // Zero is reserved for an invalid token. Wrap is theoretically possible but
    // requires 2^64 route changes during one driver incarnation.
    ++nextRouteEpoch_;
    if (nextRouteEpoch_ == 0) {
        ++nextRouteEpoch_;
    }
    return nextRouteEpoch_;
}

DeviceInstanceId DeviceRegistry::AllocateDeviceInstanceIdLocked() noexcept {
    // Zero means "no instance", so it is never handed out.
    ++nextDeviceInstanceId_;
    if (nextDeviceInstanceId_ == 0) {
        ++nextDeviceInstanceId_;
    }
    return DeviceInstanceId{nextDeviceInstanceId_};
}

bool DeviceRegistry::HasLiveRoute(const DeviceRecord& device) noexcept {
    return device.state != LifeState::Lost && device.state != LifeState::Quarantined &&
           device.quarantineReason == QuarantineReason::None &&
           TryOperationalNodeId(device.nodeId).has_value();
}

DeviceRouteToken DeviceRegistry::MakeRouteToken(const DeviceRecord& device) noexcept {
    return DeviceRouteToken{.guid = device.guid,
                            .deviceIncarnation = device.deviceIncarnation,
                            .routeEpoch = device.routeEpoch,
                            .generation = device.gen,
                            .nodeId = device.nodeId};
}

} // namespace ASFW::Discovery
