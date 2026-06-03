// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project

#include "AudioRuntimeRegistry.hpp"

#include "../../Logging/Logging.hpp"
#include "../../Protocols/Audio/IDeviceProtocol.hpp"
#include "../../Discovery/DiscoveryTypes.hpp"

#if !defined(ASFW_HOST_TEST)
#include "../../Protocols/Audio/DeviceProtocolFactory.hpp"
#include "../../Protocols/Ports/FireWireBusPort.hpp"
#endif

namespace ASFW::Audio {

AudioRuntimeRegistry::AudioRuntimeRegistry() noexcept : lock_(IOLockAlloc()) {
    if (!lock_) {
        ASFW_LOG_ERROR(Audio, "AudioRuntimeRegistry: Failed to allocate lock");
    }
}

AudioRuntimeRegistry::~AudioRuntimeRegistry() noexcept {
    Clear();
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

std::shared_ptr<IDeviceProtocol> AudioRuntimeRegistry::FindShared(uint64_t guid) noexcept {
    std::shared_ptr<IDeviceProtocol> result;
    if (lock_) {
        IOLockLock(lock_);
        auto it = protocolsByGuid_.find(guid);
        if (it != protocolsByGuid_.end()) {
            result = it->second; // copy keeps the protocol alive past the lock
        }
        IOLockUnlock(lock_);
    }
    return result;
}

std::shared_ptr<IDeviceProtocol> AudioRuntimeRegistry::EnsureForDevice(
    const Discovery::DeviceRecord& record,
    Async::IFireWireBusOps* busOps,
    Async::IFireWireBusInfo* busInfo,
    IRM::IRMClient* irmClient) noexcept {
    const uint64_t guid = record.guid;

    // Creation is orchestrator-serialized: EnsureForDevice runs only on the single Default
    // queue (the controller discovery path), so there is no concurrent create for the same
    // GUID. The lock below still guards the map against off-queue FindShared/Remove callers.
    // Idempotent: an existing instance short-circuits (e.g. re-scan on resume).
    if (auto existing = FindShared(guid)) {
        return existing;
    }

#if !defined(ASFW_HOST_TEST)
    const auto operationalNodeId = Discovery::TryOperationalNodeId(record.nodeId);
    if (!busOps || !busInfo || !operationalNodeId.has_value()) {
        ASFW_LOG(Audio,
                 "AudioRuntimeRegistry: cannot create protocol for GUID=0x%016llx node=%u - bus "
                 "ports or operational node id unavailable",
                 guid,
                 record.nodeId);
        return nullptr;
    }

    // Create() returns nullptr for everything but a recognized vendor/model, which
    // is exactly the gate the former DeviceRegistry::MaybeCreateKnownProtocol path
    // applied (recognized devices are precisely those with a non-None integration
    // mode). No protocol is created, and nothing is logged, for unknown devices.
    auto created = DeviceProtocolFactory::Create(
        record.vendorId, record.modelId, *busOps, *busInfo, *operationalNodeId, irmClient);
    if (!created) {
        return nullptr;
    }

    ASFW_LOG(Audio,
             "AudioRuntimeRegistry: ✅ protocol created: %{public}s for GUID=0x%016llx node=%u",
             created->GetName(),
             guid,
             record.nodeId);
    created->Initialize();

    std::shared_ptr<IDeviceProtocol> shared = std::move(created);
    if (lock_) {
        IOLockLock(lock_);
        protocolsByGuid_[guid] = shared;
        IOLockUnlock(lock_);
    }
    return shared;
#else
    (void)busOps;
    (void)busInfo;
    (void)irmClient;
    return nullptr;
#endif
}

void AudioRuntimeRegistry::Insert(uint64_t guid,
                                  std::shared_ptr<IDeviceProtocol> protocol) noexcept {
    std::shared_ptr<IDeviceProtocol> previous; // destruct outside the lock
    if (lock_) {
        IOLockLock(lock_);
        auto it = protocolsByGuid_.find(guid);
        if (it != protocolsByGuid_.end()) {
            previous = std::move(it->second);
            it->second = std::move(protocol);
        } else {
            protocolsByGuid_.emplace(guid, std::move(protocol));
        }
        IOLockUnlock(lock_);
    }
}

void AudioRuntimeRegistry::Remove(uint64_t guid) noexcept {
    std::shared_ptr<IDeviceProtocol> removed; // destruct outside the lock
    if (lock_) {
        IOLockLock(lock_);
        auto it = protocolsByGuid_.find(guid);
        if (it != protocolsByGuid_.end()) {
            removed = std::move(it->second);
            protocolsByGuid_.erase(it);
        }
        IOLockUnlock(lock_);
    }
}

void AudioRuntimeRegistry::Clear() noexcept {
    std::unordered_map<uint64_t, std::shared_ptr<IDeviceProtocol>> drained;
    if (lock_) {
        IOLockLock(lock_);
        drained.swap(protocolsByGuid_);
        IOLockUnlock(lock_);
    }
    // drained destructs here, outside the lock, releasing each protocol.
}

} // namespace ASFW::Audio
