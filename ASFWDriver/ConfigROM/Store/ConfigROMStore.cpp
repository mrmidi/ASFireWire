#include "../ConfigROMStore.hpp"

#include "../../Logging/LogConfig.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/IOLib.h>

namespace ASFW::Discovery {

namespace {

constexpr uint32_t kUnitSpecIdSBP2 = 0x00609E;
constexpr uint32_t kUnitSwVersionSBP2 = 0x010483;

class LockGuard {
  public:
    explicit LockGuard(IOLock* lock) noexcept : lock_(lock) {
        if (lock_ != nullptr) {
            IOLockLock(lock_);
        }
    }

    ~LockGuard() {
        if (lock_ != nullptr) {
            IOLockUnlock(lock_);
        }
    }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

  private:
    IOLock* lock_{nullptr};
};

[[nodiscard]] bool HasSBP2Unit(const ASFW::Discovery::ConfigROM& rom) noexcept {
    for (const auto& unit : rom.unitDirectories) {
        if (unit.unitSpecId == kUnitSpecIdSBP2 && unit.unitSwVersion == kUnitSwVersionSBP2) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool HasParsedUnitProfile(const ASFW::Discovery::ConfigROM& rom) noexcept {
    return !rom.unitDirectories.empty();
}

} // namespace

ConfigROMStore::ConfigROMStore() : lock_(IOLockAlloc()) {
    if (lock_ == nullptr) {
        ASFW_LOG(ConfigROM, "ConfigROMStore: failed to allocate lock");
    }
}

ConfigROMStore::~ConfigROMStore() {
    if (lock_ != nullptr) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) - dominated by logging macros.
void ConfigROMStore::Insert(const ConfigROM& rom) {
    LockGuard guard(lock_);

    if (rom.bib.guid == 0) {
        ASFW_LOG_V0(ConfigROM, "ConfigROMStore::Insert: Invalid ROM (GUID=0), skipping");
        return;
    }

    ConfigROM romCopy = rom;

    if (romCopy.firstSeen == Generation{0}) {
        romCopy.firstSeen = rom.gen;
    }

    if (romCopy.lastValidated == Generation{0}) {
        romCopy.lastValidated = rom.gen;
    }

    const auto nodeIdForKey = TryOperationalNodeId(romCopy.nodeId);
    if (!nodeIdForKey.has_value()) {
        ASFW_LOG_V0(ConfigROM, "ConfigROMStore::Insert: Invalid nodeId=%u for keying, skipping",
                    romCopy.nodeId);
        return;
    }

    const auto key = MakeKey(romCopy.gen, *nodeIdForKey);
    auto nodeIt = romsByGenNode_.find(key);
    // A manual or retry scan can finish with only the BIB/root-prefix after a
    // richer scan for this exact node/generation has already completed. Keep the
    // richer node entry: ExportConfigROM uses this index, not the GUID cache.
    // Without this guard the UI visibly regresses from (for example) 31 to 19
    // quadlets without a bus-generation change.
    const bool shouldUpdateNode =
        nodeIt == romsByGenNode_.end() ||
        (nodeIt->second.rawQuadlets.size() <= romCopy.rawQuadlets.size() &&
         (HasParsedUnitProfile(romCopy) || !HasParsedUnitProfile(nodeIt->second)));
    if (shouldUpdateNode) {
        romsByGenNode_[key] = romCopy;
    } else {
        ASFW_LOG_V2(ConfigROM,
                    "ConfigROMStore::Insert: retaining richer node cache node=%u gen=%u "
                    "candidateQuadlets=%zu cachedQuadlets=%zu",
                    romCopy.nodeId, romCopy.gen.value, romCopy.rawQuadlets.size(),
                    nodeIt->second.rawQuadlets.size());
    }

    auto it = romsByGuid_.find(romCopy.bib.guid);
    // Validated with Linux (core-device.c read_config_rom/fw_device_refresh) and Apple
    // (IOFireWireROMCache::hasROMChanged): the cache must converge on the most complete
    // ROM and never regress to a partial/unit-less one. Combine both fixes:
    //  - newer generation: take it, but main's guard refuses to overwrite a ROM that
    //    has a parsed unit profile with a newer one that lacks it (Apple keeps
    //    reconsidering unit-less generation-0 devices for slow unit publishers);
    //  - same generation: take it when it carries at least as many quadlets, i.e. a
    //    minimal→general growth within a generation (Apple: newBIBSize > getLength()).
    const bool shouldUpdateGuid =
        it == romsByGuid_.end() ||
        (it->second.gen.value < romCopy.gen.value &&
         (HasParsedUnitProfile(romCopy) || !HasParsedUnitProfile(it->second))) ||
        (it->second.gen == romCopy.gen &&
         it->second.rawQuadlets.size() <= romCopy.rawQuadlets.size());

    if (shouldUpdateGuid) {
        romsByGuid_[romCopy.bib.guid] = romCopy;

        ASFW_LOG_V2(ConfigROM,
                    "ConfigROMStore::Insert: GUID=0x%016llx gen=%u node=%u state=%u "
                    "rawQuadlets=%zu rootEntries=%zu unitDirs=%zu hasSBP2=%d",
                    romCopy.bib.guid, romCopy.gen.value, romCopy.nodeId,
                    static_cast<uint8_t>(romCopy.state), romCopy.rawQuadlets.size(),
                    romCopy.rootDirMinimal.size(), romCopy.unitDirectories.size(),
                    HasSBP2Unit(romCopy) ? 1 : 0);
    } else {
        ASFW_LOG_V2(ConfigROM,
                    "ConfigROMStore::Insert: retaining richer GUID cache for GUID=0x%016llx "
                    "candidateGen=%u candidateUnitDirs=%zu cachedGen=%u cachedUnitDirs=%zu",
                    romCopy.bib.guid, romCopy.gen.value, romCopy.unitDirectories.size(),
                    it->second.gen.value, it->second.unitDirectories.size());
    }
}

const ConfigROM* ConfigROMStore::FindByNode(Generation gen, uint8_t nodeId) const {
    LockGuard guard(lock_);

    const GenNodeKey key = MakeKey(gen, nodeId);
    auto it = romsByGenNode_.find(key);
    return (it != romsByGenNode_.end()) ? &it->second : nullptr;
}

const ConfigROM* ConfigROMStore::FindByNode(Generation gen, uint8_t nodeId,
                                            bool allowSuspended) const {
    LockGuard guard(lock_);

    const auto key = MakeKey(gen, nodeId);
    if (auto it = romsByGenNode_.find(key); it != romsByGenNode_.end()) {
        const auto& rom = it->second;
        if (!allowSuspended && rom.state == ROMState::Suspended) {
            return nullptr;
        }
        return &rom;
    }

    return nullptr;
}

const ConfigROM* ConfigROMStore::FindLatestForNode(uint8_t nodeId) const {
    LockGuard guard(lock_);

    const ConfigROM* latest = nullptr;
    const ConfigROM* latestWithUnitProfile = nullptr;
    for (const auto& [key, rom] : romsByGenNode_) {
        if (rom.nodeId != nodeId) {
            continue;
        }
        if (latest == nullptr || rom.gen.value > latest->gen.value) {
            latest = &rom;
        }
        if (HasParsedUnitProfile(rom) &&
            (latestWithUnitProfile == nullptr ||
             rom.gen.value > latestWithUnitProfile->gen.value)) {
            latestWithUnitProfile = &rom;
        }
    }

    if (latestWithUnitProfile != nullptr && latest != latestWithUnitProfile &&
        latest != nullptr && latestWithUnitProfile->bib.guid == latest->bib.guid) {
        ASFW_LOG_V2(ConfigROM,
                    "ConfigROMStore::FindLatestForNode: node=%u using gen=%u with unit profile "
                    "instead of partial gen=%u",
                    nodeId, latestWithUnitProfile->gen.value, latest ? latest->gen.value : 0);
        return latestWithUnitProfile;
    }

    return latest;
}

const ConfigROM* ConfigROMStore::FindByGuid(Guid64 guid) const {
    LockGuard guard(lock_);

    auto it = romsByGuid_.find(guid);
    return (it != romsByGuid_.end()) ? &it->second : nullptr;
}

std::vector<ConfigROM> ConfigROMStore::Snapshot(Generation gen) const {
    LockGuard guard(lock_);

    std::vector<ConfigROM> result;

    for (const auto& [key, rom] : romsByGenNode_) {
        if (rom.gen == gen) {
            result.push_back(rom);
        }
    }

    return result;
}

std::vector<ConfigROM> ConfigROMStore::SnapshotByState(Generation gen, ROMState state) const {
    LockGuard guard(lock_);

    std::vector<ConfigROM> result;

    for (const auto& [key, rom] : romsByGenNode_) {
        if (rom.gen == gen && rom.state == state) {
            result.push_back(rom);
        }
    }

    return result;
}

void ConfigROMStore::Clear() {
    LockGuard guard(lock_);

    romsByGenNode_.clear();
    romsByGuid_.clear();
}

void ConfigROMStore::SuspendAll(Generation newGen) {
    LockGuard guard(lock_);

    using enum ROMState;

    uint32_t suspendedCount = 0;

    for (auto& [key, rom] : romsByGenNode_) {
        if (rom.state == Fresh || rom.state == Validated) {
            rom.state = Suspended;
            suspendedCount++;
        }
    }

    for (auto& [guid, rom] : romsByGuid_) {
        if (rom.state == Fresh || rom.state == Validated) {
            rom.state = Suspended;
        }
    }

    ASFW_LOG(ConfigROM, "ConfigROMStore::SuspendAll: Suspended %u ROMs for generation %u",
             suspendedCount, newGen.value);
}

void ConfigROMStore::ValidateROM(Guid64 guid, Generation gen, uint8_t nodeId) {
    LockGuard guard(lock_);

    auto guidIt = romsByGuid_.find(guid);
    if (guidIt == romsByGuid_.end()) {
        ASFW_LOG(ConfigROM, "ConfigROMStore::ValidateROM: GUID 0x%016llx not found", guid);
        return;
    }

    auto& rom = guidIt->second;

    if (rom.state != ROMState::Suspended) {
        ASFW_LOG(ConfigROM,
                 "ConfigROMStore::ValidateROM: GUID 0x%016llx not in suspended state (state=%u)",
                 guid, static_cast<uint8_t>(rom.state));
        return;
    }

    if (rom.nodeId != nodeId) {
        ASFW_LOG(ConfigROM,
                 "ConfigROMStore::ValidateROM: GUID 0x%016llx moved node %u→%u in gen %u", guid,
                 rom.nodeId, nodeId, gen.value);
        rom.nodeId = nodeId;
    }

    rom.gen = gen;
    rom.state = ROMState::Validated;
    rom.lastValidated = gen;

    const GenNodeKey newKey = MakeKey(gen, nodeId);
    romsByGenNode_[newKey] = rom;

    ASFW_LOG(ConfigROM, "ConfigROMStore::ValidateROM: Validated GUID 0x%016llx at node %u gen %u",
             guid, nodeId, gen.value);
}

void ConfigROMStore::InvalidateROM(Guid64 guid) {
    LockGuard guard(lock_);

    auto it = romsByGuid_.find(guid);
    if (it == romsByGuid_.end()) {
        return;
    }

    it->second.state = ROMState::Invalid;
    it->second.nodeId = kInvalidNodeId;

    size_t erasedNodeEntries = 0;
    for (auto nodeIt = romsByGenNode_.begin(); nodeIt != romsByGenNode_.end();) {
        if (nodeIt->second.bib.guid == guid) {
            nodeIt = romsByGenNode_.erase(nodeIt);
            ++erasedNodeEntries;
            continue;
        }
        ++nodeIt;
    }

    ASFW_LOG(ConfigROM,
             "ConfigROMStore::InvalidateROM: Invalidated GUID 0x%016llx and removed %zu "
             "generation/node entries",
             guid, erasedNodeEntries);
}

void ConfigROMStore::PruneInvalid() {
    LockGuard guard(lock_);

    std::vector<Guid64> guidsToRemove;
    for (const auto& [guid, rom] : romsByGuid_) {
        if (rom.state == ROMState::Invalid) {
            guidsToRemove.push_back(guid);
        }
    }

    for (Guid64 guid : guidsToRemove) {
        romsByGuid_.erase(guid);
        ASFW_LOG(ConfigROM, "ConfigROMStore::PruneInvalid: Pruned GUID 0x%016llx from romsByGuid_",
                 guid);
    }

    std::vector<GenNodeKey> keysToRemove;
    for (const auto& [key, rom] : romsByGenNode_) {
        if (rom.state == ROMState::Invalid) {
            keysToRemove.push_back(key);
        }
    }

    for (GenNodeKey key : keysToRemove) {
        romsByGenNode_.erase(key);
    }

    ASFW_LOG(ConfigROM, "ConfigROMStore::PruneInvalid: Pruned %zu invalid ROMs",
             guidsToRemove.size());
}

ConfigROMStore::GenNodeKey ConfigROMStore::MakeKey(Generation gen, uint8_t nodeId) {
    return (gen.value << 8) | static_cast<uint32_t>(nodeId);
}

} // namespace ASFW::Discovery
