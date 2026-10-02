// SPDX-License-Identifier: Apache-2.0
#include "DiscoveryCoordinator.hpp"
#include "AvcAudioConfig.hpp"
#include "AvcExtensionInventory.hpp"
#include "../../../DeviceProfiles/Audio/ResolvedDevicePolicy.hpp"
#include "../../DriverKit/Config/AudioProfileRegistry.hpp"
#include "../../../Logging/Logging.hpp"
#include <algorithm>
#include <utility>
namespace ASFW::Audio::AVC {
namespace P = Protocols::AVC;
namespace B = Protocols::BeBoB::Bootloader;
namespace {
std::optional<DeviceProfiles::Audio::StaticAudioEndpointPlan> Policy(Discovery::DeviceRegistry& registry, uint64_t guid) {
    const auto record = registry.SnapshotByGuid(guid);
    const auto* policy = record ? DeviceProfiles::Audio::CurrentAudioPolicy(*record) : nullptr;
    return policy && registry.IsCurrent(policy->route) ? std::optional{policy->plan} : std::nullopt;
}
P::AvcProbeDecision Decision(Discovery::DeviceRegistry& registry, const Discovery::FWUnit& unit) {
    const auto device = unit.GetDevice();
    const auto plan = device ? Policy(registry, device->GetGUID()) : std::nullopt;
    return P::DecideAvcProbe(unit.GetUnitSpecID(), plan ? &*plan : nullptr);
}
}
DiscoveryCoordinator::DiscoveryCoordinator(Discovery::DeviceRegistry& registry, Protocols::Ports::FireWireBusOps& bus,
                                         Protocols::Ports::FireWireBusInfo& info, IAVCAudioConfigListener* listener)
    : registry_(registry), busInfo_(info), preparation_(bus, registry), listener_(listener) {}
void DiscoveryCoordinator::Fail(uint64_t guid, std::string reason) {
    ASFW_LOG_ERROR(Audio, "[AvcPublish] guid=%llx failed reason=%{public}s", guid, reason.c_str());
    publication_[guid] = Failed{std::move(reason)};
}
PublicationState DiscoveryCoordinator::Status(uint64_t guid) const {
    const auto it = publication_.find(guid);
    return it == publication_.end() ? PublicationState{WaitingForDiscovery{}} : it->second;
}
bool DiscoveryCoordinator::AllowsDiscovery(const Discovery::FWUnit& unit) const {
    return !stopped_ && Decision(registry_, unit) == P::AvcProbeDecision::GenericDiscovery;
}
P::AVCUnit::DiscoveryOptions DiscoveryCoordinator::OptionsFor(const Discovery::FWUnit& unit) const {
    const auto device = unit.GetDevice(); const auto plan = device ? Policy(registry_, device->GetGUID()) : std::nullopt;
    return P::DiscoveryOptionsFor(plan ? P::ExtensionInventoryFor(*plan) : P::AvcExtensionInventory::kNone);
}
void DiscoveryCoordinator::PrepareProducer(std::shared_ptr<Discovery::FWUnit> unit, std::function<void(bool)> ready) {
    if (stopped_ || !unit) { ready(false); return; }
    const auto decision = Decision(registry_, *unit);
    if (decision != P::AvcProbeDecision::GenericDiscovery && decision != P::AvcProbeDecision::ProfileOwned &&
        decision != P::AvcProbeDecision::FireworksEfc) { ready(false); return; }
    PrepareDevice(unit->GetDevice(), std::move(ready));
}
void DiscoveryCoordinator::DeviceAdded(std::shared_ptr<Discovery::FWDevice> device) {
    PrepareDevice(std::move(device), [](bool) {});
}
void DiscoveryCoordinator::PrepareDevice(std::shared_ptr<Discovery::FWDevice> device, std::function<void(bool)> ready) {
    if (stopped_ || !device) { ready(false); return; }
    const auto plan = Policy(registry_, device->GetGUID()); const auto route = registry_.CurrentRoute(device->GetGUID());
    if (!plan || !route || route->generation != device->GetGeneration() || route->nodeId != device->GetNodeID()) { ready(false); return; }
    if (!B::ShouldPrepareBootloader(*plan, device->GetVendorID(), device->GetModelID())) { ready(true); return; }
    const auto it = preparations_.find(route->guid);
    if (it == preparations_.end() || it->second.route.deviceIncarnation != route->deviceIncarnation) {
        std::vector<std::function<void(bool)>> stale;
        if (it != preparations_.end()) { stale = std::move(it->second.waiters); preparations_.erase(it); }
        for (auto& waiter : stale) waiter(false);
        StartPreparation(*route, device->GetVendorID(), device->GetModelID(), {std::move(ready)});
        return;
    }
    auto& preparation = it->second;
    switch (preparation.state) {
    case Preparation::State::Ready: ready(true); return; // Firmware confirmed for this incarnation.
    case Preparation::State::AwaitingReenumeration:
        // The device restarts after the cue and usually comes back as the same
        // incarnation on a new route. Read again (the cue is never resent) to
        // confirm the firmware is running before any producer exists.
        if (preparation.route != *route) {
            StartPreparation(*route, preparation.vendorId, preparation.modelId, {std::move(ready)});
            return;
        }
        ready(false); return;
    case Preparation::State::Failed: ready(false); return;
    case Preparation::State::Running:
        if (preparation.route != *route) preparation.newerRoute = *route;
        preparation.waiters.push_back(std::move(ready));
        return;
    }
    std::unreachable(); // Exhaustive over our own state enum; no device value reaches it.
}
void DiscoveryCoordinator::StartPreparation(const Discovery::DeviceRouteToken& route, uint32_t vendorId, uint32_t modelId,
                                            std::vector<std::function<void(bool)>> waiters) {
    const auto plan = Policy(registry_, route.guid);
    if (stopped_ || !plan || !registry_.IsCurrent(route)) { for (auto& waiter : waiters) waiter(false); return; }
    preparations_.insert_or_assign(route.guid, Preparation{route, vendorId, modelId, Preparation::State::Running, {}, std::move(waiters)});
    const auto weak = weak_from_this();
    const bool started = preparation_.Prepare(*plan, vendorId, modelId, route,
        busInfo_.GetSpeed(FW::NodeId{static_cast<uint8_t>(route.nodeId)}),
        [weak] { const auto self = weak.lock(); return self && !self->stopped_; },
        [weak, route](B::PreparationState state) {
            if (const auto self = weak.lock(); self && !self->stopped_) self->OnPrepared(route, state);
        });
    if (!started) OnPrepared(route, B::Retired{B::RetireReason::InfoUnavailable});
}
void DiscoveryCoordinator::OnPrepared(const Discovery::DeviceRouteToken& route, const B::PreparationState& state) {
    const auto it = preparations_.find(route.guid);
    if (it == preparations_.end() || it->second.route != route || it->second.state != Preparation::State::Running) return;
    auto& preparation = it->second;
    auto waiters = std::move(preparation.waiters);
    preparation.waiters.clear();
    const auto finish = [&waiters](bool ready) { for (auto& waiter : waiters) waiter(ready); };
    if (std::holds_alternative<B::AwaitingReenumeration>(state)) {
        // The normal cold-boot path: the loader starts the firmware and the
        // device re-enumerates as a new incarnation. Not a failure.
        preparation.state = Preparation::State::AwaitingReenumeration;
        publication_[route.guid] = WaitingForDiscovery{};
        ASFW_LOG(Audio, "[AvcPublish] guid=%llx waiting for firmware restart after bootloader cue", route.guid);
        finish(false); return;
    }
    const auto* retired = std::get_if<B::Retired>(&state);
    if (!retired) { // A run reports only terminal states; treat anything else as a failed preparation.
        preparation.state = Preparation::State::Failed; Fail(route.guid, "preparation-non-terminal"); finish(false); return;
    }
    switch (retired->reason) {
    case B::RetireReason::FirmwareAlreadyRunning:
        preparation.state = Preparation::State::Ready; finish(true); return;
    case B::RetireReason::GenerationChanged: {
        // Nothing reached the loader. Retry on the newest route if it is still
        // current; otherwise the next generation's attach starts afresh.
        const auto newer = preparation.newerRoute;
        const auto vendorId = preparation.vendorId, modelId = preparation.modelId;
        preparations_.erase(it);
        if (newer && registry_.IsCurrent(*newer)) { StartPreparation(*newer, vendorId, modelId, std::move(waiters)); return; }
        finish(false); return;
    }
    case B::RetireReason::UnsupportedBuild:
    case B::RetireReason::InfoUnavailable:
    case B::RetireReason::CueWriteFailed:
    case B::RetireReason::LoaderStillActiveAfterCue:
        preparation.state = Preparation::State::Failed;
        Fail(route.guid, std::string("preparation-") + B::RetireReasonName(retired->reason));
        finish(false); return;
    }
    std::unreachable(); // Exhaustive over the preparation FSM's own reasons.
}
void DiscoveryCoordinator::UnitCreated(const std::shared_ptr<P::AVCUnit>& unit) {
    if (stopped_ || !unit) return;
    const auto fwUnit = unit->GetFWUnit(); const auto device = unit->GetDevice(); if (!fwUnit || !device) return;
    if (AllowsDiscovery(*fwUnit)) publication_[unit->Guid()] = WaitingForDiscovery{};
    else PublishProfile(*device);
}
void DiscoveryCoordinator::UnitCompleted(const std::shared_ptr<P::AVCUnit>& unit, bool success) {
    if (stopped_ || !unit) return;
    const auto guid = unit->Guid();
    if (!success) {
        // A failed refresh does not withdraw a device that is already published.
        if (std::holds_alternative<Ready>(Status(guid))) {
            ASFW_LOG_WARNING(Audio, "[AvcPublish] guid=%llx refresh failed; keeping the published device", guid);
            return;
        }
        Fail(guid, "discovery-failed"); return;
    }
    const auto device = unit->GetDevice(); const auto plan = device ? Policy(registry_, guid) : std::nullopt;
    const auto snapshot = unit->GetDiscoverySnapshot(); const auto graph = unit->GetDiscoveredGraph();
    if (!device || !plan || !snapshot || !registry_.IsCurrent(snapshot->route)) { Fail(guid, "stale-discovery-route"); return; }
    if (!graph) { Fail(guid, "missing-terminal-graph"); return; }
    const auto* profile = Isoch::Audio::AudioProfileRegistry::ProfileForBuilderId(static_cast<uint32_t>(plan->profileBuilder));
    const auto config = P::BuildGraphAudioConfig({guid, device->GetVendorID(), device->GetModelID(), std::string(device->GetModelName())},
        *plan, *graph, profile ? profile->SupportedSampleRates() : std::vector<uint32_t>{});
    if (!config) { Fail(guid, "unusable-terminal-graph-geometry"); return; }
    Publish(guid, *config);
}
void DiscoveryCoordinator::PublishProfile(const Discovery::FWDevice& device) {
    const auto guid = device.GetGUID(); const auto plan = Policy(registry_, guid);
    const auto* profile = plan ? Isoch::Audio::AudioProfileRegistry::ProfileForBuilderId(static_cast<uint32_t>(plan->profileBuilder)) : nullptr;
    if (!plan || !profile) { Fail(guid, "missing-supported-profile"); return; }
    const auto config = P::BuildProfileOwnedAudioConfig({guid, device.GetVendorID(), device.GetModelID(), std::string(device.GetModelName())}, *plan, *profile);
    if (!config) { Fail(guid, "unusable-profile-geometry"); return; }
    Publish(guid, *config);
}
void DiscoveryCoordinator::Publish(uint64_t guid, const Model::ASFWAudioDevice& config) {
    if (stopped_ || !Policy(registry_, guid)) { Fail(guid, "publication-route-invalid"); return; }
    if (!listener_) { Fail(guid, "missing-audio-consumer"); return; }
    listener_->OnAVCAudioConfigurationReady(guid, config);
    publication_[guid] = Ready{};
    ASFW_LOG(Audio, "[AvcPublish] guid=%llx ready rate=%u in=%u out=%u", guid,
        config.currentSampleRate, config.inputChannelCount, config.outputChannelCount);
}
void DiscoveryCoordinator::Shutdown() {
    stopped_ = true;
    std::vector<std::function<void(bool)>> waiters;
    for (auto& [guid, preparation] : preparations_) {
        (void)guid; for (auto& waiter : preparation.waiters) waiters.push_back(std::move(waiter));
        preparation.waiters.clear(); preparation.state = Preparation::State::Failed;
    }
    for (auto& waiter : waiters) waiter(false);
}
} // namespace ASFW::Audio::AVC
