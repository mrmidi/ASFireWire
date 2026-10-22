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

// Copy of the value at `key`, or nullopt. The caller holds the store lock.
template <typename Map>
[[nodiscard]] std::optional<typename Map::mapped_type> CopyAt(const Map& map,
                                                              const typename Map::key_type& key) {
    if (const auto it = map.find(key); it != map.end()) {
        return it->second;
    }
    return std::nullopt;
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
    // richer entry, which is what ExportConfigROM serves. Without this guard the UI visibly regresses from (for example) 31 to 19
    // quadlets without a bus-generation change.
    const bool shouldUpdateNode =
        nodeIt == romsByGenNode_.end() ||
        (nodeIt->second.rawQuadlets.size() <= romCopy.rawQuadlets.size() &&
         (HasParsedUnitProfile(romCopy) || !HasParsedUnitProfile(nodeIt->second)));
    if (!shouldUpdateNode) {
        ASFW_LOG_V2(ConfigROM,
                    "ConfigROMStore::Insert: retaining richer node cache node=%u gen=%u "
                    "candidateQuadlets=%zu cachedQuadlets=%zu",
                    romCopy.nodeId, romCopy.gen.value, romCopy.rawQuadlets.size(),
                    nodeIt->second.rawQuadlets.size());
        return;
    }

    romsByGenNode_[key] = romCopy;
    ASFW_LOG_V2(ConfigROM,
                "ConfigROMStore::Insert: GUID=0x%016llx gen=%u node=%u rawQuadlets=%zu "
                "rootEntries=%zu unitDirs=%zu hasSBP2=%d",
                romCopy.bib.guid, romCopy.gen.value, romCopy.nodeId, romCopy.rawQuadlets.size(),
                romCopy.rootDirMinimal.size(), romCopy.unitDirectories.size(),
                HasSBP2Unit(romCopy) ? 1 : 0);
}

std::optional<ConfigROM> ConfigROMStore::FindByNode(Generation gen, uint8_t nodeId,
                                                    bool allowSuspended) const {
    LockGuard guard(lock_);

    auto rom = CopyAt(romsByGenNode_, MakeKey(gen, nodeId));
    if (rom && !allowSuspended && rom->state == ROMState::Suspended) {
        return std::nullopt;
    }
    return rom;
}

void ConfigROMStore::SuspendAll(Generation newGen) {
    LockGuard guard(lock_);

    using enum ROMState;

    // Entries suspended by the previous reset are now two generations old and
    // nothing reads them: selector 14 refuses suspended entries and discovery
    // re-inserts current ROMs. Hardware generations are 8 bits and wrap after
    // 255 (OHCI 1.2 Table 11-2), so keeping them lets a (generation, node) key
    // come back carrying an earlier device's ROM, and the cache grows by one
    // entry per node per reset. Keep only the generation suspended now. Apple
    // keeps one cache per device and revalidates it in place
    // (IOFireWireDevice.cpp:708,742), so it accumulates no history either.
    const size_t erasedCount = std::erase_if(romsByGenNode_, [](const auto& entry) {
        return entry.second.state == Suspended;
    });

    uint32_t suspendedCount = 0;

    for (auto& [key, rom] : romsByGenNode_) {
        if (rom.state == Fresh) {
            rom.state = Suspended;
            suspendedCount++;
        }
    }

    ASFW_LOG(ConfigROM,
             "ConfigROMStore::SuspendAll: Suspended %u ROMs, dropped %zu older ROMs for "
             "generation %u",
             suspendedCount, erasedCount, newGen.value);
}

void ConfigROMStore::EraseByGuid(Guid64 guid) {
    LockGuard guard(lock_);

    const size_t erasedCount = std::erase_if(romsByGenNode_, [guid](const auto& entry) {
        return entry.second.bib.guid == guid;
    });

    ASFW_LOG(ConfigROM, "ConfigROMStore::EraseByGuid: removed %zu ROMs for GUID 0x%016llx",
             erasedCount, guid);
}

ConfigROMStore::GenNodeKey ConfigROMStore::MakeKey(Generation gen, uint8_t nodeId) {
    return (gen.value << 8) | static_cast<uint32_t>(nodeId);
}

} // namespace ASFW::Discovery
