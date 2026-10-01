//
// AVCDiscovery.cpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C Discovery implementation
//

#include "AVCDiscovery.hpp"
#include "../../Discovery/DeviceRegistry.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Audio/Model/ASFWAudioDevice.hpp"
#include "../../DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "../../Audio/Protocols/Oxford/OxfwStreamFormats.hpp"
#include "../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../../DeviceProfiles/Audio/ResolvedDevicePolicy.hpp"
#include "../../Audio/Protocols/SelectProbeBootstrap.hpp"
#include "AvcProbeAdmission.hpp"
#include "AvcAudioConfig.hpp"
#include "AvcExtensionInventory.hpp"
#include "../../Discovery/DiscoveryTypes.hpp"
#include "Music/MusicSubunit.hpp"
#include "Graph/AvcDeviceGraph.hpp"
#include "../../Audio/Protocols/BeBoB/BeBoBPlug0StreamDiscovery.hpp"
#include "../../Audio/DriverKit/Config/AudioProfileRegistry.hpp"
#include <DriverKit/IOService.h>
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/OSString.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSArray.h>
#include <DriverKit/OSDictionary.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>

using namespace ASFW::Protocols::AVC;

namespace {

namespace Bootloader = ASFW::Protocols::BeBoB::Bootloader;

// The device catalog's answer, carried to the nub so the audio side does not
// repeat the match from (vendorId, modelId) -- a pair that cannot identify
// every family.
[[nodiscard]] std::optional<ASFW::DeviceProfiles::Audio::StaticAudioEndpointPlan>
CurrentPolicyPlan(ASFW::Discovery::DeviceRegistry& registry, uint64_t guid) {
    const auto snapshot = registry.SnapshotByGuid(guid);
    if (!snapshot.has_value()) {
        return std::nullopt;
    }
    const auto* policy = ASFW::DeviceProfiles::Audio::CurrentAudioPolicy(*snapshot);
    if (policy == nullptr || !registry.IsCurrent(policy->route)) {
        return std::nullopt;
    }
    return policy->plan;
}

[[nodiscard]] std::optional<ASFW::DeviceProfiles::Audio::StaticAudioEndpointPlan>
CurrentPolicyPlan(ASFW::Discovery::DeviceRegistry& registry,
                  const ASFW::Discovery::FWDevice& device) {
    const auto snapshot = registry.SnapshotByGuid(device.GetGUID());
    if (!snapshot.has_value()) {
        return std::nullopt;
    }
    const auto* policy = ASFW::DeviceProfiles::Audio::CurrentAudioPolicy(*snapshot);
    if (policy == nullptr || policy->route.generation != device.GetGeneration() ||
        policy->route.nodeId != device.GetNodeID() ||
        !registry.IsCurrent(policy->route)) {
        return std::nullopt;
    }
    return policy->plan;
}

[[nodiscard]] bool IsCurrentDeviceRoute(ASFW::Discovery::DeviceRegistry& registry,
                                       const ASFW::Discovery::FWDevice& device) noexcept {
    const auto route = registry.CurrentRoute(device.GetGUID());
    return route.has_value() && route->generation == device.GetGeneration() &&
           route->nodeId == device.GetNodeID();
}

// Which bring-up a unit gets is a *policy* decision the catalog already
// records, not a model identity. DecideAvcProbe keeps discovery out of the
// matching business: a new device that needs an existing bootstrap is a
// catalog row, and a device whose family/policy pair has no bootstrap is
// refused rather than silently taking the generic path.
[[nodiscard]] ASFW::Protocols::AVC::AvcProbeDecision ProbeDecisionFor(
    uint32_t specifierId,
    const std::optional<ASFW::DeviceProfiles::Audio::StaticAudioEndpointPlan>& plan) noexcept {
    return ASFW::Protocols::AVC::DecideAvcProbe(specifierId, plan ? &*plan : nullptr);
}

} // namespace

//==============================================================================
// Constants
//==============================================================================


//==============================================================================
// Constructor / Destructor
//==============================================================================

