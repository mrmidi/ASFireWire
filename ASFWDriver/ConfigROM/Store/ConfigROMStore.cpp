#include "../ConfigROMStore.hpp"

#include "../../Logging/Logging.hpp"

#include <DriverKit/IOLib.h>

namespace ASFW::Discovery {

namespace {

class LockGuard final {
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

[[nodiscard]] bool HasParsedUnitProfile(const ConfigROM& rom) noexcept {
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

void ConfigROMStore::Insert(const ConfigROM& rom) {
    LockGuard guard(lock_);
    const auto nodeId = TryOperationalNodeId(rom.nodeId);
    if (!nodeId.has_value()) {
        ASFW_LOG(ConfigROM,
                 "ConfigROMStore::Insert: invalid node=%u gen=%u",
                 rom.nodeId, rom.gen.value);
        return;
    }

    ConfigROM candidate = rom;
    if (candidate.firstSeen == Generation{0}) {
        candidate.firstSeen = rom.gen;
    }
    if (candidate.lastValidated == Generation{0}) {
        candidate.lastValidated = rom.gen;
    }

    const auto key = MakeKey(rom.gen, *nodeId);
    const auto existing = romsByGenNode_.find(key);
    const bool replace =
        existing == romsByGenNode_.end() ||
        (existing->second.rawQuadlets.size() <= candidate.rawQuadlets.size() &&
         (HasParsedUnitProfile(candidate) || !HasParsedUnitProfile(existing->second)));
    if (replace) {
        romsByGenNode_[key] = std::move(candidate);
    }
}

std::optional<ConfigROM> ConfigROMStore::FindByNode(Generation gen, uint8_t nodeId) const {
    return FindByNode(gen, nodeId, true);
}

std::optional<ConfigROM> ConfigROMStore::FindByNode(Generation gen, uint8_t nodeId,
                                                    bool allowSuspended) const {
    LockGuard guard(lock_);
    const auto it = romsByGenNode_.find(MakeKey(gen, nodeId));
    if (it == romsByGenNode_.end() ||
        (!allowSuspended && it->second.state == ROMState::Suspended)) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<ConfigROM> ConfigROMStore::FindLatestForNode(uint8_t nodeId) const {
    LockGuard guard(lock_);
    const ConfigROM* latest = nullptr;
    for (const auto& [key, rom] : romsByGenNode_) {
        if (rom.nodeId != nodeId) {
            continue;
        }
        if (latest == nullptr || rom.gen.value > latest->gen.value) {
            latest = &rom;
        }
    }
    return latest != nullptr ? std::optional<ConfigROM>{*latest} : std::nullopt;
}

std::vector<ConfigROM>
ConfigROMStore::FindByObservedGuid(Guid64 observedGuid) const {
    LockGuard guard(lock_);
    std::vector<ConfigROM> matches;
    for (const auto& [key, rom] : romsByGenNode_) {
        if (rom.bib.guid == observedGuid) {
            matches.push_back(rom);
        }
    }
    return matches;
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

std::vector<ConfigROM> ConfigROMStore::SnapshotByState(Generation gen,
                                                       ROMState state) const {
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
}

void ConfigROMStore::SuspendAll(Generation newGen) {
    LockGuard guard(lock_);
    size_t suspendedCount = 0;
    for (auto& [key, rom] : romsByGenNode_) {
        if (rom.state == ROMState::Fresh || rom.state == ROMState::Validated) {
            rom.state = ROMState::Suspended;
            ++suspendedCount;
        }
    }
    ASFW_LOG(ConfigROM,
             "ConfigROMStore::SuspendAll: suspended %zu routes for generation %u",
             suspendedCount, newGen.value);
}

void ConfigROMStore::InvalidateNode(Generation gen, uint8_t nodeId) {
    LockGuard guard(lock_);
    const auto it = romsByGenNode_.find(MakeKey(gen, nodeId));
    if (it != romsByGenNode_.end()) {
        it->second.state = ROMState::Invalid;
    }
}

void ConfigROMStore::PruneInvalid() {
    LockGuard guard(lock_);
    size_t count = 0;
    for (auto it = romsByGenNode_.begin(); it != romsByGenNode_.end();) {
        if (it->second.state == ROMState::Invalid) {
            it = romsByGenNode_.erase(it);
            ++count;
        } else {
            ++it;
        }
    }
    ASFW_LOG(ConfigROM, "ConfigROMStore::PruneInvalid: pruned %zu routes", count);
}

ConfigROMStore::GenNodeKey ConfigROMStore::MakeKey(Generation gen, uint8_t nodeId) {
    return (gen.value << 8U) | static_cast<uint32_t>(nodeId);
}

} // namespace ASFW::Discovery
