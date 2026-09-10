// SPDX-License-Identifier: Apache-2.0

#include "AudioRuntimeRegistry.hpp"

#include "AudioEndpointRuntime.hpp"
#include "../Protocols/IDeviceProtocol.hpp"
#include "../../UserClient/WireFormats/TxLatencySessionWireFormats.hpp"
#include "../../Logging/Logging.hpp"

#include <algorithm>
#include <vector>

namespace ASFW::Audio {

AudioRuntimeRegistry::AudioRuntimeRegistry() noexcept : lock_(IOLockAlloc()) {
    if (!lock_) ASFW_LOG_ERROR(Audio, "AudioRuntimeRegistry: lock allocation failed");
}

AudioRuntimeRegistry::~AudioRuntimeRegistry() noexcept {
    Clear();
    if (lock_) IOLockFree(lock_);
}

std::shared_ptr<IDeviceProtocol>
AudioRuntimeRegistry::FindShared(Devices::AudioEndpointId endpointId) noexcept {
    if (!lock_ || !endpointId) return nullptr;
    IOLockLock(lock_);
    const auto it = endpoints_.find(endpointId);
    auto result = it != endpoints_.end() ? it->second.protocol : nullptr;
    IOLockUnlock(lock_);
    return result;
}

std::shared_ptr<AudioEndpointRuntime>
AudioRuntimeRegistry::FindEndpointRuntime(Devices::AudioEndpointId endpointId) noexcept {
    if (!lock_ || !endpointId) return nullptr;
    IOLockLock(lock_);
    const auto it = endpoints_.find(endpointId);
    auto result = it != endpoints_.end() ? it->second.runtime : nullptr;
    IOLockUnlock(lock_);
    return result;
}

std::shared_ptr<const Devices::ResolvedAudioEndpointProfile>
AudioRuntimeRegistry::FindProfile(Devices::AudioEndpointId endpointId) noexcept {
    if (!lock_ || !endpointId) return nullptr;
    IOLockLock(lock_);
    const auto it = endpoints_.find(endpointId);
    auto result = it != endpoints_.end() ? it->second.profile : nullptr;
    IOLockUnlock(lock_);
    return result;
}

std::shared_ptr<AudioEndpointRuntime> AudioRuntimeRegistry::InsertResolved(
    std::shared_ptr<const Devices::ResolvedAudioEndpointProfile> profile,
    std::shared_ptr<IDeviceProtocol> protocol) noexcept {
    if (!lock_ || !profile || !profile->endpointId || !profile->deviceInstanceId) {
        return nullptr;
    }

    auto runtime = std::make_shared<AudioEndpointRuntime>(*profile);
    Entry replacement{std::move(protocol), runtime, std::move(profile)};
    Entry previous{};
    IOLockLock(lock_);
    if (timerScheduler_) {
        runtime->SetTimerScheduler(timerScheduler_);
    }
    if (auto it = endpoints_.find(replacement.profile->endpointId);
        it != endpoints_.end()) {
        previous = std::move(it->second);
        it->second = std::move(replacement);
    } else {
        endpoints_.emplace(replacement.profile->endpointId, std::move(replacement));
    }
    IOLockUnlock(lock_);
    return runtime;
}

uint32_t AudioRuntimeRegistry::CopyAudioTelemetrySnapshots(
    Runtime::AudioTelemetrySnapshot& out) noexcept {
    out = {};
    out.version = Runtime::kAudioTelemetryWireVersion;
    if (!lock_) return 0;

    std::vector<std::shared_ptr<AudioEndpointRuntime>> runtimes;
    IOLockLock(lock_);
    runtimes.reserve(std::min<size_t>(endpoints_.size(),
                                      Runtime::kAudioTelemetryMaxEndpoints));
    for (const auto& [endpointId, entry] : endpoints_) {
        (void)endpointId;
        if (entry.runtime) runtimes.push_back(entry.runtime);
        if (runtimes.size() == Runtime::kAudioTelemetryMaxEndpoints) break;
    }
    IOLockUnlock(lock_);

    for (const auto& runtime : runtimes) {
        if (runtime->CopyAudioTelemetrySnapshot(out.endpoints[out.endpointCount])) {
            ++out.endpointCount;
        }
    }
    out.byteSize = Runtime::AudioTelemetryWireByteSize(out.endpointCount);
    return out.endpointCount;
}

uint32_t AudioRuntimeRegistry::CopyConfigurationEndpointIds(
    std::array<Devices::AudioEndpointId,
               Configuration::kMaxConfigurationSnapshotCapabilities>& out) noexcept {
    out.fill({});
    if (!lock_) return 0;

    uint32_t count = 0;
    IOLockLock(lock_);
    for (const auto& [endpointId, entry] : endpoints_) {
        if (!entry.runtime || !entry.profile ||
            entry.profile->configurationCapabilityCount == 0) {
            continue;
        }
        out[count++] = endpointId;
        if (count == out.size()) break;
    }
    IOLockUnlock(lock_);
    return count;
}

uint32_t AudioRuntimeRegistry::CopyRuntimeTuningEndpointIds(
    std::array<Devices::AudioEndpointId,
               Shared::kMaxAudioRuntimeTuningEndpoints>& out) noexcept {
    out.fill({});
    if (!lock_) return 0;

    uint32_t count = 0;
    IOLockLock(lock_);
    for (const auto& [endpointId, entry] : endpoints_) {
        // A resolved runtime is the whole predicate. Tuning is served from the
        // published nub, and the nub exists for every endpoint that got this
        // far, whether or not the device also exposes rate or optical controls.
        if (!entry.runtime) continue;
        out[count++] = endpointId;
        if (count == out.size()) break;
    }
    IOLockUnlock(lock_);
    return count;
}

uint32_t AudioRuntimeRegistry::CopySemanticTopologyEndpointIds(
    std::array<Devices::AudioEndpointId,
               kMaxAudioSemanticTopologyEndpoints>& out) noexcept {
    out.fill({});
    if (!lock_) return 0;

    uint32_t count = 0;
    IOLockLock(lock_);
    for (const auto& [endpointId, entry] : endpoints_) {
        if (!entry.runtime || !entry.protocol || !entry.protocol->AsAudioSemanticTopology()) {
            continue;
        }
        out[count++] = endpointId;
        if (count == out.size()) break;
    }
    IOLockUnlock(lock_);
    return count;
}

uint32_t AudioRuntimeRegistry::CopySemanticMatrixEndpointIds(
    std::array<Devices::AudioEndpointId,
               kMaxAudioSemanticMatrixEndpoints>& out) noexcept {
    out.fill({});
    if (!lock_) return 0;

    uint32_t count = 0;
    IOLockLock(lock_);
    for (const auto& [endpointId, entry] : endpoints_) {
        if (!entry.runtime || !entry.protocol || !entry.protocol->AsAudioSemanticMatrix()) {
            continue;
        }
        out[count++] = endpointId;
        if (count == out.size()) break;
    }
    IOLockUnlock(lock_);
    return count;
}

void AudioRuntimeRegistry::SetTimerScheduler(Scheduling::ITimerScheduler* scheduler) noexcept {
    if (!lock_) return;
    IOLockLock(lock_);
    timerScheduler_ = scheduler;
    for (auto& [id, entry] : endpoints_) {
        if (entry.runtime) {
            entry.runtime->SetTimerScheduler(scheduler);
        }
    }
    IOLockUnlock(lock_);
}

bool AudioRuntimeRegistry::StartTxLatencySession(
    Devices::AudioEndpointId endpointId,
    uint32_t durationSeconds,
    uint32_t strataSize,
    uint32_t seed,
    uint32_t assumedDriftPpm,
    uint32_t* outSessionId) noexcept {
    auto runtime = FindEndpointRuntime(endpointId);
    if (!runtime) return false;
    return runtime->StartTxLatencySession(durationSeconds, strataSize, seed, assumedDriftPpm, outSessionId);
}

bool AudioRuntimeRegistry::StopTxLatencySession(
    Devices::AudioEndpointId endpointId,
    uint32_t targetSessionId) noexcept {
    auto runtime = FindEndpointRuntime(endpointId);
    if (!runtime) return false;
    return runtime->StopTxLatencySession(targetSessionId);
}

bool AudioRuntimeRegistry::CopyTxLatencyResults(
    Devices::AudioEndpointId endpointId,
    uint32_t pageIndex,
    uint32_t samplesPerPage,
    uint32_t requestedSessionId,
    UserClient::Wire::TxLatencyResultsPageWire& out) noexcept {
    auto runtime = FindEndpointRuntime(endpointId);
    if (!runtime) return false;
    return runtime->CopyTxLatencyResults(pageIndex, samplesPerPage, requestedSessionId, out);
}

void AudioRuntimeRegistry::Remove(Devices::AudioEndpointId endpointId) noexcept {
    Entry removed{};
    if (!lock_ || !endpointId) return;
    IOLockLock(lock_);
    if (auto it = endpoints_.find(endpointId); it != endpoints_.end()) {
        removed = std::move(it->second);
        endpoints_.erase(it);
    }
    IOLockUnlock(lock_);
}

void AudioRuntimeRegistry::Clear() noexcept {
    std::map<Devices::AudioEndpointId, Entry> drained;
    if (!lock_) return;
    IOLockLock(lock_);
    drained.swap(endpoints_);
    IOLockUnlock(lock_);
}

} // namespace ASFW::Audio