AVCDiscovery::AVCDiscovery(IOService* driver,
                           Discovery::DeviceRegistry& deviceRegistry,
                           Discovery::IDeviceManager& deviceManager,
                           Protocols::Ports::FireWireBusOps& busOps,
                           Protocols::Ports::FireWireBusInfo& busInfo,
                           Scheduling::ITimerScheduler& timerScheduler,
                           ASFW::Audio::IAVCAudioConfigListener* audioConfigListener)
    : driver_(driver)
    , deviceRegistry_(deviceRegistry)
    , deviceManager_(deviceManager)
    , busOps_(busOps)
    , bootloaderPreparation_(busOps, deviceRegistry)
    , busInfo_(busInfo)
    , timerScheduler_(timerScheduler)
    , audioConfigListener_(audioConfigListener) {

    // Allocate lock
    lock_ = IOLockAlloc();
    if (!lock_) {
        os_log_error(log_, "AVCDiscovery: Failed to allocate lock");
    }

    IODispatchQueue* queue = nullptr;
    auto kr = IODispatchQueue::Create("com.asfw.avc.rescan", 0, 0, &queue);
    if (kr == kIOReturnSuccess && queue) {
        rescanQueue_ = OSSharedPtr(queue, OSNoRetain);
    } else if (kr != kIOReturnSuccess) {
        os_log_error(log_, "AVCDiscovery: Failed to create rescan queue (0x%x)", kr);
    }

    // Register as discovery observers
    deviceManager_.RegisterUnitObserver(this);
    deviceManager_.RegisterDeviceObserver(this);

    os_log_info(log_, "AVCDiscovery: Initialized");
}

AVCDiscovery::~AVCDiscovery() {
    Shutdown();

    // Shutdown() unregisters observers before stopping outstanding FCP work.
    // Do not free the lock until that lifecycle boundary has been established.

    // Clean up lock
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }

    os_log_info(log_, "AVCDiscovery: Destroyed");
}

