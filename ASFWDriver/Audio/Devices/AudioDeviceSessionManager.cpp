// SPDX-License-Identifier: Apache-2.0

#include "AudioDeviceSessionManager.hpp"

#include "../../Discovery/DeviceRegistry.hpp"
#include "../../Discovery/FWDevice.hpp"
#include "../../Logging/Logging.hpp"

#include <type_traits>
#include <utility>
#include <variant>

namespace ASFW::Audio::Devices {

namespace {
class Lock final {
public:
    explicit Lock(IOLock* lock) : lock_(lock) { if (lock_) IOLockLock(lock_); }
    ~Lock() { if (lock_) IOLockUnlock(lock_); }
private:
    IOLock* lock_;
};
} // namespace

AudioDeviceSessionManager::AudioDeviceSessionManager(
    Discovery::IDeviceManager& devices,
    Discovery::DeviceRegistry& routes,
    IAudioSessionSink& sink,
    CatalogResolver catalogResolver,
    Async::IFireWireBusOps* busOps,
    Scheduling::ITimerScheduler* scheduler) noexcept
    : devices_(devices), routes_(routes), sink_(sink), lock_(IOLockAlloc()),
      catalogResolver_(std::move(catalogResolver)), busOps_(busOps),
      scheduler_(scheduler) {
    if (!catalogResolver_) {
        catalogResolver_ = [](const Discovery::DeviceRecord& record,
                              const Discovery::UnitIdentityEvidence& unit) {
            return DeviceProfiles::Audio::AudioDeviceCatalog::Resolve(record, unit);
        };
    }
}

AudioDeviceSessionManager::~AudioDeviceSessionManager() {
    Shutdown();
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

bool AudioDeviceSessionManager::RegisterProvider(
    std::unique_ptr<IAudioFamilyProvider> provider) noexcept {
    if (!provider || provider->Id() == DeviceProfiles::Audio::AudioFamilyProviderId::None) {
        return false;
    }
    Lock guard(lock_);
    if (observing_ || shuttingDown_ || providers_.contains(provider->Id())) {
        return false;
    }
    providers_.emplace(provider->Id(), std::move(provider));
    return true;
}

void AudioDeviceSessionManager::Start() {
    {
        Lock guard(lock_);
        if (observing_ || shuttingDown_) return;
        observing_ = true;
    }
    devices_.RegisterDeviceObserver(this);
    for (const auto& device : devices_.GetReadyDevices()) {
        ReconcileDevice(device);
    }
}

void AudioDeviceSessionManager::Shutdown() noexcept {
    std::vector<AudioEndpointId> endpoints;
    bool unregister = false;
    {
        Lock guard(lock_);
        if (shuttingDown_) return;
        shuttingDown_ = true;
        lifetime_.reset();
        unregister = observing_;
        observing_ = false;
        for (const auto& [endpointId, session] : sessions_) endpoints.push_back(endpointId);
    }
    if (unregister) devices_.UnregisterDeviceObserver(this);
    for (const auto endpointId : endpoints) RetireSession(endpointId, "manager-shutdown");
}

std::optional<AudioDeviceSessionSnapshot>
AudioDeviceSessionManager::Snapshot(AudioEndpointId endpointId) const noexcept {
    Lock guard(lock_);
    const auto* session = FindLocked(endpointId);
    if (!session) return std::nullopt;
    return AudioDeviceSessionSnapshot{session->endpointId, session->unitId, session->state,
                                      session->staticPlan.family, session->quarantineReason,
                                      session->probeEpoch, session->profile};
}

std::vector<AudioDeviceSessionSnapshot> AudioDeviceSessionManager::SnapshotAll() const {
    Lock guard(lock_);
    std::vector<AudioDeviceSessionSnapshot> result;
    result.reserve(sessions_.size());
    for (const auto& [id, session] : sessions_) {
        result.push_back({id, session.unitId, session.state, session.staticPlan.family,
                          session.quarantineReason, session.probeEpoch, session.profile});
    }
    return result;
}

std::optional<AudioEndpointId>
AudioDeviceSessionManager::EndpointForUnit(Discovery::UnitInstanceId unitId) const noexcept {
    Lock guard(lock_);
    const auto it = endpointByUnit_.find(unitId);
    return it != endpointByUnit_.end() ? std::optional{it->second} : std::nullopt;
}

bool AudioDeviceSessionManager::UpdateStreamingState(
    AudioEndpointId endpointId, bool streaming) noexcept {
    Lock guard(lock_);
    if (shuttingDown_) return false;
    auto* session = FindLocked(endpointId);
    if (!session) return false;

    if (streaming) {
        if (session->state == AudioSessionState::Streaming) return true;
        if (session->state != AudioSessionState::Ready) return false;
        TransitionLocked(*session, AudioSessionState::Streaming, "stream-started");
        return true;
    }

    if (session->state == AudioSessionState::Streaming) {
        TransitionLocked(*session, AudioSessionState::Ready, "stream-stopped");
        return true;
    }
    // Quiescing owns the stop path during reset/unplug and must not be moved
    // backwards to Ready by a concurrent transport completion.
    return session->state == AudioSessionState::Ready ||
           session->state == AudioSessionState::Quiescing;
}

std::optional<Discovery::DeviceRouteToken>
AudioDeviceSessionManager::CurrentRoute(Discovery::DeviceInstanceId instanceId) const noexcept {
    return routes_.CurrentRoute(instanceId);
}

bool AudioDeviceSessionManager::IsCurrent(
    const Discovery::DeviceRouteToken& route) const noexcept {
    return routes_.IsCurrent(route);
}

void AudioDeviceSessionManager::OnDeviceAdded(
    std::shared_ptr<Discovery::FWDevice> device) {
    ReconcileDevice(device);
}

void AudioDeviceSessionManager::OnDeviceResumed(
    std::shared_ptr<Discovery::FWDevice> device) {
    ReconcileDevice(device);
}

void AudioDeviceSessionManager::OnDeviceSuspended(
    std::shared_ptr<Discovery::FWDevice> device) {
    if (!device) return;
    std::vector<AudioEndpointId> endpoints;
    std::vector<IAudioDeviceAdapter*> adapters;
    {
        Lock guard(lock_);
        for (auto& [id, session] : sessions_) {
            if (session.unitId.device != device->GetInstanceId()) continue;
            if (session.adapter) adapters.push_back(session.adapter.get());
            ++session.probeEpoch;
            if (session.state == AudioSessionState::Ready ||
                session.state == AudioSessionState::Streaming ||
                session.state == AudioSessionState::Probing) {
                TransitionLocked(session, AudioSessionState::Quiescing, "bus-reset");
                endpoints.push_back(id);
            }
        }
    }
    // Cancellation is allowed to complete synchronously. The epoch and state
    // were invalidated under the lock first, so a re-entrant completion fails
    // closed without deadlocking this manager.
    for (const auto id : endpoints) {
        sink_.QuiesceEndpoint(id);
    }
    for (auto* adapter : adapters) adapter->CancelProbe();
    for (const auto id : endpoints) {
        sink_.InvalidateEndpointBindings(id);
        // The profile property on an audio nub is an immutable snapshot. Drop
        // the old nub on reset so a changed safe-probe result is serialized
        // into a fresh nub before the endpoint becomes Ready again.
        sink_.TerminateEndpoint(id);
    }
}

void AudioDeviceSessionManager::OnDeviceRemoved(
    Discovery::DeviceInstanceId instanceId) {
    std::vector<AudioEndpointId> endpoints;
    {
        Lock guard(lock_);
        for (const auto& [id, session] : sessions_) {
            if (session.unitId.device == instanceId) endpoints.push_back(id);
        }
    }
    for (const auto id : endpoints) RetireSession(id, "device-removed");
}

void AudioDeviceSessionManager::ReconcileDevice(
    const std::shared_ptr<Discovery::FWDevice>& device) {
    if (!device || !device->IsReady() || device->IsQuarantined()) return;
    const auto record = routes_.Snapshot(device->GetInstanceId());
    if (!record || record->state != Discovery::LifeState::Ready) return;

    for (const auto& unitEvidence : record->identity.units) {
        const Discovery::UnitInstanceId unitId{record->instanceId,
                                               unitEvidence.unitDirectoryOffset};
        {
            Lock guard(lock_);
            if (endpointByUnit_.contains(unitId)) {
                auto* existing = FindLocked(endpointByUnit_[unitId]);
                if (existing && existing->state == AudioSessionState::Quiescing) {
                    TransitionLocked(*existing, AudioSessionState::StaticResolved,
                                     "route-resumed");
                } else {
                    continue;
                }
            }
        }

        auto plan = catalogResolver_(*record, unitEvidence);
        if (!plan) {
            if (plan.error() == DeviceProfiles::Audio::CatalogResolutionError::NoMatch) continue;
            Lock guard(lock_);
            const auto endpointId = AllocateEndpointIdLocked();
            Session session{};
            session.endpointId = endpointId;
            session.unitId = unitId;
            session.state = AudioSessionState::Quarantined;
            session.quarantineReason =
                plan.error() == DeviceProfiles::Audio::CatalogResolutionError::AmbiguousIdentity
                    ? Discovery::QuarantineReason::AmbiguousIdentity
                    : Discovery::QuarantineReason::HazardousNoProbe;
            sessions_.emplace(endpointId, std::move(session));
            endpointByUnit_[unitId] = endpointId;
            continue;
        }
        // Firmware preparation is decided before audio support, because the
        // persona that gets cued is deliberately RecognizedUnsupported: it is a
        // bootloader, not an audio endpoint, and the early-out below would
        // otherwise skip it. Nothing here creates an adapter or sends FCP.
        if (plan->bootloaderCue !=
            DeviceProfiles::Audio::BootloaderCuePolicy::None) {
            const auto route = routes_.CurrentRoute(record->instanceId);
            AudioEndpointId endpointId{};
            bool start = false;
            {
                Lock guard(lock_);
                if (endpointByUnit_.contains(unitId)) continue;
                endpointId = AllocateEndpointIdLocked();
                Session session{};
                session.endpointId = endpointId;
                session.unitId = unitId;
                session.staticPlan = *plan;
                session.state = AudioSessionState::Preparing;
                sessions_.emplace(endpointId, std::move(session));
                endpointByUnit_[unitId] = endpointId;
                // Without transport the session is recorded and left in
                // Preparing rather than partially driven. There is no second,
                // degraded firmware path to fall into.
                start = busOps_ != nullptr && scheduler_ != nullptr &&
                        route.has_value();
            }
            if (start) {
                BeginPreparation(endpointId, *route);
            } else {
                ASFW_LOG(Firmware,
                         "[Bootloader] endpoint=%llu preparation not started "
                         "(busOps=%d scheduler=%d route=%d)",
                         endpointId.value, busOps_ != nullptr,
                         scheduler_ != nullptr, route.has_value());
            }
            continue;
        }
        if (plan->support != DeviceProfiles::Audio::SupportDisposition::Supported &&
            plan->support != DeviceProfiles::Audio::SupportDisposition::GenericFallback) {
            continue;
        }

        AudioEndpointId endpointId{};
        {
            Lock guard(lock_);
            if (const auto existing = endpointByUnit_.find(unitId);
                existing != endpointByUnit_.end()) {
                endpointId = existing->second;
            } else {
                endpointId = AllocateEndpointIdLocked();
                Session session{};
                session.endpointId = endpointId;
                session.unitId = unitId;
                session.staticPlan = *plan;
                session.state = AudioSessionState::StaticResolved;
                const auto provider = providers_.find(plan->family);
                if (provider == providers_.end()) {
                    session.state = AudioSessionState::Quarantined;
                    session.quarantineReason = Discovery::QuarantineReason::UnsupportedFamily;
                } else {
                    session.adapter = provider->second->CreateAdapter(
                        AudioFamilyProviderContext{unitId, *plan, *this});
                    if (!session.adapter) {
                        session.state = AudioSessionState::Failed;
                    }
                }
                sessions_.emplace(endpointId, std::move(session));
                endpointByUnit_[unitId] = endpointId;
            }
        }
        BeginProbe(endpointId, *record);
    }
}

void AudioDeviceSessionManager::BeginPreparation(
    AudioEndpointId endpointId, Discovery::DeviceRouteToken route) noexcept {
    namespace Boot = Families::BeBoB::Bootloader;
    uint64_t epoch = 0;
    Boot::PreparationStep step{};
    {
        Lock guard(lock_);
        auto* session = FindLocked(endpointId);
        if (!session || session->state != AudioSessionState::Preparing) return;
        session->bootloaderClient = std::make_shared<Boot::BeBoBBootloaderClient>(
            *busOps_, *scheduler_, route);
        step = Boot::BeginPreparation();
        session->preparationState = step.state;
        // Shared with the probe path deliberately: a generation change bumps it
        // and every in-flight preparation callback is then ignored.
        epoch = ++session->probeEpoch;
    }
    // This path is cold and rare, so every transition is logged rather than
    // anomalies only. See MAUDIO_BOOTLOADER_CUE_DESIGN.md §7.
    ASFW_LOG(Firmware, "[Bootloader] endpoint=%llu preparation begin",
             endpointId.value);
    DrivePreparation(endpointId, epoch, std::move(step));
}

void AudioDeviceSessionManager::DrivePreparation(
    AudioEndpointId endpointId, uint64_t preparationEpoch,
    Families::BeBoB::Bootloader::PreparationStep step) noexcept {
    namespace Boot = Families::BeBoB::Bootloader;

    std::shared_ptr<Boot::BeBoBBootloaderClient> client;
    {
        Lock guard(lock_);
        auto* session = FindLocked(endpointId);
        if (!session || session->probeEpoch != preparationEpoch ||
            session->state != AudioSessionState::Preparing) {
            return;
        }
        session->preparationState = step.state;
        client = session->bootloaderClient;
    }
    if (!client) return;

    if (const auto* retired = std::get_if<Boot::Retired>(&step.state)) {
        ASFW_LOG(Firmware, "[Bootloader] endpoint=%llu retired reason=%{public}s",
                 endpointId.value, Boot::RetireReasonName(retired->reason));
        client->Cancel();
        // Dropped directly rather than through RetireSession: this session never
        // published an endpoint, so there is nothing for the sink to quiesce,
        // invalidate or terminate. A cued device comes back in a later
        // generation under a different identity and is resolved from scratch,
        // which is what makes re-running the machine idempotent (§6).
        Lock guard(lock_);
        if (const auto it = sessions_.find(endpointId); it != sessions_.end()) {
            TransitionLocked(it->second, AudioSessionState::Retired,
                             "bootloader-preparation-complete");
            endpointByUnit_.erase(it->second.unitId);
            sessions_.erase(it);
        }
        return;
    }

    std::weak_ptr<int> alive = lifetime_;
    auto onEvent = [this, alive, endpointId, preparationEpoch](
                       Boot::PreparationEvent event) mutable {
        if (alive.expired()) return;
        Boot::PreparationState current{};
        {
            Lock guard(lock_);
            const auto* session = FindLocked(endpointId);
            if (!session || session->probeEpoch != preparationEpoch) return;
            current = session->preparationState;
        }
        DrivePreparation(endpointId, preparationEpoch,
                         Boot::AdvancePreparation(current, event));
    };

    std::visit(
        [&](const auto& action) {
            using Action = std::decay_t<decltype(action)>;
            if constexpr (std::is_same_v<Action, Boot::ReadInfoBlock>) {
                ASFW_LOG(Firmware, "[Bootloader] endpoint=%llu action=read-info",
                         endpointId.value);
                client->ReadInfo(std::move(onEvent));
            } else if constexpr (std::is_same_v<Action, Boot::WriteCue>) {
                // The only cue write in the driver. IsPermittedBootloaderWrite
                // gates the bytes and the address inside the client regardless
                // of what reaches here.
                ASFW_LOG(Firmware,
                         "[Bootloader] endpoint=%llu action=send-cue "
                         "protocolVersion=%u",
                         endpointId.value, action.cue.ProtocolVersion());
                client->SendCue(action.cue, std::move(onEvent));
            } else if constexpr (std::is_same_v<Action, Boot::ReadResponse>) {
                ASFW_LOG(Firmware,
                         "[Bootloader] endpoint=%llu action=read-response "
                         "delayMs=%u",
                         endpointId.value, action.delayMs);
                client->ReadResponse(action.delayMs, std::move(onEvent));
            }
            // Done carries no transport work; the Retired branch above owns it.
        },
        step.action);
}

void AudioDeviceSessionManager::BeginProbe(AudioEndpointId endpointId,
                                            const Discovery::DeviceRecord& record) noexcept {
    IAudioDeviceAdapter* adapter = nullptr;
    uint64_t epoch = 0;
    {
        Lock guard(lock_);
        auto* session = FindLocked(endpointId);
        if (!session || !session->adapter ||
            (session->state != AudioSessionState::StaticResolved &&
             session->state != AudioSessionState::Quiescing)) return;
        epoch = ++session->probeEpoch;
        TransitionLocked(*session, AudioSessionState::Probing, "probe-start");
        adapter = session->adapter.get();
    }

    std::weak_ptr<int> alive = lifetime_;
    const bool accepted = adapter->Probe(
        [this, alive, endpointId, epoch, record](auto result) mutable {
            if (alive.expired()) return;
            CompleteProbe(endpointId, epoch, std::move(record), std::move(result));
        });
    if (!accepted) {
        Lock guard(lock_);
        if (auto* session = FindLocked(endpointId);
            session && session->probeEpoch == epoch) {
            TransitionLocked(*session, AudioSessionState::Failed, "probe-rejected");
        }
    }
}

void AudioDeviceSessionManager::CompleteProbe(
    AudioEndpointId endpointId, uint64_t probeEpoch,
    Discovery::DeviceRecord record,
    std::expected<FamilyProbeFacts, ProbeError> result) noexcept {
    std::shared_ptr<const ResolvedAudioEndpointProfile> profile;
    std::shared_ptr<IDeviceProtocol> protocol;
    {
        Lock guard(lock_);
        auto* session = FindLocked(endpointId);
        if (!session || session->probeEpoch != probeEpoch ||
            session->state != AudioSessionState::Probing) return;
        if (!result) {
            TransitionLocked(*session,
                result.error() == ProbeError::Cancelled
                    ? AudioSessionState::StaticResolved
                    : AudioSessionState::Failed,
                "probe-complete-error");
            return;
        }
        std::vector<FacetDescriptor> adapterFacets;
        for (const auto& facet : session->adapter->Facets()) {
            if (facet) adapterFacets.push_back(facet->Descriptor());
        }
        auto narrowed = ResolvedProfileBuilder::ApplyProbeEvidence(
            session->staticPlan, *result);
        if (!narrowed) {
            session->quarantineReason = Discovery::QuarantineReason::InsufficientSafeEvidence;
            TransitionLocked(*session, AudioSessionState::Quarantined,
                             "probe-evidence-insufficient");
            return;
        }
        session->staticPlan = std::move(*narrowed);
        auto built = ResolvedProfileBuilder::Build(
            ProfileBuildContext{endpointId, record, session->staticPlan, *result,
                                adapterFacets});
        if (!built) {
            session->quarantineReason = Discovery::QuarantineReason::InsufficientSafeEvidence;
            TransitionLocked(*session, AudioSessionState::Quarantined,
                             "profile-build-failed");
            return;
        }
        profile = std::make_shared<const ResolvedAudioEndpointProfile>(std::move(*built));
        session->profile = profile;
        protocol = session->adapter->ProtocolHold();
        TransitionLocked(*session, AudioSessionState::Ready, "probe-complete");
    }
    sink_.EndpointReady(std::move(profile), std::move(protocol));
}

void AudioDeviceSessionManager::RetireSession(AudioEndpointId endpointId,
                                              const char* reason) noexcept {
    std::unique_ptr<IAudioDeviceAdapter> adapter;
    Discovery::UnitInstanceId unitId{};

    // A preparation session torn down from outside — which is the *expected*
    // ending, because a successful cue makes the device reset and disappear.
    // It never published an endpoint, so the sink has nothing to quiesce, and
    // the machine never produced a RetireReason, so log the state it died in:
    // awaiting-response means the cue was written and the device left before
    // answering, which is what success looks like from here.
    {
        std::shared_ptr<Families::BeBoB::Bootloader::BeBoBBootloaderClient> client;
        {
            Lock guard(lock_);
            auto* session = FindLocked(endpointId);
            if (!session || session->state == AudioSessionState::Retired) return;
            if (session->state == AudioSessionState::Preparing) {
                ASFW_LOG(Firmware,
                         "[Bootloader] endpoint=%llu torn down in state=%{public}s "
                         "reason=%{public}s",
                         endpointId.value,
                         Families::BeBoB::Bootloader::PreparationStateName(
                             session->preparationState),
                         reason);
                ++session->probeEpoch;
                client = std::move(session->bootloaderClient);
                const auto preparingUnit = session->unitId;
                TransitionLocked(*session, AudioSessionState::Retired, reason);
                sessions_.erase(endpointId);
                endpointByUnit_.erase(preparingUnit);
            }
        }
        if (client) {
            // Outside the lock. Cancels a response-read timer that may still be
            // armed; without this it fires later against a stale generation and
            // keeps the client alive past the session that owned it.
            client->Cancel();
            return;
        }
    }

    {
        Lock guard(lock_);
        auto* session = FindLocked(endpointId);
        if (!session || session->state == AudioSessionState::Retired) return;
        TransitionLocked(*session, AudioSessionState::Quiescing, reason);
        ++session->probeEpoch;
        adapter = std::move(session->adapter);
        unitId = session->unitId;
    }

    sink_.QuiesceEndpoint(endpointId);
    if (adapter) {
        adapter->CancelProbe();
        (void)adapter->Shutdown();
    }
    sink_.InvalidateEndpointBindings(endpointId);
    sink_.TerminateEndpoint(endpointId);

    {
        Lock guard(lock_);
        const auto it = sessions_.find(endpointId);
        if (it != sessions_.end()) {
            TransitionLocked(it->second, AudioSessionState::Retired, reason);
            endpointByUnit_.erase(unitId);
            sessions_.erase(it);
        }
    }
}

void AudioDeviceSessionManager::TransitionLocked(Session& session,
                                                  AudioSessionState next,
                                                  const char* reason) noexcept {
    ASFW_LOG(Audio,
             "[AudioSession] endpoint=%llu device=%llu provider=%u state=%u->%u reason=%{public}s",
             session.endpointId.value, session.unitId.device.value,
             static_cast<unsigned>(session.staticPlan.family),
             static_cast<unsigned>(session.state), static_cast<unsigned>(next),
             reason ? reason : "unspecified");
    session.state = next;
}

AudioEndpointId AudioDeviceSessionManager::AllocateEndpointIdLocked() noexcept {
    return AudioEndpointId{++nextEndpointId_};
}

AudioDeviceSessionManager::Session*
AudioDeviceSessionManager::FindLocked(AudioEndpointId endpointId) noexcept {
    const auto it = sessions_.find(endpointId);
    return it != sessions_.end() ? &it->second : nullptr;
}

const AudioDeviceSessionManager::Session*
AudioDeviceSessionManager::FindLocked(AudioEndpointId endpointId) const noexcept {
    const auto it = sessions_.find(endpointId);
    return it != sessions_.end() ? &it->second : nullptr;
}

} // namespace ASFW::Audio::Devices