void AVCDiscovery::Shutdown() {
    bool expected = false;
    if (!shuttingDown_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    // Remove external producers first. Device callbacks may otherwise enqueue
    // a fresh AV/C command after FCP has been shut down.
    deviceManager_.UnregisterDeviceObserver(this);
    deviceManager_.UnregisterUnitObserver(this);

    std::vector<std::shared_ptr<AVCUnit>> units;
    std::vector<Scheduling::TimerToken> tokensToCancel;
    if (lock_) {
        IOLockLock(lock_);
        units.reserve(units_.size());
        for (const auto& [guid, unit] : units_) {
            (void)guid;
            if (unit) {
                units.push_back(unit);
            }
        }
        for (const auto& [guid, token] : rescanTimersByGuid_) {
            (void)guid;
            if (token != Scheduling::kInvalidTimerToken) {
                tokensToCancel.push_back(token);
            }
        }
        rescanTimersByGuid_.clear();
        activeRescanSerialByGuid_.clear();
        fcpTransportsByNodeID_.clear();
        rescanAttempts_.clear();
        IOLockUnlock(lock_);
    }

    for (auto token : tokensToCancel) {
        timerScheduler_.Cancel(token);
    }

    for (const auto& unit : units) {
        unit->Shutdown();
    }
}

//==============================================================================
// IUnitObserver Interface
//==============================================================================

void AVCDiscovery::OnUnitPublished(std::shared_ptr<Discovery::FWUnit> unit) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    if (!IsAVCUnit(unit)) {
        return;
    }

    uint64_t guid = GetUnitGUID(unit);

    ASFW_LOG(Async,
             "✅ AV/C DETECTED: GUID=%llx, specID=0x%06x - SCANNING...",
             guid, unit->GetUnitSpecID());

    // Get parent device
    auto device = unit->GetDevice();
    if (!device) {
        os_log_error(log_, "AVCDiscovery: Unit has no parent device");
        return;
    }

    // Resolve the registry's immutable policy before constructing a protocol
    // producer. This snapshot is bound to the current route and fails closed
    // after reset, quarantine, loss, or removal.
    const auto policyPlan = CurrentPolicyPlan(deviceRegistry_, *device);
    if (!policyPlan.has_value()) {
        ASFW_LOG_WARNING(AVC,
                         "AVCDiscovery: refusing probe without current resolved policy GUID=0x%016llx",
                         guid);
        return;
    }

    // Create AVCUnit
    auto avcUnit = std::make_shared<AVCUnit>(device, unit, deviceRegistry_, busOps_, busInfo_,
                                             timerScheduler_, DiscoveryOptionsFor(ExtensionInventoryFor(*policyPlan)));

    // Publish the unit to the shutdown owner before initializing it. A
    // termination callback can race discovery after our first atomic check;
    // if it wins, Shutdown() will see and stop this FCP producer.
    IOLockLock(lock_);
    if (shuttingDown_.load(std::memory_order_acquire)) {
        IOLockUnlock(lock_);
        avcUnit->Shutdown();
        return;
    }
    units_[guid] = avcUnit;
    IOLockUnlock(lock_);

    // One policy decision, resolved once and honoured by every arm below.
    if (!IsCurrentDeviceRoute(deviceRegistry_, *device)) {
        ASFW_LOG_WARNING(AVC,
                         "AVCDiscovery: refusing probe without current resolved policy GUID=0x%016llx",
                         guid);
        avcUnit->Shutdown();
        IOLockLock(lock_);
        const auto it = units_.find(guid);
        if (it != units_.end() && it->second == avcUnit) {
            units_.erase(it);
        }
        IOLockUnlock(lock_);
        RebuildNodeIDMap();
        return;
    }
    const AvcProbeDecision decision = ProbeDecisionFor(unit->GetUnitSpecID(), policyPlan);

    if (decision == AvcProbeDecision::ProfileOwned) {
        // The M-Audio special firmware freezes on the generic information and
        // BridgeCo probes. Its catalog profile's fixed formation is enough to
        // publish an endpoint; device commands are issued only by the selected
        // protocol during start, through the per-frame FCP gate.
        PublishProfileOwnedConfig(guid, *device);
        RebuildNodeIDMap();
        return;
    }

    // Echo Fireworks units (Onyx 400F) advertise an AV/C unit directory but are
    // driven by EFC, not by the Music subunit: Linux snd-fireworks never issues
    // UNIT_INFO/SUBUNIT_INFO or descriptor reads, and Apple's old class driver
    // is the only stack that ever spoke AV/C to them. Skip generic discovery
    // and publish the profile-owned geometry; the runtime protocol verifies
    // that geometry against HWINFO before streaming.
    if (decision == AvcProbeDecision::FireworksEfc) {
        ASFW_LOG(AVC,
                 "AVCDiscovery: Fireworks device matched; bypassing generic AV/C discovery GUID=0x%016llx",
                 guid);
        PublishProfileOwnedConfig(guid, *device);
        RebuildNodeIDMap();
        return;
    }

    // Everything past this point issues generic AV/C: UNIT_INFO, SUBUNIT_INFO,
    // plugs and descriptor reads, then the chip's extension inventory. Only
    // GenericDiscovery asks for that. Falling
    // through with any other bootstrap would speak generic AV/C to a device
    // whose policy forbids it -- which is what wedges M-Audio BeBoB firmware --
    // so each outcome is named here and no default: arm is allowed to swallow
    // a new one.
    switch (decision) {
        case AvcProbeDecision::GenericDiscovery:
            break;

        // Handled above; every arm returns before reaching this switch.
        case AvcProbeDecision::ProfileOwned:
        case AvcProbeDecision::FireworksEfc:
            break;

        case AvcProbeDecision::RegisterDriven:
            // Register-driven families. An AV/C unit directory here is
            // incidental; their bring-up does not go through this path.
            ASFW_LOG(AVC,
                     "AVCDiscovery: register-driven family; skipping generic AV/C discovery "
                     "GUID=0x%016llx",
                     guid);
            RebuildNodeIDMap();
            return;

        case AvcProbeDecision::NotAvcUnit:
        case AvcProbeDecision::NoPolicy:
        case AvcProbeDecision::Refused:
            // No family/policy pair resolved: a hazardous or ambiguous identity,
            // or a device with no units. Unrecognised is the unsafe state for
            // AV/C, so stay off the wire rather than probing generically.
            ASFW_LOG(AVC,
                     "AVCDiscovery: no probe bootstrap for this identity; skipping generic "
                     "AV/C discovery GUID=0x%016llx",
                     guid);
            RebuildNodeIDMap();
            return;
    }

    const std::weak_ptr<AVCDiscovery> weakSelf = weak_from_this();

    // Initialize (probe subunits, plugs)
    avcUnit->Initialize([weakSelf, avcUnit, guid](bool success) {
        const auto self = weakSelf.lock();
        if (!self || self->shuttingDown_.load(std::memory_order_acquire)) {
            return;
        }
        if (!success) {
            // A bus reset mid-attach lands here too; OnUnitResumed runs it again.
            ASFW_LOG_ERROR(AVC, "AVCDiscovery: AVCUnit initialization failed: GUID=%llx", guid);
            return;
        }

        self->HandleInitializedUnit(guid, avcUnit);
    });

    // Rebuild node ID map (unit now has transport)
    RebuildNodeIDMap();
}

void AVCDiscovery::HandleInitializedUnit(uint64_t guid, const std::shared_ptr<AVCUnit>& avcUnit) {
    if (!avcUnit) {
        return;
    }

    auto device = avcUnit->GetDevice();
    if (!device) {
        os_log_error(log_, "AVCDiscovery: AVCUnit missing parent device: GUID=%llx", guid);
        return;
    }

    const auto policyPlan = CurrentPolicyPlan(deviceRegistry_, *device);
    if (!policyPlan.has_value()) {
        ASFW_LOG_WARNING(AVC,
                         "AVCDiscovery: refusing configuration without current resolved policy GUID=0x%016llx",
                         guid);
        return;
    }

    const bool hasAudioSubunit = std::any_of(avcUnit->GetSubunits().begin(), avcUnit->GetSubunits().end(),
        [](const auto& subunit) { return subunit->GetType() == AVCSubunitType::kAudio ||
                                       subunit->GetType() == AVCSubunitType::kMusic; });
    if (!hasAudioSubunit) {
        ASFW_LOG(AVC, "[AvcPublish] guid=%llx skipped reason=no-audio-or-music-subunit", guid);
        return;
    }
    // The graph comes from the music/audio descriptors, or, for a unit without
    // them (OXFW971), from the current formats of unit plug 0.
    const auto graph = avcUnit->GetDiscoveredGraph();
    if (!graph || graph->playback.dataBlockSize == 0 || graph->capture.dataBlockSize == 0) {
        ASFW_LOG_WARNING(Audio, "[AvcPublish] guid=%llx deferred reason=unresolved-stream-graph", guid);
        ScheduleRescan(guid, avcUnit);
        return;
    }

    if (lock_) {
        IOLockLock(lock_);
        rescanAttempts_.erase(guid);
        IOLockUnlock(lock_);
    }

    const AvcEndpointIdentity identity{.guid = guid, .vendorId = device->GetVendorID(),
                                       .modelId = device->GetModelID(),
                                       .modelName = std::string(device->GetModelName())};
    // The runtime profile's rates bound what is offered (Phase 88: 48 kHz only,
    // though its descriptor lists 32-96 kHz). No profile: the graph's rates.
    const auto* profile = ::ASFW::Isoch::Audio::AudioProfileRegistry::ProfileForBuilderId(
        static_cast<uint32_t>(policyPlan->profileBuilder));
    const auto config = BuildGraphAudioConfig(identity, *policyPlan, *graph,
                                              profile ? profile->SupportedSampleRates()
                                                      : std::vector<uint32_t>{});
    if (!config) {
        ASFW_LOG_WARNING(Audio,
                         "[AvcPublish] guid=%llx deferred reason=unusable-graph-geometry rate=%u/%u dbs=%u/%u",
                         guid, graph->playback.currentSampleRate, graph->capture.currentSampleRate,
                         graph->playback.dataBlockSize, graph->capture.dataBlockSize);
        return;
    }
    const uint32_t pin = policyPlan->streamTraits.start.startRatePinHz;
    if (pin != 0 && std::ranges::find(graph->playback.supportedSampleRates, pin) ==
                        graph->playback.supportedSampleRates.end()) {
        ASFW_LOG_WARNING(Audio, "[AvcGraphConfig] guid=%llx pinned rate %u not among discovered rates", guid, pin);
    }
    ASFW_LOG(Audio, "[AvcGraphConfig] guid=%llx in=%u out=%u rates=%zu rate=%u playbackDbs=%u captureDbs=%u",
             guid, config->inputChannelCount, config->outputChannelCount, config->sampleRates.size(),
             config->currentSampleRate, graph->playback.dataBlockSize, graph->capture.dataBlockSize);
    PublishReadyAudioConfig(guid, *config);
}

void AVCDiscovery::PublishProfileOwnedConfig(uint64_t guid, const Discovery::FWDevice& device) {
    const auto plan = CurrentPolicyPlan(deviceRegistry_, device);
    if (!plan || plan->support != ASFW::DeviceProfiles::Audio::SupportDisposition::Supported) {
        ASFW_LOG_WARNING(Audio, "[AvcPublish] guid=%llx refused reason=no-supported-policy", guid);
        return;
    }
    const auto* profile = ::ASFW::Isoch::Audio::AudioProfileRegistry::ProfileForBuilderId(
        static_cast<uint32_t>(plan->profileBuilder));
    if (profile == nullptr) {
        ASFW_LOG_ERROR(Audio, "[AvcPublish] guid=%llx refused reason=no-profile builder=%u", guid,
                       static_cast<unsigned>(plan->profileBuilder));
        return;
    }
    const AvcEndpointIdentity identity{.guid = guid, .vendorId = device.GetVendorID(),
                                       .modelId = device.GetModelID(),
                                       .modelName = std::string(device.GetModelName())};
    const auto config = BuildProfileOwnedAudioConfig(identity, *plan, *profile);
    if (!config) {
        ASFW_LOG_ERROR(Audio, "[AvcPublish] guid=%llx refused reason=unusable-profile-geometry", guid);
        return;
    }
    PublishReadyAudioConfig(guid, *config);
}

void AVCDiscovery::PublishReadyAudioConfig(uint64_t guid, const ::ASFW::Audio::Model::ASFWAudioDevice& config) {
    if (!CurrentPolicyPlan(deviceRegistry_, guid).has_value()) {
        ASFW_LOG_WARNING(Audio,
                         "AVCDiscovery: refusing audio config for stale or unresolved policy GUID=0x%016llx",
                         guid);
        return;
    }
    if (!audioConfigListener_) {
        ASFW_LOG_ERROR(Audio,
                       "AVCDiscovery: no audio config listener; dropping config for GUID=%llx",
                       guid);
        return;
    }

    ASFW_LOG(Audio, "[AvcPublish] guid=%llx source=%{public}s rate=%u in=%u out=%u",
        guid, config.graphResolved ? "graph" : "family", config.currentSampleRate,
        config.inputChannelCount, config.outputChannelCount);
    audioConfigListener_->OnAVCAudioConfigurationReady(guid, config);
}

void AVCDiscovery::ScheduleRescan(uint64_t guid, const std::shared_ptr<AVCUnit>& avcUnit) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    if (!avcUnit) {
        return;
    }
    const auto route = deviceRegistry_.CurrentRoute(guid);
    if (!route.has_value()) {
        return;
    }

    constexpr uint8_t kMaxAutoRescanAttempts = 1;
    constexpr uint32_t kRescanDelayMs = 250;

    uint8_t attempt = 0;
    uint64_t operationSerial = 0;
    Scheduling::TimerToken oldRescanToken = Scheduling::kInvalidTimerToken;
    IOLockLock(lock_);
    auto rescanIt = rescanTimersByGuid_.find(guid);
    if (rescanIt != rescanTimersByGuid_.end()) {
        oldRescanToken = rescanIt->second;
        rescanTimersByGuid_.erase(rescanIt);
    }
    auto& count = rescanAttempts_[guid];
    if (count >= kMaxAutoRescanAttempts) {
        IOLockUnlock(lock_);
        if (oldRescanToken != Scheduling::kInvalidTimerToken) {
            timerScheduler_.Cancel(oldRescanToken);
        }
        ASFW_LOG(Audio,
                 "AVCDiscovery: Auto re-scan limit reached for GUID=%llx (attempts=%u)",
                 guid, count);
        return;
    }
    count++;
    attempt = count;
    operationSerial = ++nextRescanOperationSerial_;
    activeRescanSerialByGuid_[guid] = operationSerial;
    IOLockUnlock(lock_);

    if (oldRescanToken != Scheduling::kInvalidTimerToken) {
        timerScheduler_.Cancel(oldRescanToken);
    }

    auto unit = avcUnit;
    const std::weak_ptr<AVCDiscovery> weakSelf = weak_from_this();
    const auto token = timerScheduler_.ScheduleAfter(
        static_cast<uint64_t>(kRescanDelayMs) * 1000000ULL,
        [weakSelf, route = *route, operationSerial, attempt, unit]() {
            const auto self = weakSelf.lock();
            if (!self || !self->IsRescanCurrent(route, operationSerial)) {
                return;
            }

            if (self->lock_) {
                IOLockLock(self->lock_);
                self->rescanTimersByGuid_.erase(route.guid);
                IOLockUnlock(self->lock_);
            }

            auto work = [weakSelf, route, operationSerial, attempt, unit]() {
                const auto self = weakSelf.lock();
                if (!self || !self->IsRescanCurrent(route, operationSerial)) {
                    return;
                }

                ASFW_LOG(Audio, "AVCDiscovery: Auto re-scan attempt %u for GUID=%llx", attempt, route.guid);
                unit->ReScan([weakSelf, route, operationSerial, unit](bool success) {
                    const auto self = weakSelf.lock();
                    if (!self || !self->IsRescanCurrent(route, operationSerial)) {
                        return;
                    }

                    if (!success) {
                        ASFW_LOG_ERROR(Audio,
                                       "AVCDiscovery: AVCUnit re-scan failed GUID=%llx",
                                       route.guid);
                        return;
                    }

                    self->HandleInitializedUnit(route.guid, unit);
                });
            };

            if (self->rescanQueue_) {
                self->rescanQueue_->DispatchAsync(^{ work(); });
            } else {
                work();
            }
        });

    if (lock_) {
        IOLockLock(lock_);
        rescanTimersByGuid_[guid] = token;
        IOLockUnlock(lock_);
    }
}

void AVCDiscovery::OnUnitSuspended(std::shared_ptr<Discovery::FWUnit> unit) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    uint64_t guid = GetUnitGUID(unit);

    IOLockLock(lock_);
    auto it = units_.find(guid);
    if (it != units_.end()) {
        os_log_info(log_,
                    "AVCDiscovery: AV/C unit suspended: GUID=%llx",
                    guid);
        // Unit remains in map but operations will fail until resumed
    }
    IOLockUnlock(lock_);

    // Rebuild node ID map (suspended units removed from routing)
    RebuildNodeIDMap();
}

void AVCDiscovery::OnUnitResumed(std::shared_ptr<Discovery::FWUnit> unit) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    uint64_t guid = GetUnitGUID(unit);

    std::shared_ptr<AVCUnit> avcUnit;
    IOLockLock(lock_);
    auto it = units_.find(guid);
    if (it != units_.end()) {
        avcUnit = it->second;
        os_log_info(log_,
                    "AVCDiscovery: AV/C unit resumed: GUID=%llx",
                    guid);
        // Unit is now available again
    }
    IOLockUnlock(lock_);

    if (avcUnit) {
        avcUnit->OnRouteRevalidated();
        // An attach that failed (a device that reset the bus mid-discovery, as a
        // crashing Phase 88 does) never published an audio device. Run it once
        // more now that the unit is back.
        if (avcUnit->GetDiscoveryStatus() == AVCDiscoveryStatus::Failed) {
            ASFW_LOG(AVC, "AVCDiscovery: unit resumed after a failed attach; rescanning GUID=%llx", guid);
            ScheduleRescan(guid, avcUnit);
        }
    }

    // Rebuild node ID map (resumed units back in routing)
    RebuildNodeIDMap();
}

void AVCDiscovery::OnUnitTerminated(std::shared_ptr<Discovery::FWUnit> unit) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    uint64_t guid = GetUnitGUID(unit);

    std::shared_ptr<AVCUnit> avcUnit;
    IOLockLock(lock_);

    auto it = units_.find(guid);
    if (it != units_.end()) {
        avcUnit = it->second;
        os_log_info(log_,
                    "AVCDiscovery: AV/C unit terminated: GUID=%llx",
                    guid);
        units_.erase(it);
    }
    rescanAttempts_.erase(guid);
    IOLockUnlock(lock_);

    // A response-router lease may keep the transport alive after its unit has
    // left discovery. Stop it explicitly so no pending callback survives the
    // unit-removal lifecycle boundary.
    if (avcUnit) {
        avcUnit->Shutdown();
    }

    // Rebuild node ID map (terminated unit removed)
    RebuildNodeIDMap();
}

void AVCDiscovery::OnDeviceAdded(std::shared_ptr<Discovery::FWDevice> device) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    PrepareMAudioBootloader(device);
}

void AVCDiscovery::OnDeviceResumed(std::shared_ptr<Discovery::FWDevice> device) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    PrepareMAudioBootloader(device);
}

void AVCDiscovery::PrepareMAudioBootloader(
    const std::shared_ptr<Discovery::FWDevice>& device) {
    if (!device) {
        return;
    }
    // The registry's resolved plan decides whether a cue applies; preparation
    // does not re-run catalog matching.
    const auto plan = CurrentPolicyPlan(deviceRegistry_, *device);
    if (!plan.has_value() ||
        !Bootloader::ShouldPrepareBootloader(*plan, device->GetVendorID(),
                                             device->GetModelID())) {
        return;
    }
    const auto route = deviceRegistry_.CurrentRoute(device->GetGUID());
    if (!route.has_value() || route->generation != device->GetGeneration() ||
        route->nodeId != device->GetNodeID() || !deviceRegistry_.IsCurrent(*route)) {
        ASFW_LOG_WARNING(AVC,
                         "MAudio boot cue skipped: no current device route GUID=0x%016llx",
                         device->GetGUID());
        return;
    }

    const bool started = bootloaderPreparation_.Prepare(
        *plan, device->GetVendorID(), device->GetModelID(), *route,
        busInfo_.GetSpeed(ASFW::FW::NodeId{static_cast<uint8_t>(route->nodeId)}),
        [weakSelf = weak_from_this()] {
            const auto self = weakSelf.lock();
            return self && !self->shuttingDown_.load(std::memory_order_acquire);
        });
    if (!started) {
        return;
    }

    ASFW_LOG(AVC,
             "MAudio 1814 bootloader identified; reading BootROM before guarded cue GUID=0x%016llx",
             route->guid);
}

void AVCDiscovery::OnDeviceSuspended(std::shared_ptr<Discovery::FWDevice> device) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    (void)device;
}

void AVCDiscovery::OnDeviceRemoved(Discovery::Guid64 guid) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    std::shared_ptr<AVCUnit> avcUnit;
    IOLockLock(lock_);

    const auto it = units_.find(guid);
    if (it != units_.end()) {
        avcUnit = it->second;
        units_.erase(it);
    }
    rescanAttempts_.erase(guid);
    IOLockUnlock(lock_);

    if (avcUnit) {
        avcUnit->Shutdown();
    }

    RebuildNodeIDMap();
}

//==============================================================================
// Public API
//==============================================================================

AVCUnit* AVCDiscovery::GetAVCUnit(uint64_t guid) {
    IOLockLock(lock_);

    auto it = units_.find(guid);
    AVCUnit* result = (it != units_.end()) ? it->second.get() : nullptr;

    IOLockUnlock(lock_);

    return result;
}

AVCUnit* AVCDiscovery::GetAVCUnit(std::shared_ptr<Discovery::FWUnit> unit) {
    if (!unit) {
        return nullptr;
    }

    uint64_t guid = GetUnitGUID(unit);
    return GetAVCUnit(guid);
}

std::vector<AVCUnit*> AVCDiscovery::GetAllAVCUnits() {
    IOLockLock(lock_);

    std::vector<AVCUnit*> result;
    result.reserve(units_.size());

    for (auto& [guid, avcUnit] : units_) {
        result.push_back(avcUnit.get());
    }

    IOLockUnlock(lock_);

    return result;
}

void AVCDiscovery::ReScanAllUnits() {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<std::pair<uint64_t, std::shared_ptr<AVCUnit>>> scanUnits;
    IOLockLock(lock_);
    scanUnits.reserve(units_.size());
    for (const auto& [guid, avcUnit] : units_) {
        if (avcUnit) scanUnits.emplace_back(guid, avcUnit);
    }
    rescanAttempts_.clear();
    IOLockUnlock(lock_);

    ASFW_LOG(AVC, "[AVCDiag] manual discovery requested units=%zu", scanUnits.size());
    // Each unit reruns its bring-up -- the same commands attach sends, the
    // extension inventory included -- into a fresh exchange log, so the report
    // shows the complete discovery. Devices whose policy forbids discovery
    // traffic are sent nothing and keep their existing log.
    std::vector<std::pair<uint64_t, std::shared_ptr<AVCUnit>>> eligible;
    for (const auto& [guid, avcUnit] : scanUnits) {
        const auto plan = CurrentPolicyPlan(deviceRegistry_, guid);
        const auto unit = avcUnit->GetFWUnit();
        const auto decision = unit ? ProbeDecisionFor(unit->GetUnitSpecID(), plan)
                                   : AvcProbeDecision::NoPolicy;
        if (decision != AvcProbeDecision::GenericDiscovery) {
            ASFW_LOG(AVC, "[AVCDiag] GUID=%llx skipped by probe policy decision=%u", guid,
                     static_cast<unsigned>(decision));
            avcUnit->MarkRescanSkipped();
            continue;
        }
        if (avcUnit->TryBeginRescan()) {
            avcUnit->BeginExchangeSession();
            eligible.emplace_back(guid, avcUnit);
        }
    }
    // Publish every unit's synchronous state before any async probe can finish.
    for (const auto& [guid, avcUnit] : eligible) {
        avcUnit->ReScanAlreadyBegun([avcUnit, guid](bool success) {
            ASFW_LOG(AVC, "[AVCDiag] GUID=%llx result=%{public}s", guid, success ? "completed" : "failed");
            // Per-unit status is finalized before this callback; no discovery lock is held.
        });
    }
}

FCPTransport* AVCDiscovery::GetFCPTransportForNodeID(uint16_t nodeID) {
    // Legacy borrowing API. New asynchronous callers must use Acquire...()
    // and retain the returned shared owner across their complete operation.
    const auto transport = AcquireFCPTransportForNodeID(nodeID);
    return transport.get();
}

std::shared_ptr<FCPTransport> AVCDiscovery::AcquireFCPTransportForNodeID(uint16_t nodeID) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return nullptr;
    }
    IOLockLock(lock_);

    // Normalize to node number (low 6 bits) to match map keys
    const uint16_t nodeNumber = static_cast<uint16_t>(nodeID & 0x3Fu);

    auto it = fcpTransportsByNodeID_.find(nodeNumber);
    std::shared_ptr<FCPTransport> result = (it != fcpTransportsByNodeID_.end())
                                                ? it->second
                                                : nullptr;

    IOLockUnlock(lock_);

    return result;
}

//==============================================================================
// Bus Reset Handling
//==============================================================================

void AVCDiscovery::OnBusReset(uint32_t newGeneration) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    os_log_info(log_,
                "AVCDiscovery: Bus reset (generation %u)",
                newGeneration);

    // ControllerCore invalidates DeviceRegistry routes before this callback.
    std::vector<Scheduling::TimerToken> cancelledRescans;

    // Notify all AVCUnits of bus reset
    IOLockLock(lock_);

    for (auto& [guid, avcUnit] : units_) {
        avcUnit->OnBusReset(newGeneration);
    }

    activeRescanSerialByGuid_.clear();
    for (const auto& [guid, token] : rescanTimersByGuid_) {
        (void)guid;
        if (token != Scheduling::kInvalidTimerToken) {
            cancelledRescans.push_back(token);
        }
    }
    rescanTimersByGuid_.clear();

    IOLockUnlock(lock_);

    for (const auto token : cancelledRescans) {
        timerScheduler_.Cancel(token);
    }

    // Rebuild node ID map (node IDs changed)
    RebuildNodeIDMap();
}

bool AVCDiscovery::IsRescanCurrent(const Discovery::DeviceRouteToken& route,
                                   uint64_t operationSerial) const noexcept {
    if (!route || operationSerial == 0 || shuttingDown_.load(std::memory_order_acquire) || !lock_) {
        return false;
    }

    IOLockLock(lock_);
    const auto it = activeRescanSerialByGuid_.find(route.guid);
    const bool active = it != activeRescanSerialByGuid_.end() && it->second == operationSerial;
    IOLockUnlock(lock_);
    return active && deviceRegistry_.IsCurrent(route);
}

//==============================================================================
// Private Helpers
//==============================================================================

bool AVCDiscovery::IsAVCUnit(std::shared_ptr<Discovery::FWUnit> unit) const {
    if (!unit) {
        return false;
    }

    return IsTa1394Unit(unit->GetUnitSpecID());
}

uint64_t AVCDiscovery::GetUnitGUID(std::shared_ptr<Discovery::FWUnit> unit) const {
    if (!unit) {
        return 0;
    }

    auto device = unit->GetDevice();
    if (!device) {
        return 0;
    }

    return device->GetGUID();
}

void AVCDiscovery::RebuildNodeIDMap() {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    IOLockLock(lock_);

    // Clear old mappings
    fcpTransportsByNodeID_.clear();

    // Rebuild from current units
    for (auto& [guid, avcUnit] : units_) {
        auto device = avcUnit->GetDevice();
        if (!device) {
            continue;  // Device destroyed
        }

        auto unit = avcUnit->GetFWUnit();
        if (!unit || !unit->IsReady()) {
            continue;  // Unit suspended or terminated
        }

        // Normalize to node number (low 6 bits) to tolerate full vs short IDs
        const uint16_t fullNodeID = device->GetNodeID();
        const uint16_t nodeNumber = static_cast<uint16_t>(fullNodeID & 0x3Fu);
        
        auto transport = avcUnit->GetFCPTransportShared();
        if (!transport) {
            continue;
        }
        fcpTransportsByNodeID_[nodeNumber] = std::move(transport);

        os_log_debug(log_,
                     "AVCDiscovery: Mapped fullNodeID=0x%04x (node=%u) → FCPTransport (GUID=%llx)",
                     fullNodeID, nodeNumber, guid);
    }

    IOLockUnlock(lock_);
}
